#pragma once

#ifndef I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#endif
#include <gnome-software.h>

G_BEGIN_DECLS

/* Body of the plugin's list_apps vfunc. @source_tag is the vfunc pointer,
 * kept a caller argument so the task tag still names the vfunc itself.
 *
 * Answers exactly three query shapes — alternate_of, is_installed == TRUE,
 * and a keyword search — and fails everything else synchronously with
 * G_IO_ERROR_NOT_SUPPORTED. */
void gs_modulix_list_apps_async(GsPlugin *plugin, GsAppQuery *query,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data, gpointer source_tag);

GsAppList *gs_modulix_list_apps_finish(GAsyncResult *result, GError **error);

G_END_DECLS
