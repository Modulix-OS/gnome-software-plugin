/**
 * @file gs-modulix-upgrade.h
 * @brief The Modulix OS release upgrade: GNOME Software's *distro upgrade*
 *   banner, backed by the `release.json` `mxpkgs` publishes.
 *
 * A Modulix release (`VERSION_ID` 0.1 → 0.2) is not a mechanism of its own.
 * Releases ride the same tracked branch as every other revision, so moving to
 * one is moving the flake inputs — exactly what gs-modulix-update.c's
 * "Modulix OS" row already does, automatically. What this file adds is the
 * *announcement*: a `GsUpgradeBanner` saying "Modulix OS 0.2 Available", whose
 * buttons merely anticipate, by hand, the `Daemon.UpdateSystem` calls the
 * automatic chain would have made anyway.
 *
 * The two faces cannot share one `GsApp`: `gs_upgrade_banner_refresh()`
 * (`src/gs-upgrade-banner.c`) `g_critical`s on any state outside
 * `{AVAILABLE, QUEUED_FOR_INSTALL, INSTALLING, DOWNLOADING, UPDATABLE,
 * PENDING_INSTALL}`, and `GS_APP_STATE_UPDATABLE_LIVE` — the state the update
 * row lives in — is not among them. They do share the daemon call, hence the
 * common in-flight flag (`gs_modulix_update_try_acquire()`).
 *
 * Mapping, all three implemented here:
 *
 * | Banner | GNOME Software job | Daemon call | Resulting state |
 * |---|---|---|---|
 * | display | `list_distro_upgrades_async` | `Store1.GetRemoteRelease` | `AVAILABLE` |
 * | "Download" | `download_upgrade_async` | `UpdateSystem("build")` | `UPDATABLE` |
 * | "Restart & Upgrade" | `trigger_upgrade_async` | `UpdateSystem("boot")` | `PENDING_INSTALL` |
 *
 * After a successful trigger **GNOME Software reboots by itself**
 * (`gs_utils_invoke_reboot_async()` in `upgrade_trigger_finished_cb`,
 * `src/gs-updates-page.c`), which is why `"boot"` is the only correct mode and
 * why nothing here restarts anything.
 *
 * Both action vfuncs are invoked on *every* plugin implementing them — the
 * jobs do no management-plugin filtering — so each starts by checking that the
 * app is ours and returns success untouched otherwise.
 */

#pragma once

#include <gnome-software.h>

G_BEGIN_DECLS

/**
 * @brief Records the running system's release, for later comparison against
 *   the one `mxpkgs` publishes.
 *
 * Reads `VERSION_ID` from `/etc/os-release` through `GsOsRelease`. Called once
 * from the plugin's `setup_async`, on the main thread, because the listing
 * runs on a worker and must not re-read a file per query.
 *
 * @pre Called once, before any gs_modulix_upgrade_list_async().
 * @post The version is memoized for the plugin's lifetime; a missing or
 *   unreadable `os-release` leaves it unset, which makes every later listing
 *   return empty rather than guess.
 */
void gs_modulix_upgrade_init(void);

/**
 * @brief Implements `GsPluginClass::list_distro_upgrades_async`.
 *
 * Reads `Store1.GetRemoteRelease` on a worker thread and returns at most one
 * app: the upstream release, when it is strictly newer than the running one
 * (`as_vercmp_simple`). Never fails — `gs_plugin_job_list_distro_upgrades`
 * discards *every* plugin's contribution as soon as one plugin errors, so an
 * unreachable daemon or an unknown release must travel as an empty list.
 *
 * @param plugin The Modulix plugin. Not NULL.
 * @param flags GNOME Software's listing flags; unused (the read is equally
 *   cheap interactive or not, being daemon-cached).
 * @param cancellable Optional; honoured between the read and the reply.
 * @param callback Completion callback.
 * @param user_data Data for @p callback.
 * @param source_tag Source tag set on the task.
 * @pre gs_modulix_upgrade_init() has run.
 * @post None beyond the returned list.
 */
void gs_modulix_upgrade_list_async(GsPlugin *plugin,
                                   GsPluginListDistroUpgradesFlags flags,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data, gpointer source_tag);

/**
 * @brief Finishes gs_modulix_upgrade_list_async().
 *
 * @param result The #GAsyncResult handed to the callback.
 * @param error Return location for a #GError.
 * @return (transfer full): the app list, empty when there is no upgrade to
 *   announce.
 */
GsAppList *gs_modulix_upgrade_list_finish(GAsyncResult *result, GError **error);

/**
 * @brief Implements `GsPluginClass::download_upgrade_async`: realises the new
 *   system closure without activating it.
 *
 * `UpdateSystem("build")` — the same mode the automatic chain's "download"
 * step uses. Nothing is written to the configuration repository and nothing is
 * activated, so a failure leaves the system exactly as it was.
 *
 * @param plugin The Modulix plugin. Not NULL.
 * @param app The app the job targets; ignored unless it is ours and its
 *   `modulix::kind` is `"upgrade"`.
 * @param flags Download flags; unused.
 * @param cancellable Optional; not honoured once the blocking call started
 *   (the daemon has no cancellation channel for a rebuild).
 * @param callback Completion callback.
 * @param user_data Data for @p callback.
 * @param source_tag Source tag set on the task.
 * @pre None.
 * @post On success @p app is `GS_APP_STATE_UPDATABLE`, which is what makes the
 *   banner offer "Restart & Upgrade"; on failure it returns to
 *   `GS_APP_STATE_AVAILABLE`. An app that is not ours is left untouched and
 *   the task succeeds.
 */
void gs_modulix_upgrade_download_async(GsPlugin *plugin, GsApp *app,
                                       GsPluginDownloadUpgradeFlags flags,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data,
                                       gpointer source_tag);

/**
 * @brief Finishes gs_modulix_upgrade_download_async().
 *
 * @param result The #GAsyncResult handed to the callback.
 * @param error Return location for a #GError.
 * @return TRUE on success, FALSE with @p error set otherwise.
 */
gboolean gs_modulix_upgrade_download_finish(GAsyncResult *result,
                                            GError **error);

/**
 * @brief Implements `GsPluginClass::trigger_upgrade_async`: prepares the new
 *   generation for the next boot.
 *
 * `UpdateSystem("boot")`. GNOME Software reboots on its own once this
 * succeeds, so this must not.
 *
 * @param plugin The Modulix plugin. Not NULL.
 * @param app The app the job targets; ignored unless it is ours and its
 *   `modulix::kind` is `"upgrade"`.
 * @param flags Trigger flags; unused.
 * @param cancellable Optional; not honoured once the blocking call started.
 * @param callback Completion callback.
 * @param user_data Data for @p callback.
 * @param source_tag Source tag set on the task.
 * @pre None.
 * @post On success @p app is `GS_APP_STATE_PENDING_INSTALL` and the
 *   `CheckUpdate` memo is retired (gs_modulix_update_forget_check()), so the
 *   "Modulix OS" update row stops offering the update this just prepared; on
 *   failure @p app returns to `GS_APP_STATE_UPDATABLE`.
 */
void gs_modulix_upgrade_trigger_async(GsPlugin *plugin, GsApp *app,
                                      GsPluginTriggerUpgradeFlags flags,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data, gpointer source_tag);

/**
 * @brief Finishes gs_modulix_upgrade_trigger_async().
 *
 * @param result The #GAsyncResult handed to the callback.
 * @param error Return location for a #GError.
 * @return TRUE on success, FALSE with @p error set otherwise.
 */
gboolean gs_modulix_upgrade_trigger_finish(GAsyncResult *result,
                                           GError **error);

G_END_DECLS
