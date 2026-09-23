/**
 * @file gs-modulix-app.h
 * @brief GsApp construction from Modulix `a{sv}` payloads, plus Modulix
 *        ownership/kind accessors.
 *
 * Declares the entry points that turn a Modulix `GVariant` app/plugin entry
 * into a `GsApp` for GNOME Software, and the small set of accessors the rest
 * of the plugin uses to recognize a `GsApp` this plugin produced and read
 * back its Modulix kind/name.
 *
 * The `GVariant` dicts consumed here come straight from `org.modulix.Store1`
 * (via `plugin/src/dbus/gs-modulix-store1.c`) — see
 * `AppEntry`/`PluginEntry`/`EnrichEntry` in
 * `modulix-daemon/src/store/entry.rs` for the field-by-field wire contract
 * (key name, `s`/`b`/`i`/`u` type, and whether it is always present or
 * omitted). The implementation file (`gs-modulix-app.c`) documents, per
 * setter, which keys it reads and how a missing/wrong-typed key is handled.
 */
#pragma once

#include <glib.h>
#include <gnome-software.h>

G_BEGIN_DECLS

/**
 * @brief Tells whether @p app was created by this Modulix plugin instance.
 *
 * @param app GsApp to inspect. Not NULL. Not mutated.
 * @param plugin GsPlugin whose ownership is being tested (the plugin passed
 *   to gs_app_set_management_plugin() when the app was built). Not NULL.
 * @pre None.
 * @post @p app is unmodified.
 * @return TRUE when @p app's management plugin is @p plugin (i.e. @p app
 *   was returned by gs_modulix_make_app_from_json() or
 *   gs_modulix_add_module_plugins() for this plugin instance); FALSE
 *   otherwise, including for an app owned by a different plugin (e.g.
 *   flatpak/packagekit).
 */
gboolean gs_modulix_app_is_ours(GsApp *app, GsPlugin *plugin);

/**
 * @brief Reads back the Modulix row kind stamped on @p app.
 *
 * @param app GsApp to inspect. Not NULL. Not mutated.
 * @pre None.
 * @post @p app is unmodified.
 * @return The `"modulix::kind"` object data set by
 *   gs_modulix_make_app_from_json() or gs_modulix_add_module_plugins():
 *   `"package"`, `"module"` or `"plugin"`. NULL when @p app carries no such
 *   data, i.e. it is not a Modulix app. Transfer-none: owned by @p app's
 *   object data, valid only as long as @p app is, must not be freed.
 */
const gchar *gs_modulix_app_kind(GsApp *app);

/**
 * @brief Reads back the Modulix install identifier stamped on @p app.
 *
 * @param app GsApp to inspect. Not NULL. Not mutated.
 * @pre None.
 * @post @p app is unmodified.
 * @return The `"modulix::name"` object data: the nix attribute for a
 *   package, the module name for a module, or `"<module>/<plugin>"` for a
 *   module-plugin addon (see gs_modulix_add_module_plugins()). NULL when
 *   @p app carries no such data, i.e. it is not a Modulix app.
 *   Transfer-none: owned by @p app's object data, valid only as long as
 *   @p app is, must not be freed.
 */
const gchar *gs_modulix_app_name(GsApp *app);

/**
 * @brief Builds (or reuses) the GsApp for one Modulix `a{sv}` app entry.
 *
 * Returns the GsApp for @p dict, from the plugin cache when one already
 * exists for this (kind, name) — callers must unref it either way.
 *
 * @p is_installed is only a fallback: the installed state normally comes
 * from the entry's own `installed` field, which the daemon stamps on every
 * path.
 *
 * @param dict `a{sv}` GVariant decoded from one element of the Modulix
 *   `aa{sv}` reply (or a bare dict on the single-entry callers). Read-only,
 *   borrowed for the duration of this call. Not NULL. See gs-modulix-app.c
 *   for the exact set of keys read and the behaviour on a missing/
 *   wrong-typed key.
 * @param plugin GsPlugin this app is attached to: used as the app's
 *   management plugin and as the key namespace for the plugin cache
 *   (gs_plugin_cache_lookup()/gs_plugin_cache_add()). Not NULL.
 * @param is_installed Fallback installed state, used only when @p dict has
 *   no `"installed"` key (a daemon predating that field). Ignored whenever
 *   `"installed"` is present in @p dict.
 * @pre None.
 * @post A GsApp exists in the plugin cache under key
 *   `"<kind>\x1f<name>"` for this (kind, name) pair, populated/refreshed
 *   from @p dict's fields.
 * @return A new reference to the GsApp for @p dict (transfer-full: the
 *   caller must g_object_unref() it), reused from the plugin cache across
 *   calls sharing the same (kind, name). NULL when @p dict has no non-empty
 *   `"name"` key — the row is dropped.
 */
GsApp *gs_modulix_make_app_from_variant(GVariant *dict, GsPlugin *plugin,
                                        gboolean is_installed);

/**
 * @brief Builds a sized remote GIcon for a Flathub/Flatpak icon URL.
 *
 * 256x256 remote icon for @p url (Flathub/Flatpak icon URL).
 *
 * @param url Icon URL (typically the Flathub/AppStream icon URL carried in
 *   a Modulix JSON entry's `"icon"` field, or an enrichment `"icon"` key).
 *   Not NULL.
 * @pre None.
 * @post None (no state beyond the returned object).
 * @return A new GIcon wrapping @p url via gs_remote_icon_new(), with both
 *   logical width and height forced to 128px (transfer-full: the caller
 *   owns the returned reference). Never NULL for a non-NULL @p url.
 */
GIcon *gs_modulix_remote_icon_new(const gchar *url);

/**
 * @brief Tells whether @p state is owned by an in-flight install/uninstall
 *        and must not be overwritten by a listing/refine refresh.
 *
 * @param state GsAppState to classify.
 * @pre None.
 * @post None (pure function).
 * @return TRUE for GS_APP_STATE_INSTALLING, GS_APP_STATE_REMOVING,
 *   GS_APP_STATE_QUEUED_FOR_INSTALL, GS_APP_STATE_PURCHASING or
 *   GS_APP_STATE_DOWNLOADING; FALSE for every other state.
 */
gboolean gs_modulix_state_is_transient(GsAppState state);

/**
 * @brief Appends every app of a Modulix `aa{sv}` array to @p list.
 *
 * `seen_ids` (nullable, gchar* -> unused) dedups by GsApp id across calls
 * sharing the same table: an entry whose id is already present is skipped,
 * and each added id is recorded. Pass NULL to disable (needed on the
 * alternate_of path, where every entry intentionally shares one id).
 *
 * @p is_installed is the fallback described on
 * gs_modulix_make_app_from_variant().
 *
 * @param list GsAppList to append newly-built apps to. Not NULL. Mutated:
 *   gains one GsApp per non-deduped, non-NULL-name entry of @p array.
 * @param array `aa{sv}` GVariant, as returned by a `gs_modulix_store1_*`
 *   call. Borrowed for the duration of this call. NULL is tolerated: nothing
 *   is appended (the daemon call already failed and logged its own
 *   warning — see `plugin/src/dbus/gs-modulix-store1.c`).
 * @param plugin GsPlugin forwarded to gs_modulix_make_app_from_variant() for
 *   every entry. Not NULL.
 * @param is_installed Fallback installed state forwarded to
 *   gs_modulix_make_app_from_variant() for every entry.
 * @param seen_ids Nullable GHashTable<owned gchar* id, unused> used to
 *   dedup by GsApp id across multiple calls sharing the same table: an
 *   entry whose id is already a member is skipped (its GsApp is unreffed
 *   and dropped), otherwise the id is duplicated with g_strdup() and added.
 *   Pass NULL to disable deduplication, required on the `alternate_of` path
 *   where every row intentionally shares one GsApp id. Mutated when
 *   non-NULL: gains one entry per newly-added app.
 * @pre @p seen_ids (when non-NULL) is a valid, live GHashTable.
 * @post @p list holds the appended apps; @p seen_ids (when given) holds
 *   every id added to @p list during this call, in addition to any it
 *   already held.
 * @return None.
 */
void gs_modulix_append_apps_from_variant(GsAppList *list, GVariant *array,
                                         GsPlugin *plugin,
                                         gboolean is_installed,
                                         GHashTable *seen_ids);

/**
 * @brief Fetches a module's plugin list and attaches it to @p module_app as
 *        ADDON GsApps.
 *
 * @param module_app The module's GsApp, previously built by
 *   gs_modulix_make_app_from_json() with kind `"module"`. Not NULL.
 *   Mutated: gains an ADDON list via gs_app_add_addons() on every call
 *   that finds at least one plugin — safe to call repeatedly, since
 *   gs_app_add_addons() de-dups by GsApp pointer
 *   (`gs_app_list_add_safe(…CHECK_FOR_DUPE)`).
 * @param module_name Nix module name used to fetch (or reuse from cache)
 *   the plugin list, and as the addon id prefix (`"<module_name>/<plugin
 *   name>"`). Not NULL.
 * @param plugin GsPlugin used as each addon's management plugin and as the
 *   plugin-cache namespace. Not NULL.
 * @pre None.
 * @post Zero or more addon GsApps exist in the plugin cache under key
 *   `"plugin\x1f<module_name>/<plugin name>"`, refreshed from the latest
 *   fetch; @p module_app carries them as addons once at most.
 * @return None. A failed or empty fetch (module_name has no plugins, or the
 *   client-side plugins cache fails) leaves @p module_app unchanged.
 */
void gs_modulix_add_module_plugins(GsApp *module_app, const gchar *module_name,
                                   GsPlugin *plugin);

/**
 * @brief Builds (or reuses) the addon GsApp for one Modulix `a{sv}` plugin
 *        entry.
 *
 * @param dict `a{sv}` GVariant decoded from one element of a Modulix plugin
 *   `aa{sv}` array (`ListModulePlugins` or `ListInstalledPlugins`).
 *   Read-only, borrowed for the duration of this call. Not NULL.
 * @param plugin GsPlugin this addon is attached to. Not NULL.
 * @param module_fallback Module key used when @p dict has no `"module"`
 *   member. Not NULL.
 * @pre None.
 * @post A GsApp exists in the plugin cache under key
 *   `"plugin\x1f<module>/<name>"`, populated/refreshed from @p dict's
 *   fields.
 * @return A new reference to the addon GsApp (transfer-full), reused from
 *   the plugin cache across calls sharing the same (module, name). NULL
 *   when @p dict has no non-empty `"name"` key.
 */
GsApp *gs_modulix_make_plugin_app_from_variant(GVariant *dict,
                                               GsPlugin *plugin,
                                               const gchar *module_fallback);

/**
 * @brief Appends every plugin decoded from a Modulix `aa{sv}` plugin array
 *        to @p list.
 *
 * @param list GsAppList to append to. Not NULL. Mutated: gains one addon
 *   GsApp per non-NULL-name entry of @p array.
 * @param array `aa{sv}` GVariant. Borrowed for the duration of this call.
 *   NULL is tolerated: nothing is appended.
 * @param plugin GsPlugin forwarded to
 *   gs_modulix_make_plugin_app_from_variant() for every entry. Not NULL.
 * @param module_fallback Forwarded to
 *   gs_modulix_make_plugin_app_from_variant() for every entry. Not NULL.
 * @pre None.
 * @post @p list holds the newly-built plugin apps.
 * @return None.
 */
void gs_modulix_append_plugin_apps_from_variant(GsAppList *list,
                                                GVariant *array,
                                                GsPlugin *plugin,
                                                const gchar *module_fallback);

/**
 * @brief Reads `meta`'s `"screenshots"` array and attaches AsScreenshot
 *        objects to @p app.
 *
 * @param app GsApp to attach screenshots to. Not NULL. Mutated: gains one
 *   AsScreenshot per well-formed element of `meta`'s `"screenshots"` value,
 *   unless it already has screenshots (see gs-modulix-app.c for the guard
 *   rationale and the exact shape read:
 *   `a(sba(suu))` — `(caption, is_default, [(url,width,height)])`).
 * @param meta `a{sv}` GVariant to read the `"screenshots"` member from (an
 *   enrichment payload). Not NULL, borrowed. Missing key, or a
 *   `"screenshots"` value of the wrong type, is tolerated: no screenshots
 *   are added.
 * @pre None.
 * @post @p app carries the parsed screenshots, or is unchanged if it
 *   already had any, or if `meta` had none/malformed ones.
 * @return None.
 */
void gs_modulix_add_app_screenshots(GsApp *app, GVariant *meta);

G_END_DECLS
