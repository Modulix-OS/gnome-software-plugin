#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

const gchar *gs_modulix_json_str(JsonObject *obj, const gchar *key);
gboolean     gs_modulix_json_bool(JsonObject *obj, const gchar *key);
gint         gs_modulix_json_int(JsonObject *obj, const gchar *key);

/* Load @json into @parser (reused across blobs by the caller) and return its
 * root object / array, or NULL when @json is NULL, malformed, or of the wrong
 * shape — every payload comes from the daemon through the store-client shim,
 * so a wrong shape is a bug elsewhere, not something to crash on. @ctx labels
 * the g_warning emitted on a parse error. */
JsonObject *gs_modulix_json_parse_object(JsonParser *parser, const gchar *json,
                                         const gchar *ctx);
JsonArray  *gs_modulix_json_parse_array(JsonParser *parser, const gchar *json,
                                        const gchar *ctx);

G_END_DECLS
