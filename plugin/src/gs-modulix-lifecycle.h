#pragma once

/**
 * @file gs-modulix-lifecycle.h
 * @brief Public API of the install/uninstall bodies backing the plugin's
 * `install_apps`/`uninstall_apps` vfuncs.
 *
 * Every operation here ends in one blocking call
 * (`plugin/src/dbus/gs-modulix-daemon1.h`) to `org.modulix.Daemon` over the
 * system bus. That call is a full NixOS rebuild carried out daemon-side: it is
 * polkit-gated (the authorisation prompt is raised by the daemon, not this
 * process) and, once authorised, blocks the calling thread for as long as
 * the rebuild takes — commonly minutes. Nothing is queued on this side: the
 * daemon serializes its own write commands, so each request goes straight out
 * on its own #GTask worker thread. See gs-modulix-lifecycle.c for the exact
 * `GsApp` state transitions and the cancellation semantics (an in-flight
 * rebuild cannot actually be interrupted).
 */

#ifndef I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#endif
#include <gnome-software.h>

G_BEGIN_DECLS

/**
 * @brief Body of the plugin's `install_apps` vfunc: installs every app in
 * @p list that this plugin owns.
 *
 * Apps not owned by this plugin (gs_modulix_app_is_ours() false) are
 * silently ignored. Owned apps are moved to %GS_APP_STATE_INSTALLING
 * immediately, on the calling (main) thread, before this function returns —
 * this is the only progress signal GNOME Software gets: there is no
 * percentage or step reporting during the daemon call itself
 * (`GsPluginProgressCallback` is accepted by the vfunc but never invoked
 * here). The actual install — one `InstallPackage` call for this request's
 * packages, one `InstallModule` call for its modules, then one
 * `InstallPlugin` call per plugin — runs on a #GTask worker thread and blocks
 * it for the whole NixOS rebuild. Authorisation is requested by polkit
 * daemon-side; a refusal is indistinguishable here from any other
 * daemon-side failure (see gs-modulix-lifecycle.c).
 *
 * @param plugin The owning #GsPlugin, used to filter @p list to apps this
 * plugin manages and as the #GTask source object — which refs it for the
 * operation's whole lifetime, so a plugin finalize cannot race the worker
 * thread. Must not be NULL.
 * @param list The apps GNOME Software wants installed. Only the subset this
 * plugin owns is acted upon; apps belonging to other plugins are left
 * untouched. Borrowed for the duration of this call only — the apps this
 * plugin keeps are individually ref'd before this function returns.
 * @param cancellable Accepted for API conformance and stored on the
 * underlying #GTask, but never consulted: cancelling it does not abort an
 * in-flight daemon call/rebuild, and does not prevent the state transition
 * from being applied. Because #GTask's check-cancellable default is left
 * enabled, cancelling only changes what gs_modulix_lifecycle_finish()
 * reports once the (unabortable) operation completes. May be NULL.
 * @param callback Invoked on the main thread once this request's daemon
 * calls have returned (success or failure) — potentially minutes later.
 * @param user_data Passed verbatim to @p callback.
 * @param source_tag The vfunc pointer (`gs_plugin_modulix_install_apps_async`),
 * recorded via g_task_set_source_tag() for diagnostics.
 *
 * @post For apps this plugin owns: state is %GS_APP_STATE_INSTALLING
 * immediately; on eventual success every such app is moved to
 * %GS_APP_STATE_INSTALLED, on failure back to %GS_APP_STATE_AVAILABLE —
 * including the apps whose kind was skipped after an earlier call failed, so
 * none is left stuck in the transient state. The result (and, on failure, a
 * %GS_PLUGIN_ERROR_FAILED #GError) is delivered through @p callback /
 * gs_modulix_lifecycle_finish(). If @p list contains no app this plugin owns,
 * @p callback fires almost immediately with success and no daemon call is
 * made.
 */
void gs_modulix_lifecycle_install_async(GsPlugin *plugin, GsAppList *list,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data,
                                        gpointer source_tag);

/**
 * @brief Body of the plugin's `uninstall_apps` vfunc: uninstalls every app in
 * @p list that this plugin owns.
 *
 * Mirrors gs_modulix_lifecycle_install_async() exactly, with the opposite
 * states and daemon calls: apps this plugin owns move to
 * %GS_APP_STATE_REMOVING immediately (main thread), the calls are
 * `UninstallPackage` / `UninstallModule` / `UninstallPlugin`, and each is
 * again a full, polkit-gated, blocking NixOS rebuild with no progress
 * reporting. See gs_modulix_lifecycle_install_async() for the full parameter
 * contract, cancellation semantics (an in-flight rebuild cannot be aborted),
 * and error surfacing.
 *
 * @param plugin The owning #GsPlugin; see gs_modulix_lifecycle_install_async().
 * @param list The apps GNOME Software wants uninstalled; only the subset this
 * plugin owns is acted upon.
 * @param cancellable Accepted but not consulted; see
 * gs_modulix_lifecycle_install_async(). May be NULL.
 * @param callback Invoked on the main thread once this request's daemon calls
 * have returned.
 * @param user_data Passed verbatim to @p callback.
 * @param source_tag The vfunc pointer (`gs_plugin_modulix_uninstall_apps_async`).
 *
 * @post For apps this plugin owns: state is %GS_APP_STATE_REMOVING
 * immediately; on eventual success every such app moves to
 * %GS_APP_STATE_AVAILABLE, on failure back to %GS_APP_STATE_INSTALLED.
 */
void gs_modulix_lifecycle_uninstall_async(GsPlugin *plugin, GsAppList *list,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data,
                                          gpointer source_tag);

/**
 * @brief Finish function for both gs_modulix_lifecycle_install_async() and
 * gs_modulix_lifecycle_uninstall_async().
 *
 * @param result The #GAsyncResult passed to the `GAsyncReadyCallback` given
 * to the matching `*_async` call. Must be the #GTask produced by that call.
 * @param error On failure, set to a %GS_PLUGIN_ERROR / %GS_PLUGIN_ERROR_FAILED
 * error with a generic "Modulix daemon call failed" message — GNOME Software
 * has no richer per-cause UI for this failure, though the actual reason
 * (D-Bus error, denied polkit authorisation, rebuild/transaction failure) is
 * logged by gs-modulix-lifecycle.c's call_names()/call_plugin(). If @p cancellable was
 * cancelled, a %G_IO_ERROR_CANCELLED error is reported instead (via #GTask's
 * default check-cancellable behaviour), even though the underlying daemon
 * call/rebuild was not actually interrupted and may still be running or may
 * already have applied its state change. May be NULL to ignore the error.
 *
 * @return TRUE if the operation succeeded, FALSE otherwise (with @p error
 * set, unless NULL).
 */
gboolean gs_modulix_lifecycle_finish(GAsyncResult *result, GError **error);

G_END_DECLS
