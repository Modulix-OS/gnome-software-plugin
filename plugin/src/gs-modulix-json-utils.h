#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

const gchar *gs_modulix_json_str(JsonObject *obj, const gchar *key);
gboolean     gs_modulix_json_bool(JsonObject *obj, const gchar *key);
gint         gs_modulix_json_int(JsonObject *obj, const gchar *key);

G_END_DECLS
