/**
 * @file theoldreader_source_feed.c  TheOldReader feed subscription routines
 * 
 * Copyright (C) 2008  Arnold Noronha <arnstein87@gmail.com>
 * Copyright (C) 2013  Lars Windolf <lars.lindner@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. 
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <glib.h>
#include <string.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include "common.h"
#include "debug.h"
#include "xml.h"

#include "feedlist.h"
#include "theoldreader_source.h"
#include "subscription.h"
#include "node.h"
#include "theoldreader_source_edit.h"
#include "metadata.h"
#include "db.h"
#include "item_state.h"

/**
 * This is identical to xpath_foreach_match, except that it takes the context
 * as parameter.
 */
static void
theoldreader_source_xpath_foreach_match (const gchar* expr, xmlXPathContextPtr xpathCtxt, xpathMatchFunc func, gpointer user_data) 
{
	xmlXPathObjectPtr xpathObj = NULL;
	xpathObj = xmlXPathEval ((xmlChar*)expr, xpathCtxt);
	
	if (xpathObj && xpathObj->nodesetval && xpathObj->nodesetval->nodeMax) {
		int	i;
		for (i = 0; i < xpathObj->nodesetval->nodeNr; i++) {
			(*func) (xpathObj->nodesetval->nodeTab[i], user_data);
			xpathObj->nodesetval->nodeTab[i] = NULL ;
		}
	}
	
	if (xpathObj)
		xmlXPathFreeObject (xpathObj);
}

void
theoldreader_source_migrate_node(nodePtr node) 
{
	/* scan the node for bad ID's, if so, brutally remove the node */
	itemSetPtr itemset = node_get_itemset (node);
	GList *iter = itemset->ids;
	for (; iter; iter = g_list_next (iter)) {
		itemPtr item = item_load (GPOINTER_TO_UINT (iter->data));
		if (item && item->sourceId) {
			if (!g_str_has_prefix(item->sourceId, "tag:google.com")) {
				debug1(DEBUG_UPDATE, "Item with sourceId [%s] will be deleted.", item->sourceId);
				db_item_remove(GPOINTER_TO_UINT(iter->data));
			} 
		}
		if (item) item_unload (item);
	}

	/* cleanup */
	itemset_free (itemset);
}

static void
theoldreader_source_xml_unlink_node (xmlNodePtr node, gpointer data) 
{
	xmlUnlinkNode (node);
	xmlFreeNode (node);
}

static itemPtr
theoldreader_source_load_item_from_sourceid (nodePtr node, gchar *sourceId, GHashTable *cache) 
{
	gpointer    ret = g_hash_table_lookup (cache, sourceId);
	itemSetPtr  itemset;
	int         num = g_hash_table_size (cache);
	GList       *iter; 
	itemPtr     item = NULL;

	if (ret)
		return item_load (GPOINTER_TO_UINT (ret));

	/* skip the top 'num' entries */
	itemset = node_get_itemset (node);
	iter = itemset->ids;
	while (num--) iter = g_list_next (iter);

	for (; iter; iter = g_list_next (iter)) {
		item = item_load (GPOINTER_TO_UINT (iter->data));
		if (item && item->sourceId) {
			/* save to cache */
			g_hash_table_insert (cache, g_strdup(item->sourceId), (gpointer) item->id);
			if (g_str_equal (item->sourceId, sourceId)) {
				itemset_free (itemset);
				return item;
			}
		}
		item_unload (item);
	}

	g_warning ("Could not find item for %s!", sourceId);
	itemset_free (itemset);
	return NULL;
}

static void
theoldreader_source_item_retrieve_status (const xmlNodePtr entry, subscriptionPtr subscription, GHashTable *cache)
{
	TheOldReaderSourcePtr gsource = (TheOldReaderSourcePtr) node_source_root_from_node (subscription->node)->data ;
	xmlNodePtr      xml;
	nodePtr         node = subscription->node;
	xmlChar         *id = NULL;
	gboolean        read = FALSE;

	xml = entry->children;
	g_assert (xml);

	/* Note: at the moment TheOldReader doesn't exposed a "starred" label
	   like Google Reader did. It also doesn't expose the like feature it
	   implements. Therefore we cannot sync the flagged state with 
	   TheOldReader. */

	for (xml = entry->children; xml; xml = xml->next) {
		if (g_str_equal (xml->name, "id"))
			id = xmlNodeGetContent (xml);

		if (g_str_equal (xml->name, "category")) {
			xmlChar* label = xmlGetProp (xml, "label");
			if (!label)
				continue;

			if (g_str_equal (label, "read"))
				read = TRUE;

			xmlFree (label);
		}
	}

	if (!id) {
		g_warning ("Skipping item without id in theoldreader_source_item_retrieve_status()!");
		return;
	}
	
	itemPtr item = theoldreader_source_load_item_from_sourceid (node, id, cache);
	if (item && item->sourceId) {
		if (g_str_equal (item->sourceId, id) && !theoldreader_source_edit_is_in_queue(gsource, id)) {
			
			if (item->readStatus != read)
				item_read_state_changed (item, read);
		}
	}
	if (item)
		item_unload (item);
	xmlFree (id);
}

static void
theoldreader_feed_subscription_process_update_result (subscriptionPtr subscription, const struct updateResult* const result, updateFlags flags)
{
	gchar 	*id;

	debug_start_measurement (DEBUG_UPDATE);

	/* Save old subscription metadata which contains "theoldreader-feed-id"
	   which is mission critical and the feed parser currently drops all
	   previous metadata :-( */
	id = g_strdup (metadata_list_get (subscription->metadata, "theoldreader-feed-id"));

	/* Always do standard feed parsing to get the items... */
	feed_get_subscription_type ()->process_update_result (subscription, result, flags);

	/* Set remote id again */
	metadata_list_set (&subscription->metadata, "theoldreader-feed-id", id);
	g_free (id);

	if (!result->data)
		return;
g_print("%s\n", result->data);	
	/* FIXME: The following workaround ensure that the code below,
	   that uses UI callbacks item_*_state_changed(), does not 
	   reset the newCount of the feed list (see SF #2666478)
	   by getting the newCount first and setting it again later. */
	guint newCount = feedlist_get_new_item_count ();

	xmlDocPtr doc = xml_parse (result->data, result->size, NULL);
	if (doc) {		
		xmlNodePtr root = xmlDocGetRootElement (doc);
		xmlNodePtr entry = root->children ; 
		GHashTable *cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

		while (entry) { 
			if (!g_str_equal (entry->name, "entry")) {
				entry = entry->next;
				continue; /* not an entry */
			}
			
			theoldreader_source_item_retrieve_status (entry, subscription, cache);
			entry = entry->next;
		}
		
		g_hash_table_unref (cache);
		xmlFreeDoc (doc);
	} else { 
		debug0 (DEBUG_UPDATE, "theoldreader_feed_subscription_process_update_result(): Couldn't parse XML!");
		g_warning ("theoldreader_feed_subscription_process_update_result(): Couldn't parse XML!");
	}

	// FIXME: part 2 of the newCount workaround
	feedlist_update_new_item_count (newCount);
	
	debug_end_measurement (DEBUG_UPDATE, "theoldreader_feed_subscription_process_update_result");
}

static gboolean
theoldreader_feed_subscription_prepare_update_request (subscriptionPtr subscription, 
                                                       struct updateRequest *request)
{
	debug0 (DEBUG_UPDATE, "preparing TheOldReader feed subscription for update");
	TheOldReaderSourcePtr source = (TheOldReaderSourcePtr) node_source_root_from_node (subscription->node)->data; 
	
	g_assert (source); 
	if (source->loginState == THEOLDREADER_SOURCE_STATE_NONE) { 
		subscription_update (node_source_root_from_node (subscription->node)->subscription, 0) ;
		return FALSE;
	}

	if (!metadata_list_get (subscription->metadata, "theoldreader-feed-id")) {
		g_warning ("Skipping TheOldReader feed '%s' (%s) without id!", subscription->source, subscription->node->id);
		return FALSE;
	}

	debug1 (DEBUG_UPDATE, "Setting cookies for a TheOldReader subscription '%s'", subscription->source);
	gchar* source_escaped = g_uri_escape_string(request->source, NULL, TRUE);
	gchar* newUrl = g_strdup_printf ("http://theoldreader.com/reader/atom/%s", metadata_list_get (subscription->metadata, "theoldreader-feed-id"));
	update_request_set_source (request, newUrl);
	g_free (newUrl);
	g_free (source_escaped);

	update_request_set_auth_value (request, source->authHeaderValue);
	return TRUE;
}

struct subscriptionType theOldReaderSourceFeedSubscriptionType = {
	theoldreader_feed_subscription_prepare_update_request,
	theoldreader_feed_subscription_process_update_result
};
