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
