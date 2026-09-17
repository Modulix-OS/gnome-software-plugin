#pragma once

#ifndef I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#endif
#include <gnome-software.h>

G_BEGIN_DECLS

/* Body of the plugin's refine vfunc. @source_tag is the vfunc pointer, kept a
 * caller argument so the task tag still names the vfunc itself.
 *
 * Returns without a thread hop when @require_flags asks for nothing we
 * handle. */
void gs_modulix_refine_async(GsPlugin *plugin, GsAppList *list,
                             GsPluginRefineRequireFlags require_flags,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data,
                             gpointer source_tag);

gboolean gs_modulix_refine_finish(GAsyncResult *result, GError **error);

G_END_DECLS
