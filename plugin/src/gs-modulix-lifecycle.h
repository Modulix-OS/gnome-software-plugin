#pragma once

#ifndef I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#endif
#include <gnome-software.h>

G_BEGIN_DECLS

/* Coalescing queue for install/uninstall daemon calls: at most one call is in
 * flight, and everything enqueued while it runs is merged into the next
 * single call. One instance per plugin, owned by it. */
typedef struct _GsModulixLifecycle GsModulixLifecycle;

GsModulixLifecycle *gs_modulix_lifecycle_new(void);

/* Safe at plugin finalize: a running worker holds a ref on the plugin, so it
 * cannot race this. */
void gs_modulix_lifecycle_free(GsModulixLifecycle *self);

/* Bodies of the install_apps / uninstall_apps vfuncs: enqueue the apps of
 * @list we manage, move them to their in-progress state right away (this runs
 * on the main thread), and complete @callback once the coalesced daemon call
 * carrying them has returned. @source_tag is the vfunc pointer. */
void gs_modulix_lifecycle_install_async(GsModulixLifecycle *self,
                                        GsPlugin *plugin, GsAppList *list,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data,
                                        gpointer source_tag);

void gs_modulix_lifecycle_uninstall_async(GsModulixLifecycle *self,
                                          GsPlugin *plugin, GsAppList *list,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data,
                                          gpointer source_tag);

gboolean gs_modulix_lifecycle_finish(GAsyncResult *result, GError **error);

G_END_DECLS
