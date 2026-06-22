#pragma once

#include <glib.h>
#include <gnome-software.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

GsApp *gs_modulix_make_app_from_json(JsonObject *obj, GsPlugin *plugin,
                                     gboolean is_installed);

void gs_modulix_append_apps_from_json(GsAppList *list, const gchar *json,
                                      GsPlugin *plugin, gboolean is_installed,
                                      gboolean with_plugins,
                                      JsonParser *parser);

void gs_modulix_add_module_plugins(GsApp *module_app, const gchar *module_name,
                                   GsPlugin *plugin);

void gs_modulix_add_app_screenshots(GsApp *app, JsonObject *meta);

G_END_DECLS
