/**
 * @file gs-modulix-update.h
 * @brief System-update integration: the synthetic "Modulix OS" GsApp, and the
 *   `refresh_metadata`/`update_apps` vfunc bodies.
 *
 * Modulix has no per-app updates of its own (nix packages and modules are
 * reinstalled, not updated, via the lifecycle queue in gs-modulix-lifecycle.c)
 * — this file covers the one thing that *is* an update: the NixOS system
 * itself, driven by `org.modulix.Store1.CheckUpdate`,
 * `org.modulix.Store1.ListOutdatedInputs` and
 * `org.modulix.Store1.RebootRequired` (reads) and
 * `org.modulix.Daemon.UpdateSystem` (write), all documented in CLAUDE.md's
 * D-Bus contract table.
 *
 * The first two reads answer different questions and cost accordingly.
 * `CheckUpdate` is the *search*: the daemon resolves a whole candidate
 * `flake.lock` (a full `nix flake update`, minutes) and keeps it in RAM, so
 * the following `UpdateSystem` applies exactly those revisions instead of
 * resolving them again. `ListOutdatedInputs` is the *display*: a cached,
 * row-by-row description of what that candidate changes. Only
 * gs_modulix_update_check() spends the first; everything else reads the
 * second.
 *
 * One synthetic GsApp ("org.modulix.ModulixOS", GS_APP_SPECIAL_KIND_OS_UPDATE)
 * represents the whole system: gs_modulix_update_get_app() builds/reuses it
 * from the plugin cache (same "one GsApp per key" rule as gs-modulix-app.c),
 * gs_modulix_update_sync() refreshes its dynamic fields from one
 * ListOutdatedInputs read, gs_modulix_update_check() does the same after a
 * live check, gs_modulix_update_list() is what the "is-for-update" branch of
 * gs-modulix-list.c calls, and the async pair below is what
 * gs-plugin-modulix.c wires onto `GsPluginClass::refresh_metadata_async`/
 * `update_apps_async`.
 *
 * GNOME Software applies an update in two successive jobs, a `NO_APPLY`
 * "download" then an apply, and the second one is only issued if the app is
 * still `GS_APP_STATE_UPDATABLE_LIVE` when the first finishes. That split is the
 * only thing the mode depends on: `NO_APPLY` maps to `"build"` (realise the
 * closure, activate nothing, keep the state), the apply job maps to `"boot"`.
 * `"switch"` is never requested from here any more — a click in the Updates page
 * now gets exactly the automatic behaviour, see update_mode_for_flags()
 * (`gs-modulix-update.c`) for why.
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
 * (`"org.modulix.ModulixOS"`), special-kind (`GS_APP_SPECIAL_KIND_OS_UPDATE` —
 * this is what makes GNOME Software's Updates page render it as the
 * system-update row), kind (`AS_COMPONENT_KIND_OPERATING_SYSTEM`, set *after*
 * the special kind, which forces GENERIC on its way through and would otherwise
 * warn; the final kind matters because `gs_app_is_updatable()` is
 * unconditionally TRUE for it, which is what keeps the row through
 * `filter_updatable_apps` while it sits in `GS_APP_STATE_PENDING_INSTALL`),
 * bundle-kind
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
 * transient, see gs_modulix_state_is_transient()), update-details-text (one line
 * per outdated input: `"<input> : <current_rev> → <new_rev> (<dd/mm/yyyy>)"`),
 * and update-version (ISO date of the most recently modified input).
 *
 * An app sitting in `GS_APP_STATE_PENDING_INSTALL` with nothing outdated is left
 * entirely alone: a completed `"boot"` update owns that state *and* the details
 * it left behind, and the daemon's outdated-inputs cache is empty by then, so
 * rewriting would blank the row that has to offer the restart. A *newly
 * discovered* update does take it over, and must: staying `PENDING_INSTALL`
 * would keep the app out of `_should_auto_update()` and strand the new update
 * unapplied behind a reboot that only applies the older one.
 *
 * The `force_outdated` this receives is the memoized `CheckUpdate` answer, and
 * that memo expires (10 minutes): an update applied outside GNOME Software
 * (`nixos-rebuild switch` from a terminal) would otherwise keep forcing a
 * detail-less row back on, and have the automatic monitor spend a real
 * multi-minute `UpdateSystem` on an update that no longer exists.
 *
 * @param plugin GsPlugin forwarded to gs_modulix_update_get_app(). Not NULL.
 * The row list is not the only input to the state: the memoized result of the
 * last gs_modulix_update_check() is folded in as `force_outdated` (while it is
 * fresh, see above), so an update whose lockfile diff moved no *direct* input —
 * `diff_locks()` in `modulix-core-utils` only walks the lockfile root's direct
 * inputs, so that means zero rows for a real update — still lands on
 * `GS_APP_STATE_UPDATABLE_LIVE` rather than being declared current.
 *
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
 * @brief Asks the daemon to look for a system update, and refreshes the
 *   "Modulix OS" GsApp from the answer.
 *
 * The "search for updates" path, as opposed to gs_modulix_update_sync()'s
 * "show what is already known": calls gs_modulix_store1_check_update(), which
 * resolves a candidate `flake.lock` daemon-side and parks it in RAM for the
 * next gs_modulix_daemon1_update_system() to write. The row-by-row detail is
 * then read back with gs_modulix_store1_list_outdated_inputs(), free at that
 * point since the check just refilled the daemon's cache from a local diff of
 * the two lockfiles.
 *
 * The boolean the check returns wins over the row list when deciding whether
 * the system is outdated: an update that moved no *direct* input yields an
 * empty detail list but is still a real update.
 *
 * @param plugin GsPlugin forwarded to gs_modulix_update_get_app(). Not NULL.
 * @pre Blocks the calling thread for a full `nix flake update` daemon-side —
 *   **minutes**, not milliseconds. Worker thread only, and only on an
 *   explicit refresh; listing the Updates page must keep using
 *   gs_modulix_update_sync().
 * @post The cached "Modulix OS" GsApp's dynamic fields reflect the check, and
 *   the daemon holds a candidate lockfile iff this returned TRUE. A TRUE answer
 *   also resets the phase to #GS_MODULIX_UPDATE_PHASE_IDLE: the new candidate
 *   obsoletes whatever a previous `"build"` realised.
 * @return TRUE when an update is available, FALSE when the system is current
 *   or the check failed (already logged, reported as "nothing to update").
 */
gboolean gs_modulix_update_check(GsPlugin *plugin);

/**
 * @brief Appends the "Modulix OS" GsApp to @p list, only when there is
 *   something to update.
 *
 * Calls gs_modulix_update_sync() with `force_refresh = FALSE` (the "is for
 * update" query the Updates page issues is not itself a user-triggered
 * refresh). The app is appended when that returned TRUE — which already covers
 * the "no direct input moved" case, since gs_modulix_update_sync() folds the
 * memoized gs_modulix_update_check() boolean in — and also when the app sits in
 * `GS_APP_STATE_PENDING_INSTALL`: a `"boot"` update already ran, the daemon's
 * outdated cache is empty by then, and the row still has to offer the restart.
 *
 * Returning an empty list stops GNOME Software's whole automatic chain:
 * `get_updates_finished_cb` treats zero apps as "no updates" and never reaches
 * the update jobs.
 *
 * @param plugin GsPlugin forwarded to gs_modulix_update_sync(). Not NULL.
 * @param list GsAppList to append to when applicable. Not NULL.
 * @pre Same threading precondition as gs_modulix_update_sync().
 * @post @p list gains the "Modulix OS" GsApp iff either condition above holds;
 *   otherwise unchanged.
 * @return None.
 */
void gs_modulix_update_list(GsPlugin *plugin, GsAppList *list);

/**
 * @brief Implementation of `GsPluginClass::refresh_metadata_async`/`_finish`:
 *   looks for a system update.
 *
 * This is GNOME Software's designated "go and check" hook — the Updates
 * page's Refresh button and gs-update-monitor.c's daily check — so it runs
 * gs_modulix_update_check(), the one path allowed to spend a full `nix flake
 * update`. Merely listing the Updates page goes through
 * gs_modulix_update_list() instead and stays on the daemon's cache.
 *
 * @param plugin The plugin instance. Not NULL.
 * @param cache_age_secs Unused: the check is authoritative and always live,
 *   and no in-tree GNOME Software caller passes the `0` that used to be this
 *   plugin's "force" marker anyway. The daemon's own cache is what absorbs
 *   repeated listings.
 * @param cancellable Cancellation token forwarded to the worker task, or
 *   NULL.
 * @param callback Invoked once the (worker-thread) check completes; result
 *   retrievable through gs_modulix_update_refresh_metadata_finish().
 * @param user_data Opaque pointer forwarded unchanged to @p callback.
 * @param source_tag Task source tag (the caller's own vfunc pointer).
 * @pre gs_modulix_bus_init() must have succeeded.
 * @post Runs gs_modulix_update_check() on a GLib worker-pool thread; on
 *   success the daemon holds a candidate lockfile for the next
 *   `UpdateSystem`. Always completes the task successfully: a failed daemon
 *   call is "no update available", never a job failure — refreshing metadata
 *   has no user-visible failure mode in this plugin.
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
 * Flag mapping (see CLAUDE.md "System updates"): `NO_APPLY` →
 * `UpdateSystem("build")`, which realises the new closure and activates
 * nothing, leaving the app `GS_APP_STATE_UPDATABLE_LIVE`; `INTERACTIVE`
 * without `NO_APPLY` → `UpdateSystem("switch")`, which activates the update on
 * the running system, final state `GS_APP_STATE_INSTALLED` plus
 * `GS_APP_QUIRK_NEEDS_REBOOT` only when `Store1.RebootRequired` says the
 * running kernel/modules/initrd no longer match; anything else →
 * `UpdateSystem("boot")`, final state `GS_APP_STATE_PENDING_INSTALL` plus
 * `GS_APP_QUIRK_NEEDS_REBOOT` unconditionally. A click's *download* job
 * carries `INTERACTIVE` and `NO_APPLY` together, so it still maps to
 * `"build"`: only its apply job switches (see update_mode_for_flags()).
 * `NO_DOWNLOAD` and `NO_APPLY` both set → task succeeds immediately, nothing to
 * do (unreachable in practice: gs-plugin-job-update-apps.c asserts against that
 * combination). `NO_DOWNLOAD` alone cannot be honoured — the daemon does not
 * separate download from apply once asked to apply — so it applies.
 *
 * Leaving the app `UPDATABLE_LIVE` after `"build"` is load-bearing, not
 * cosmetic: `_should_auto_update()` (`src/gs-update-monitor.c`) re-reads the
 * state when the `NO_APPLY` job completes and only issues the apply job for an
 * app still in that state. Applying from the `NO_APPLY` job instead — which this
 * plugin used to do, mapping it to `"boot"` — silently ended the sequence there.
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
 * @param flags Selects `build` vs `boot` vs no-op, per the mapping above.
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
 * @post On success: the app's state reflects the outcome, and its
 *   update-details/-version are kept — they still describe, respectively, what
 *   the pending apply and the next boot will activate. On failure: the app
 *   reverts to
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

/**
 * @brief Claims the process-wide "a system rebuild is running" flag.
 *
 * `Daemon.UpdateSystem` is serialized daemon-side by the build queue, so two
 * concurrent callers would not corrupt anything — they would simply block for
 * minutes each. This flag is what lets the second one fail fast instead.
 *
 * @pre None; safe from any thread.
 * @post On TRUE, the flag is held until gs_modulix_update_release() is called;
 *   on FALSE nothing changed.
 * @return TRUE when the caller now owns the flag, FALSE when a rebuild was
 *   already in flight.
 */
gboolean gs_modulix_update_try_acquire(void);

/**
 * @brief Releases the flag gs_modulix_update_try_acquire() granted.
 *
 * @pre The caller obtained the flag and has not released it yet.
 * @post Another caller may claim it.
 */
void gs_modulix_update_release(void);

G_END_DECLS
