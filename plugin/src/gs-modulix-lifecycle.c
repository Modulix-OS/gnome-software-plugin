/*
 * gs-modulix-lifecycle.c — install / uninstall, coalesced.
 *
 * GNOME Software can fire several install jobs in a row (a module and its
 * plugins, "install all updates", fast clicking). Each one is a polkit-gated
 * D-Bus call to org.modulix.Daemon, so they are queued instead: a single
 * worker thread drains the queue, merges everything pending into one call per
 * operation, and completes every queued task with the shared result.
 *
 * Failure detail stops at the shim: mx_store_* writes collapse every failure
 * mode (bus down, D-Bus error, denied polkit prompt, daemon-side transaction
 * failure) to NULL, so the whole batch gets one generic error.
 */

#include "gs-modulix-lifecycle.h"

#include "gs-modulix-app.h"
#include "gs-modulix-config.h"

#include "modulix-store-client.h"

struct _GsModulixLifecycle {
  GMutex lock;
  GPtrArray *pending; /* PendingOp*, free func = pending_op_free */
  gboolean in_flight; /* a daemon call is currently running */
};

/* ── operations ─────────────────────────────────────────────────────────── */

typedef gchar *(*ModulixNamesFn)(const gchar *const *names, guint n);
typedef gchar *(*ModulixPluginFn)(const gchar *module, const gchar *plugin);

typedef struct {
  ModulixNamesFn pkg_fn;
  ModulixNamesFn mod_fn;
  ModulixPluginFn plugin_fn;
  GsAppState in_progress;
  GsAppState on_success;
  GsAppState on_failure;
} LifecycleOps;

static const LifecycleOps install_ops = {
    .pkg_fn = mx_store_install_packages,
    .mod_fn = mx_store_install_modules,
    .plugin_fn = mx_store_install_plugin,
    .in_progress = GS_APP_STATE_INSTALLING,
    .on_success = GS_APP_STATE_INSTALLED,
    .on_failure = GS_APP_STATE_AVAILABLE,
};

static const LifecycleOps uninstall_ops = {
    .pkg_fn = mx_store_uninstall_packages,
    .mod_fn = mx_store_uninstall_modules,
    .plugin_fn = mx_store_uninstall_plugin,
    .in_progress = GS_APP_STATE_REMOVING,
    .on_success = GS_APP_STATE_AVAILABLE,
    .on_failure = GS_APP_STATE_INSTALLED,
};

/* One queued install/uninstall request: the caller's task, its apps, and which
 * operation (install vs uninstall) to run. */
typedef struct {
  GTask *task;
  GPtrArray *apps;         /* ref'd apps (from collect_owned_apps) */
  const LifecycleOps *ops; /* &install_ops or &uninstall_ops */
} PendingOp;

static void pending_op_free(gpointer data) {
  PendingOp *op = data;
  g_clear_object(&op->task);
  g_clear_pointer(&op->apps, g_ptr_array_unref);
  g_free(op);
}

/* ── daemon calls ───────────────────────────────────────────────────────── */

static gboolean app_is_kind(GsApp *app, const gchar *kind) {
  return g_strcmp0(gs_modulix_app_kind(app), kind) == 0;
}

static void set_group_state(GPtrArray *apps, const gchar *kind,
                            GsAppState state) {
  for (guint i = 0; i < apps->len; i++) {
    GsApp *app = g_ptr_array_index(apps, i);
    if (app_is_kind(app, kind))
      gs_app_set_state(app, state);
  }
}

/* The names of every @kind app of @apps, NULL-terminated; the strings are
 * borrowed from the apps. */
static GPtrArray *names_of_kind(GPtrArray *apps, const gchar *kind) {
  GPtrArray *names = g_ptr_array_new(); /* borrowed const gchar* */

  for (guint i = 0; i < apps->len; i++) {
    GsApp *app = g_ptr_array_index(apps, i);
    if (app_is_kind(app, kind))
      g_ptr_array_add(names, (gpointer)gs_modulix_app_name(app));
  }
  g_ptr_array_add(names, NULL);

  return names;
}

/* TRUE when the shim returned a status string, i.e. the daemon replied without
 * a D-Bus error. Frees the status. */
static gboolean status_ok(gchar *status) {
  if (status == NULL)
    return FALSE;
  mx_store_free_string(status);
  return TRUE;
}

/* One batched call for all @kind apps of @apps, then their success/failure
 * state. Nothing to do (and no call) when @apps holds no app of that kind. */
static gboolean call_for_kind(GPtrArray *apps, const gchar *kind,
                              ModulixNamesFn fn, const LifecycleOps *ops) {
  g_autoptr(GPtrArray) names = names_of_kind(apps, kind);
  guint n = names->len - 1; /* minus the NULL terminator */

  if (n == 0)
    return TRUE;

  gboolean ok = status_ok(fn((const gchar *const *)names->pdata, n));
  set_group_state(apps, kind, ok ? ops->on_success : ops->on_failure);
  return ok;
}

/* Module plugins are installed one call apiece — the daemon's Install/Uninstall
 * Plugin methods take a (module, plugin) pair, not a batch. */
static gboolean call_for_plugins(GPtrArray *apps, const LifecycleOps *ops) {
  gboolean ok = TRUE;

  for (guint i = 0; ok && i < apps->len; i++) {
    GsApp *app = g_ptr_array_index(apps, i);
    if (!app_is_kind(app, "plugin"))
      continue;

    const gchar *parent = g_object_get_data(G_OBJECT(app), "modulix::parent");
    const gchar *pname =
        g_object_get_data(G_OBJECT(app), "modulix::plugin_name");

    gboolean pok = status_ok(ops->plugin_fn(parent, pname));
    gs_app_set_state(app, pok ? ops->on_success : ops->on_failure);
    ok = pok;
  }

  return ok;
}

/* Runs the actual daemon calls for @apps under @ops. Pure worker: touches no
 * GTask. Sets each app's success/failure state. @error is filled with a
 * generic failure on the first mx_store_* call that returns NULL — the shim
 * doesn't carry structured D-Bus error detail back through its status
 * string, only success/failure. */
static gboolean lifecycle_execute(GPtrArray *apps, const LifecycleOps *ops,
                                  GError **error) {
  gboolean ok = TRUE;

#if MODULIX_ENABLE_PACKAGES
  if (ok)
    ok = call_for_kind(apps, "package", ops->pkg_fn, ops);
#endif

#if MODULIX_ENABLE_MODULES
  if (ok)
    ok = call_for_kind(apps, "module", ops->mod_fn, ops);
#endif

  if (ok)
    ok = call_for_plugins(apps, ops);

  if (!ok)
    g_set_error_literal(error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
                        "Modulix daemon call failed");

  return ok;
}

/* ── queue draining ─────────────────────────────────────────────────────── */

/* Merges every PendingOp of one operation (all install, or all uninstall) into
 * a single deduplicated app list, runs it once, and completes each op's task
 * with the shared result. */
static void run_group(GPtrArray *ops_list, const LifecycleOps *ops) {
  g_autoptr(GPtrArray) merged = g_ptr_array_new(); /* borrowed refs */
  g_autoptr(GHashTable) seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < ops_list->len; i++) {
    PendingOp *op = g_ptr_array_index(ops_list, i);
    for (guint j = 0; j < op->apps->len; j++) {
      GsApp *app = g_ptr_array_index(op->apps, j);
      /* dedup by (kind, name) so overlapping requests don't send an app twice;
       * \x1f separates the fields (absent from kinds and package names) */
      gchar *key = g_strdup_printf("%s\x1f%s", gs_modulix_app_kind(app) ?: "",
                                   gs_modulix_app_name(app) ?: "");
      if (g_hash_table_add(seen, key))
        g_ptr_array_add(merged, app);
    }
  }

  g_autoptr(GError) err = NULL;
  gboolean ok = lifecycle_execute(merged, ops, &err);

  for (guint i = 0; i < ops_list->len; i++) {
    PendingOp *op = g_ptr_array_index(ops_list, i);
    if (ok)
      g_task_return_boolean(op->task, TRUE);
    else
      g_task_return_error(op->task, g_error_copy(err));
  }
}

/* Splits a drained batch by operation and runs each group once. */
static void process_batch(GPtrArray *batch) {
  g_autoptr(GPtrArray) install = g_ptr_array_new();
  g_autoptr(GPtrArray) uninstall = g_ptr_array_new();

  for (guint i = 0; i < batch->len; i++) {
    PendingOp *op = g_ptr_array_index(batch, i);
    g_ptr_array_add(op->ops == &install_ops ? install : uninstall, op);
  }

  if (install->len > 0)
    run_group(install, &install_ops);
  if (uninstall->len > 0)
    run_group(uninstall, &uninstall_ops);
}

/* What the worker thread owns while it runs: the queue, and a ref on the
 * plugin that owns it (so the queue cannot be freed under the worker). */
typedef struct {
  GsModulixLifecycle *queue;
  GsPlugin *plugin;
} DrainCtx;

/* Single serialized worker: drains all pending ops, runs them (coalesced), then
 * loops if more piled up during the run. Exactly one instance is alive at a
 * time (guarded by `in_flight`). */
static gpointer drain_worker(gpointer ctx_ptr) {
  DrainCtx *ctx = ctx_ptr;
  GsModulixLifecycle *self = ctx->queue;

  for (;;) {
    g_mutex_lock(&self->lock);
    GPtrArray *batch = self->pending;
    self->pending = g_ptr_array_new_with_free_func(pending_op_free);
    g_mutex_unlock(&self->lock);

    process_batch(batch);
    g_ptr_array_unref(batch);

    g_mutex_lock(&self->lock);
    if (self->pending->len == 0) {
      self->in_flight = FALSE;
      g_mutex_unlock(&self->lock);
      break;
    }
    g_mutex_unlock(&self->lock);
  }

  g_object_unref(ctx->plugin);
  g_free(ctx);
  return NULL;
}

/* ── enqueue ────────────────────────────────────────────────────────────── */

static GPtrArray *collect_owned_apps(GsAppList *list, GsPlugin *plugin) {
  GPtrArray *apps = g_ptr_array_new_with_free_func(g_object_unref);

  for (guint i = 0; i < gs_app_list_length(list); i++) {
    GsApp *app = gs_app_list_index(list, i);
    if (gs_modulix_app_is_ours(app, plugin))
      g_ptr_array_add(apps, g_object_ref(app));
  }

  return apps;
}

static void enqueue_lifecycle(GsModulixLifecycle *self, GsPlugin *plugin,
                              GsAppList *list, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data,
                              gpointer source_tag, const LifecycleOps *ops) {
  GTask *task = g_task_new(plugin, cancellable, callback, user_data);
  GPtrArray *apps = collect_owned_apps(list, plugin);

  g_task_set_source_tag(task, source_tag);

  if (apps->len == 0) {
    g_ptr_array_unref(apps);
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
    return;
  }

  /* Reflect the in-progress state right away (this vfunc runs on the main
   * thread), even for apps that will be coalesced into a later daemon call. */
  for (guint i = 0; i < apps->len; i++)
    gs_app_set_state(g_ptr_array_index(apps, i), ops->in_progress);

  PendingOp *op = g_new0(PendingOp, 1);
  op->task = task; /* takes ownership of the g_task_new ref */
  op->apps = apps;
  op->ops = ops;

  g_mutex_lock(&self->lock);
  g_ptr_array_add(self->pending, op);
  gboolean start = !self->in_flight;
  if (start)
    self->in_flight = TRUE;
  g_mutex_unlock(&self->lock);

  /* Only the transition idle→busy spawns a worker; it keeps a plugin ref for
   * its lifetime and drains everything, so no second worker is ever started. */
  if (start) {
    DrainCtx *ctx = g_new0(DrainCtx, 1);
    ctx->queue = self;
    ctx->plugin = g_object_ref(plugin);
    g_thread_unref(g_thread_new("modulix-lifecycle", drain_worker, ctx));
  }
}

/* ── public API ─────────────────────────────────────────────────────────── */

GsModulixLifecycle *gs_modulix_lifecycle_new(void) {
  GsModulixLifecycle *self = g_new0(GsModulixLifecycle, 1);

  g_mutex_init(&self->lock);
  self->pending = g_ptr_array_new_with_free_func(pending_op_free);
  self->in_flight = FALSE;

  return self;
}

void gs_modulix_lifecycle_free(GsModulixLifecycle *self) {
  if (self == NULL)
    return;

  g_clear_pointer(&self->pending, g_ptr_array_unref);
  g_mutex_clear(&self->lock);
  g_free(self);
}

void gs_modulix_lifecycle_install_async(GsModulixLifecycle *self,
                                        GsPlugin *plugin, GsAppList *list,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data,
                                        gpointer source_tag) {
  enqueue_lifecycle(self, plugin, list, cancellable, callback, user_data,
                    source_tag, &install_ops);
}

void gs_modulix_lifecycle_uninstall_async(GsModulixLifecycle *self,
                                          GsPlugin *plugin, GsAppList *list,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data,
                                          gpointer source_tag) {
  enqueue_lifecycle(self, plugin, list, cancellable, callback, user_data,
                    source_tag, &uninstall_ops);
}

gboolean gs_modulix_lifecycle_finish(GAsyncResult *result, GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}
