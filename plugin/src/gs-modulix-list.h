/**
 * @file gs-modulix-list.h
 * @brief Public entry points for the plugin's `list_apps` vfunc body.
 *
 * Declares the async/finish pair `gs-plugin-modulix.c` wires onto
 * `GsPluginClass::list_apps_{async,finish}`. The implementation
 * (`gs-modulix-list.c`) answers exactly four `GsAppQuery` shapes —
 * `alternate_of`, `is_installed == TRUE`, `is_for_update == TRUE`, and a
 * keyword search — each by
 * issuing one or more blocking `org.modulix.Store1` reads (see
 * `plugin/src/dbus/gs-modulix-store1.h`) on a worker thread and turning
 * every `aa{sv}` reply into `GsApp`s via
 * `gs_modulix_append_apps_from_variant()` (`gs-modulix-app.h`).
 */

#pragma once

#ifndef I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#endif
#include <gnome-software.h>

G_BEGIN_DECLS

/**
 * @brief Body of the plugin's `list_apps` vfunc: search, installed listing,
 * or the details page's "Sources" (`alternate_of`) query.
 *
 * Inspects @p query and accepts exactly one of four shapes: `alternate_of`
 * set, `is_installed == TRUE`, `is_for_update == TRUE`, or a non-empty
 * keyword list — each maps to one or two `org.modulix.Store1` reads (or, for
 * `is_for_update`, one `ListOutdatedInputs` read via gs-modulix-update.c) run
 * on a worker thread
 * (`g_task_run_in_thread`), started from `gs-modulix-list.c`'s
 * `list_apps_thread()`. Any other shape (including @p query itself being
 * NULL, more than one of the three properties set, or none of them) is
 * rejected synchronously, before a thread is spawned, by completing the
 * task with `G_IO_ERROR_NOT_SUPPORTED`.
 *
 * @param plugin The `GsPlugin` instance driving this task; also the task's
 *   `GTask` source object. Borrowed — must outlive the async operation.
 * @param query The query to answer, or NULL (then nothing is supported and
 *   the task fails synchronously). Borrowed for the duration of this call
 *   only; any data needed later (keywords, `alternate_of` id, max results)
 *   is copied into the task's private `ListData` before returning.
 * @param cancellable Optional `GCancellable` to abort the worker-thread
 *   read(s) early, or NULL. Borrowed; the task holds its own reference for
 *   as long as it needs it.
 * @param callback Invoked on the thread-default main context once the task
 *   completes (success or error). Its result must be finalised with
 *   gs_modulix_list_apps_finish().
 * @param user_data Opaque pointer forwarded verbatim to @p callback.
 * @param source_tag Task source tag, expected to be the calling vfunc's own
 *   function pointer (`gs_plugin_modulix_list_apps_async` in
 *   `gs-plugin-modulix.c`) so `g_task_get_source_tag()` still names the
 *   vfunc itself rather than this helper.
 *
 * @pre `gs_modulix_bus_init()` has already succeeded (called from
 *   `gs_plugin_modulix_setup_async()`); this function does not check.
 * @post Exactly one of @p callback's `GAsyncReadyCallback` contract paths is
 *   taken: eventual success with a `GsAppList` retrievable via
 *   gs_modulix_list_apps_finish(), or a `GError` (typically
 *   `G_IO_ERROR_NOT_SUPPORTED` for an unsupported query, or a cancellation
 *   error — see gs_modulix_list_apps_finish() for what "failure" does and
 *   does not mean on the `alternate_of` path).
 */
void gs_modulix_list_apps_async(GsPlugin *plugin, GsAppQuery *query,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data, gpointer source_tag);

/**
 * @brief Finishes an operation started with gs_modulix_list_apps_async().
 *
 * Thin wrapper over `g_task_propagate_pointer()`.
 *
 * @param result The `GAsyncResult` passed to the `GAsyncReadyCallback`.
 * @param error Return location for a `GError`, or NULL to ignore errors.
 *   Set when the underlying `GTask` was completed with an error (unsupported
 *   query shape, or cancellation on every path except `alternate_of`, which
 *   never fails the task on cancellation — see gs_modulix_list_apps_async()).
 * @return A new `GsAppList`, transfer full — the caller owns it and must
 *   `g_object_unref()` it. NULL on error (@p error, if non-NULL, is set in
 *   that case). Note this is distinct from an *empty but non-NULL* list,
 *   which is the legitimate "no results" outcome for a search or an
 *   `alternate_of` id no Store1 read matched.
 */
GsAppList *gs_modulix_list_apps_finish(GAsyncResult *result, GError **error);

G_END_DECLS
