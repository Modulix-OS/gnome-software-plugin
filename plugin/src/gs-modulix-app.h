#pragma once

#include <glib.h>
#include <gnome-software.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* ─── GsApp ownership / Modulix metadata ────────────────────────────────────
 * Every GsApp we create carries "modulix::kind" ("package" | "module" |
 * "plugin") and "modulix::name" (the nix attribute or module name) as object
 * data — see gs_modulix_make_app_from_json(). Both accessors return NULL on
 * an app that is not ours. */

gboolean gs_modulix_app_is_ours(GsApp *app, GsPlugin *plugin);
const gchar *gs_modulix_app_kind(GsApp *app);
const gchar *gs_modulix_app_name(GsApp *app);

/* Returns the GsApp for @obj, from the plugin cache when one already exists
 * for this (kind, name) — callers must unref it either way.
 *
 * @is_installed is only a fallback: the installed state normally comes from
 * the entry's own `installed` field, which the daemon stamps on every path.
 */
GsApp *gs_modulix_make_app_from_json(JsonObject *obj, GsPlugin *plugin,
                                     gboolean is_installed);

/* 256x256 remote icon for @url (Flathub/Flatpak icon URL). */
GIcon *gs_modulix_remote_icon_new(const gchar *url);

/* `seen_ids` (nullable, gchar* -> unused) dedups by GsApp id across calls
 * sharing the same table: an entry whose id is already present is skipped,
 * and each added id is recorded. Pass NULL to disable (needed on the
 * alternate_of path, where every entry intentionally shares one id).
 *
 * @is_installed is the fallback described on
 * gs_modulix_make_app_from_json(). */
void gs_modulix_append_apps_from_json(GsAppList *list, const gchar *json,
                                      GsPlugin *plugin, gboolean is_installed,
                                      JsonParser *parser,
                                      GHashTable *seen_ids);

void gs_modulix_add_module_plugins(GsApp *module_app, const gchar *module_name,
                                   GsPlugin *plugin);

void gs_modulix_add_app_screenshots(GsApp *app, JsonObject *meta);

G_END_DECLS
