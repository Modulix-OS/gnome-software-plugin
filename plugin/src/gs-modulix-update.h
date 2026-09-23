/**
 * @file gs-modulix-update.h
 * @brief System-update integration: the synthetic "Modulix OS" GsApp, and the
 *   `refresh_metadata`/`update_apps` vfunc bodies.
 *
 * Modulix has no per-app updates of its own (nix packages and modules are
 * reinstalled, not updated, via the lifecycle queue in gs-modulix-lifecycle.c)
 * — this file covers the one thing that *is* an update: the NixOS system
 * itself, driven by `org.modulix.Store1.ListOutdatedInputs` (read) and
 * `org.modulix.Daemon.UpdateSystem` (write), both documented in CLAUDE.md's
 * D-Bus contract table.
 *
 * One synthetic GsApp ("org.modulix.ModulixOS", GS_APP_SPECIAL_KIND_OS_UPDATE)
 * represents the whole system: gs_modulix_update_get_app() builds/reuses it
 * from the plugin cache (same "one GsApp per key" rule as gs-modulix-app.c),
 * gs_modulix_update_sync() refreshes its dynamic fields from one
 * ListOutdatedInputs read, gs_modulix_update_list() is what the "is-for-update"
 * branch of gs-modulix-list.c calls, and the async pair below is what
 * gs-plugin-modulix.c wires onto `GsPluginClass::refresh_metadata_async`/
 * `update_apps_async`.
 */
#pragma once

#ifndef I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#endif
#include <gnome-software.h>

G_BEGIN_DECLS

/**
 * @brief Builds (or reuses, from the plugin cache) the synthetic "Modulix OS"
 *   GsApp and (re-)applies its static fields.
 *
 * Static fields set on every call (idempotent, cheap to repeat): id
 * (`"org.modulix.ModulixOS"`), kind (`AS_COMPONENT_KIND_OPERATING_SYSTEM`),
 * special-kind (`GS_APP_SPECIAL_KIND_OS_UPDATE` — this is what makes GNOME
 * Software's Updates page render it as the system-update row), bundle-kind
 * (`AS_BUNDLE_KIND_PACKAGE`, mandatory or the loader drops it), scope
 * (`AS_COMPONENT_SCOPE_SYSTEM`), management-plugin, translated name/summary,
 * size-download (`GS_SIZE_TYPE_UNKNOWABLE` — the daemon gives no size), and
 * (once, guarded by `gs_app_has_icons()`) a themed `"modulix-logo"` icon.
 * Object data `"modulix::kind"` = `"update"` is set so
 * gs-modulix-lifecycle.c's kind-filtered install/uninstall calls never match
 * this app.
 *
 * @param plugin GsPlugin used as management plugin and plugin-cache
 *   namespace. Not NULL.
 * @pre None.
 * @post The plugin cache holds this GsApp under key `"update\x1fmodulix-os"`.
 * @return (transfer full) A reference to the GsApp, reused across calls. The
 *   caller must g_object_unref() it. Never NULL.
 */
GsApp *gs_modulix_update_get_app(GsPlugin *plugin);

/**
 * @brief Reads `ListOutdatedInputs` and refreshes the "Modulix OS" GsApp's
 *   dynamic fields from the result.
 *
 * Dynamic fields, rewritten on every call regardless of whether they changed
 * (unlike GsApp metadata, gs_app_set_update_details_text()/
 * gs_app_set_update_version() are not write-once): state
 * (`GS_APP_STATE_UPDATABLE_LIVE` when at least one input is outdated, else
 * `GS_APP_STATE_INSTALLED` — skipped entirely when the app's current state is
 * transient, see gs_modulix_state_is_transient()), update-details-text (one
 * line per outdated input: `"<input> : <current_rev> → <new_rev>
 * (<dd/mm/yyyy>)"`), and update-version (ISO date of the most recently
 * modified input).
 *
 * @param plugin GsPlugin forwarded to gs_modulix_update_get_app(). Not NULL.
 * @param force_refresh Forwarded to gs_modulix_store1_list_outdated_inputs():
 *   FALSE serves the daemon's 1h cache, TRUE forces a live re-check (slow).
 * @pre None. Blocks the calling thread for the daemon round-trip — must not
 *   be called from the main thread.
 * @post The cached "Modulix OS" GsApp's dynamic fields reflect the read's
 *   result. A failed read (NULL from the daemon call, already logged there)
 *   is treated as "no outdated inputs", not an error.
 * @return TRUE if at least one input is outdated (the app should be shown as
 *   updatable); FALSE otherwise.
 */
gboolean gs_modulix_update_sync(GsPlugin *plugin, gboolean force_refresh);

/**
 * @brief Appends the "Modulix OS" GsApp to @p list, only when there is
 *   something to update.
 *
 * Calls gs_modulix_update_sync() with `force_refresh = FALSE` (the "is for
 * update" query the Updates page issues is not itself a user-triggered
 * refresh) and appends the app to @p list only when that returned TRUE.
 *
 * @param plugin GsPlugin forwarded to gs_modulix_update_sync(). Not NULL.
 * @param list GsAppList to append to when applicable. Not NULL.
 * @pre Same threading precondition as gs_modulix_update_sync().
 * @post @p list gains the "Modulix OS" GsApp iff the system has outdated
 *   inputs; otherwise unchanged.
 * @return None.
 */
void gs_modulix_update_list(GsPlugin *plugin, GsAppList *list);

/**
 * @brief Implementation of `GsPluginClass::refresh_metadata_async`/`_finish`:
 *   re-syncs the "Modulix OS" GsApp's outdated-inputs state.
 *
 * @param plugin The plugin instance. Not NULL.
 * @param cache_age_secs Forwarded by GNOME Software; `0` (an explicit,
 *   user-triggered refresh) maps to `force_refresh = TRUE` in
 *   gs_modulix_update_sync(), matching `ListOutdatedInputs`'s own documented
 *   `force_refresh` contract. Any other value maps to FALSE (serve the
 *   daemon's 1h cache).
 * @param cancellable Cancellation token forwarded to the worker task, or
 *   NULL.
 * @param callback Invoked once the (worker-thread) sync completes; result
 *   retrievable through gs_modulix_update_refresh_metadata_finish().
 * @param user_data Opaque pointer forwarded unchanged to @p callback.
 * @param source_tag Task source tag (the caller's own vfunc pointer).
 * @pre gs_modulix_bus_init() must have succeeded.
 * @post Runs gs_modulix_update_sync() on a GLib worker-pool thread. Always
 *   completes the task successfully: a failed daemon read is "no outdated
 *   inputs", never a job failure — refreshing metadata has no user-visible
 *   failure mode in this plugin.
 * @return None.
 */
void gs_modulix_update_refresh_metadata_async(GsPlugin *plugin,
                                              guint64 cache_age_secs,
                                              GCancellable *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data,
                                              gpointer source_tag);

/**
 * @brief Finishes gs_modulix_update_refresh_metadata_async().
 *
 * @param result The GAsyncResult of the async call.
 * @param error Set on failure (cancellation only); left untouched on
 *   success.
 * @return TRUE on success, FALSE with @p error set on cancellation.
 */
gboolean gs_modulix_update_refresh_metadata_finish(GAsyncResult *result,
                                                    GError **error);

/**
 * @brief Implementation of `GsPluginClass::update_apps_async`/`_finish`:
 *   applies the system update via `UpdateSystem`.
 *
 * Ignored @p list entries: only the "Modulix OS" GsApp (owned by @p plugin,
 * `modulix::kind` == `"update"`) is acted on; if @p list carries none, the
 * task succeeds immediately without touching the daemon.
 *
 * Flag mapping (see CLAUDE.md "System updates"): `NO_DOWNLOAD` and
 * `NO_APPLY` both set → task succeeds immediately, nothing to do. `NO_APPLY`
 * alone → `UpdateSystem("boot")`, final state `GS_APP_STATE_PENDING_INSTALL`.
 * Any other combination (including `NO_DOWNLOAD` alone, which cannot be
 * honoured — the daemon does not separate download from apply) →
 * `UpdateSystem("switch")`, final state `GS_APP_STATE_INSTALLED`.
 *
 * At most one update runs at a time process-wide: a second call while one is
 * already running fails immediately with `GS_PLUGIN_ERROR_FAILED`, without
 * touching the app's state, rather than queuing (unlike
 * gs-modulix-lifecycle.c's coalescing queue — a system update is a single
 * app, single call, not a batchable operation).
 *
 * @param plugin The plugin instance. Not NULL.
 * @param list Apps to update; only this plugin's "Modulix OS" app (if
 *   present) is acted on.
 * @param flags Selects `boot` vs `switch` vs no-op, per the mapping above.
 * @param progress_cb Called once with `GS_APP_PROGRESS_UNKNOWN` right before
 *   the daemon call starts (the daemon reports no granular progress). May be
 *   NULL.
 * @param progress_data Opaque pointer forwarded to @p progress_cb.
 * @param cancellable Cancellation token forwarded to the worker task, or
 *   NULL. Does not abort a rebuild the daemon already started.
 * @param callback Invoked once the (worker-thread) call completes; result
 *   retrievable through gs_modulix_update_apps_finish().
 * @param user_data Opaque pointer forwarded unchanged to @p callback.
 * @param source_tag Task source tag (the caller's own vfunc pointer).
 * @pre Main thread, daemon connection live.
 * @post On success: the app's state reflects the outcome and its
 *   update-details/-version are cleared (the daemon's own cache is already
 *   invalidated, see `invalidate_updates()`,
 *   `modulix-daemon/src/daemon/mod.rs`). On failure: the app reverts to
 *   `GS_APP_STATE_UPDATABLE_LIVE` and the task carries a
 *   `GS_PLUGIN_ERROR_FAILED` #GError (the daemon's real `GError` is logged,
 *   not surfaced further — same convention as gs-modulix-lifecycle.c).
 * @return None.
 */
void gs_modulix_update_apps_async(GsPlugin *plugin, GsAppList *list,
                                  GsPluginUpdateAppsFlags flags,
                                  GsPluginProgressCallback progress_cb,
                                  gpointer progress_data,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data, gpointer source_tag);

/**
 * @brief Finishes gs_modulix_update_apps_async().
 *
 * @param result The GAsyncResult of the async call.
 * @param error Set on failure (a failed `UpdateSystem` call, an update
 *   already in flight, or cancellation); left untouched on success.
 * @return TRUE when the update succeeded or there was nothing of ours to
 *   update; FALSE with @p error set otherwise.
 */
gboolean gs_modulix_update_apps_finish(GAsyncResult *result, GError **error);

G_END_DECLS
