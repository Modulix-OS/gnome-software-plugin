#pragma once

/**
 * @file gs-modulix-lifecycle.h
 * @brief Public API of the install/uninstall coalescing queue that backs the
 * plugin's `install_apps`/`uninstall_apps` vfuncs.
 *
 * Every operation here ends in one blocking `mx_store_*` call
 * (`modulix-store-client.h`) to `org.modulix.Daemon` over the system bus.
 * That call is a full NixOS rebuild carried out daemon-side: it is
 * polkit-gated (the authorisation prompt is raised by the daemon, not this
 * process) and, once authorised, blocks the calling thread for as long as
 * the rebuild takes — commonly minutes. See gs-modulix-lifecycle.c for the
 * queue/worker mechanics, the exact `GsApp` state transitions, and the
 * cancellation semantics (an in-flight rebuild cannot actually be
 * interrupted).
 */

#ifndef I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#endif
#include <gnome-software.h>

G_BEGIN_DECLS

/**
 * @brief Coalescing queue for install/uninstall daemon calls.
 *
 * At most one `mx_store_*` write call is in flight at a time; everything
 * enqueued (via gs_modulix_lifecycle_install_async() /
 * gs_modulix_lifecycle_uninstall_async()) while a call is running is merged
 * into the next single daemon call for that operation kind, deduplicated by
 * (kind, name). One instance is created per plugin instance and owned by it
 * (see `GsPluginModulix::lifecycle` in gs-plugin-modulix.c). Opaque; access
 * only through the `gs_modulix_lifecycle_*` functions declared below.
 */
typedef struct _GsModulixLifecycle GsModulixLifecycle;

/**
 * @brief Creates a new, empty lifecycle queue.
 *
 * @return A newly allocated #GsModulixLifecycle with no pending operations
 * and no worker running. Ownership passes to the caller, which must release
 * it with gs_modulix_lifecycle_free().
 */
GsModulixLifecycle *gs_modulix_lifecycle_new(void);

/**
 * @brief Destroys a lifecycle queue.
 *
 * @param self The queue to free, or NULL (then this is a no-op).
 *
 * @pre None beyond @p self being either NULL or a valid, still-owned
 * #GsModulixLifecycle.
 * @post @p self and everything it owns is freed. Safe to call at plugin
 * finalize even while a drain worker could still be running: the worker
 * holds its own reference to the owning #GsPlugin (see `DrainCtx` in
 * gs-modulix-lifecycle.c), so GObject finalize — and this call — cannot
 * happen while a worker is mid-drain.
 */
void gs_modulix_lifecycle_free(GsModulixLifecycle *self);

/**
 * @brief Body of the plugin's `install_apps` vfunc: enqueues an install of
 * every app in @p list that this plugin owns.
 *
 * Apps not owned by this plugin (gs_modulix_app_is_ours() false) are
 * silently ignored. Owned apps are moved to %GS_APP_STATE_INSTALLING
 * immediately, on the calling (main) thread, before this function returns —
 * this is the only progress signal GNOME Software gets: there is no
 * percentage or step reporting during the daemon call itself
 * (`GsPluginProgressCallback` is accepted by the vfunc but never invoked by
 * this queue). The actual install — one coalesced `mx_store_install_packages`
 * / `mx_store_install_modules` / `mx_store_install_plugin` call per app kind,
 * batched with whatever else is queued — runs on a private worker thread and
 * blocks it for the whole NixOS rebuild. Authorisation is requested by
 * polkit daemon-side; a refusal is indistinguishable here from any other
 * daemon-side failure (see gs-modulix-lifecycle.c).
 *
 * @param self The queue to enqueue onto. Must not be NULL.
 * @param plugin The owning #GsPlugin, used to filter @p list to apps this
 * plugin manages and as the #GTask source object. Must outlive the async
 * operation; a worker started by this call takes its own reference so it
 * survives even a plugin finalize (see gs_modulix_lifecycle_free()).
 * @param list The apps GNOME Software wants installed. Only the subset this
 * plugin owns is acted upon; apps belonging to other plugins are left
 * untouched. Borrowed for the duration of this call only — the apps this
 * plugin keeps are individually ref'd before this function returns.
 * @param cancellable Accepted for API conformance and stored on the
 * underlying #GTask, but never consulted by this queue: cancelling it does
 * not abort a queued or in-flight daemon call/rebuild, and does not prevent
 * the queued state transition from being applied. Because #GTask's
 * check-cancellable default is left enabled, cancelling only changes what
 * gs_modulix_lifecycle_finish() reports once the (unabortable) operation
 * completes. May be NULL.
 * @param callback Invoked on the main thread once the coalesced daemon call
 * that ends up carrying this batch's apps has returned (success or
 * failure) — potentially minutes later, and only after every other request
 * merged into the same batch has also settled.
 * @param user_data Passed verbatim to @p callback.
 * @param source_tag The vfunc pointer (`gs_plugin_modulix_install_apps_async`),
 * recorded via g_task_set_source_tag() for diagnostics.
 *
 * @post For apps this plugin owns: state is %GS_APP_STATE_INSTALLING
 * immediately; on eventual success every such app's kind-group is moved to
 * %GS_APP_STATE_INSTALLED, on failure back to %GS_APP_STATE_AVAILABLE. The
 * result (and, on failure, a %GS_PLUGIN_ERROR_FAILED #GError) is delivered
 * through @p callback / gs_modulix_lifecycle_finish(). If @p list contains no
 * app this plugin owns, @p callback fires almost immediately with success and
 * no daemon call is made.
 */
void gs_modulix_lifecycle_install_async(GsModulixLifecycle *self,
                                        GsPlugin *plugin, GsAppList *list,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data,
                                        gpointer source_tag);

/**
 * @brief Body of the plugin's `uninstall_apps` vfunc: enqueues an uninstall
 * of every app in @p list that this plugin owns.
 *
 * Mirrors gs_modulix_lifecycle_install_async() exactly, with the opposite
 * states and daemon calls: apps this plugin owns move to
 * %GS_APP_STATE_REMOVING immediately (main thread), the coalesced call uses
 * `mx_store_uninstall_packages` / `mx_store_uninstall_modules` /
 * `mx_store_uninstall_plugin`, and the daemon call is again a full,
 * polkit-gated, blocking NixOS rebuild with no progress reporting. See
 * gs_modulix_lifecycle_install_async() for the full parameter contract,
 * cancellation semantics (an in-flight rebuild cannot be aborted), and error
 * surfacing.
 *
 * @param self The queue to enqueue onto. Must not be NULL.
 * @param plugin The owning #GsPlugin; see gs_modulix_lifecycle_install_async().
 * @param list The apps GNOME Software wants uninstalled; only the subset this
 * plugin owns is acted upon.
 * @param cancellable Accepted but not consulted; see
 * gs_modulix_lifecycle_install_async(). May be NULL.
 * @param callback Invoked on the main thread once the coalesced daemon call
 * carrying this batch has returned.
 * @param user_data Passed verbatim to @p callback.
 * @param source_tag The vfunc pointer (`gs_plugin_modulix_uninstall_apps_async`).
 *
 * @post For apps this plugin owns: state is %GS_APP_STATE_REMOVING
 * immediately; on eventual success every such app's kind-group moves to
 * %GS_APP_STATE_AVAILABLE, on failure back to %GS_APP_STATE_INSTALLED.
 */
void gs_modulix_lifecycle_uninstall_async(GsModulixLifecycle *self,
                                          GsPlugin *plugin, GsAppList *list,
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
 * error with a generic "Modulix daemon call failed" message — the
 * `mx_store_*` shim collapses every daemon-side failure mode (bus down,
 * D-Bus error, denied polkit authorisation, rebuild/transaction failure) to
 * NULL, so no more specific reason is available here. If @p cancellable was
 * cancelled, a %G_IO_ERROR_CANCELLED error is reported instead (via #GTask's
 * default check-cancellable behaviour), even though the underlying daemon
 * call/rebuild was not actually interrupted and may still be running or may
 * already have applied its state change. May be NULL to ignore the error.
 *
 * @return TRUE if the coalesced operation carrying this task succeeded,
 * FALSE otherwise (with @p error set, unless NULL).
 */
gboolean gs_modulix_lifecycle_finish(GAsyncResult *result, GError **error);

G_END_DECLS
