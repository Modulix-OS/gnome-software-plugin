/**
 * @file gs-modulix-refine.h
 * @brief Public entry points for the Modulix plugin's `refine` vfunc body.
 *
 * Refine is GNOME Software's fill-in-the-missing-metadata pass over a
 * `GsAppList`: it is called often, always over a batch rather than one app
 * at a time (once per page load/refresh, over the whole result list), and
 * must fill in only the pieces of metadata named by
 * `GsPluginRefineRequireFlags` for the apps it owns, leaving anything else
 * untouched. See gs-modulix-refine.c's `MODULIX_REFINE_FLAGS_WE_HANDLE` for
 * exactly which flags this plugin honours; every other flag is ignored.
 */

#pragma once

#ifndef I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#endif
#include <gnome-software.h>

G_BEGIN_DECLS

/**
 * gs_modulix_refine_async:
 * @plugin: the Modulix `GsPlugin` instance. Borrowed: it is also the
 *   `GTask` source object and must outlive the asynchronous operation.
 * @list: (transfer none): the batch of `GsApp` to refine. A reference is
 *   taken for the duration of the operation (released when the internal
 *   task data is freed); the caller retains its own ownership of @list and
 *   must not assume it is otherwise mutated synchronously by this call.
 * @require_flags: bitmask of `GsPluginRefineRequireFlags` naming the
 *   metadata the caller wants filled in. Only DESCRIPTION, SCREENSHOTS,
 *   ADDONS, LICENSE and ICON are honoured (see
 *   `MODULIX_REFINE_FLAGS_WE_HANDLE` in gs-modulix-refine.c); every other
 *   bit is ignored. Regardless of which bits are set, an app not owned by
 *   this plugin (`gs_modulix_app_is_ours()`) is always skipped, and addons
 *   are only ever built for an app whose `modulix::kind` is `"module"`.
 * @cancellable: (nullable) (transfer none): cancellation token, checked once
 *   per app in the worker-thread loop. NULL means no cancellation support.
 * @callback: `GAsyncReadyCallback` invoked when the operation completes; the
 *   result is retrieved with gs_modulix_refine_finish().
 * @user_data: (nullable): opaque pointer passed through to @callback
 *   unchanged.
 * @source_tag: the vfunc pointer, kept a caller argument (rather than
 *   hardcoded) so the `GTask`'s source tag still names the vfunc itself
 *   even though the actual work happens in this helper.
 *
 * Asynchronous body of the plugin's `refine` vfunc. Fills in the metadata
 * named by @require_flags — addons, icon, description, license and
 * screenshots — for every app of @list this plugin manages, via two
 * batched prefetches (enrichment, and on the details page only, nixpkgs
 * licenses) run once for the whole list before the per-app pass, so the
 * per-app work itself never blocks on the network or the bus. A piece of
 * requested metadata that turns out to be unavailable (no Flathub
 * enrichment for an app with no `app_id`, no license found, …) is left
 * unset on the `GsApp` rather than reported as an error.
 *
 * @pre @plugin and @list are non-NULL and valid for the duration of the
 *   call.
 * @post Returns without spawning a worker thread, completing @callback
 *   synchronously within this call, when @require_flags carries none of the
 *   flags this plugin handles. Otherwise the per-app work (and both
 *   prefetches) run on a `GTask` worker thread; @callback fires on
 *   completion or on cancellation, in which case gs_modulix_refine_finish()
 *   reports a `G_IO_ERROR_CANCELLED` #GError.
 */
void gs_modulix_refine_async(GsPlugin *plugin, GsAppList *list,
                             GsPluginRefineRequireFlags require_flags,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data,
                             gpointer source_tag);

/**
 * gs_modulix_refine_finish:
 * @result: (transfer none): the `GAsyncResult` passed to the
 *   gs_modulix_refine_async() callback.
 * @error: (nullable): return location for a #GError. Set only when the
 *   operation was cancelled mid-batch (`G_IO_ERROR_CANCELLED`); left
 *   untouched on success. A missing piece of metadata on an individual app
 *   is never surfaced as an error here.
 *
 * Retrieves the result of gs_modulix_refine_async().
 *
 * @pre Called from (or after) the `GAsyncReadyCallback` that
 *   gs_modulix_refine_async() was given, at most once per @result.
 * @return %TRUE if the batch was processed to completion (even if some
 *   individual apps ended up with metadata left unset), %FALSE if the
 *   operation was cancelled, in which case @error (when non-NULL) is set.
 */
gboolean gs_modulix_refine_finish(GAsyncResult *result, GError **error);

G_END_DECLS
