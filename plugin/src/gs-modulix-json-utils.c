/**
 * @file gs-modulix-json-utils.c
 * @brief Implementation of the total JSON accessors; see the header for the
 *        contract of each one.
 */

#include "gs-modulix-json-utils.h"

const gchar *gs_modulix_json_str(JsonObject *obj, const gchar *key) {
  if (!json_object_has_member(obj, key))
    return "";
  JsonNode *node = json_object_get_member(obj, key);
  if (!JSON_NODE_HOLDS_VALUE(node) || JSON_NODE_HOLDS_NULL(node))
    return "";
  return json_node_get_string(node);
}

gboolean gs_modulix_json_bool(JsonObject *obj, const gchar *key) {
  if (!json_object_has_member(obj, key))
    return FALSE;
  JsonNode *node = json_object_get_member(obj, key);
  return JSON_NODE_HOLDS_VALUE(node) && json_node_get_boolean(node);
}

gint gs_modulix_json_int(JsonObject *obj, const gchar *key) {
  if (!json_object_has_member(obj, key))
    return 0;
  JsonNode *node = json_object_get_member(obj, key);
  return JSON_NODE_HOLDS_VALUE(node) ? (gint)json_node_get_int(node) : 0;
}

/**
 * @brief Loads a payload into the parser and hands back its root node, shared
 *        by both public parse helpers.
 *
 * @param parser JsonParser to decode into (transfer-none). Not NULL.
 * @param json Raw JSON text; NULL and the empty string are treated as "no
 *   payload" and produce NULL without a warning.
 * @param ctx Label naming the call site, used only in the parse-error warning.
 * @pre @p parser is a live JsonParser.
 * @post On success @p parser holds the new parse tree; on failure its previous
 *   content is left as json-glib leaves it after a failed load.
 * @return The root node, borrowed from @p parser (transfer-none), or NULL when
 *   there was no payload or it did not parse. The node's kind is not checked
 *   here — that is the caller's job.
 */
static JsonNode *parse_root(JsonParser *parser, const gchar *json,
                            const gchar *ctx) {
  g_autoptr(GError) err = NULL;

  if (json == NULL || *json == '\0')
    return NULL;
  if (!json_parser_load_from_data(parser, json, -1, &err)) {
    g_warning("[modulix] %s JSON parse: %s", ctx, err->message);
    return NULL;
  }
  return json_parser_get_root(parser);
}

JsonObject *gs_modulix_json_parse_object(JsonParser *parser, const gchar *json,
                                         const gchar *ctx) {
  JsonNode *root = parse_root(parser, json, ctx);
  return (root != NULL && JSON_NODE_HOLDS_OBJECT(root))
             ? json_node_get_object(root)
             : NULL;
}

JsonArray *gs_modulix_json_parse_array(JsonParser *parser, const gchar *json,
                                       const gchar *ctx) {
  JsonNode *root = parse_root(parser, json, ctx);
  return (root != NULL && JSON_NODE_HOLDS_ARRAY(root)) ? json_node_get_array(root)
                                                       : NULL;
}
