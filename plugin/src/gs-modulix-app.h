#pragma once

#include <glib.h>
#include <gnome-software.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* @label_variant: suffix the name by "(<pname>)" when the entry belongs to a
 * group of distinguishable variants. FALSE on the search path, where
 * `dedup_by_group` has already merged the variants. */
GsApp *gs_modulix_make_app_from_json(JsonObject *obj, GsPlugin *plugin,
                                     gboolean is_installed,
                                     gboolean label_variant);

/* 256x256 remote icon for @url (Flathub/Flatpak icon URL). */
GIcon *gs_modulix_remote_icon_new(const gchar *url);

/* `seen_ids` (nullable, gchar* -> unused) dedups by GsApp id across calls
 * sharing the same table: an entry whose id is already present is skipped,
 * and each added id is recorded. Pass NULL to disable (needed on the
 * alternate_of path, where every entry intentionally shares one id).
 * `label_variant` is forwarded to gs_modulix_make_app_from_json(). */
void gs_modulix_append_apps_from_json(GsAppList *list, const gchar *json,
                                      GsPlugin *plugin, gboolean is_installed,
                                      JsonParser *parser,
                                      GHashTable *seen_ids,
                                      gboolean label_variant);

void gs_modulix_add_module_plugins(GsApp *module_app, const gchar *module_name,
                                   GsPlugin *plugin);

void gs_modulix_add_app_screenshots(GsApp *app, JsonObject *meta);

G_END_DECLS
