/**
 * @file gs-modulix-lifecycle.c
 * @brief install / uninstall, coalesced.
 *
 * GNOME Software can fire several install jobs in a row (a module and its
 * plugins, "install all updates", fast clicking). Each one is a polkit-gated
 * D-Bus call to org.modulix.Daemon, so they are queued instead: a single
 * worker thread drains the queue, merges everything pending into one call per
 * operation, and completes every queued task with the shared result.
 *
 * Each `mx_store_*` write call this file makes is a full, synchronous NixOS
 * rebuild carried out by `mx-daemon` (see `modulix-daemon/src/command/`):
 * the daemon checks polkit authorisation before running the command
 * (`Command::execute`'s precondition, `modulix-daemon/src/command/mod.rs`),
 * then runs the corresponding blocking `modulix-core-utils` call via
 * `spawn_blocking` (see the `lifecycle_commands!` macro,
 * `modulix-daemon/src/command/lifecycle.rs`). The `mx_store_*` shim
 * (`modulix-store-client/src/ffi.rs`) uses a *blocking* zbus connection, so
 * the whole rebuild — commonly minutes — blocks whichever thread in this
 * process calls it; here that is always the private drain worker thread
 * (drain_worker()), never the main thread.
 *
 * Failure detail stops at the shim: mx_store_* writes collapse every failure
 * mode (bus down, D-Bus error, denied polkit prompt, daemon-side transaction
 * failure) to NULL, so the whole batch gets one generic
 * %GS_PLUGIN_ERROR_FAILED #GError (see lifecycle_execute()). A denied polkit
 * prompt is therefore reported to GNOME Software exactly like any other
 * daemon-side failure — there is no distinct "you refused authorisation"
 * path here. Separately, the daemon also emits a `CommandFailed` D-Bus signal
 * on failure, which `gs-modulix-failure-monitor.c` turns into a desktop
 * notification with a "View error" button (the Nix rebuild log); that is an
 * independent, main-thread-only reporting path and not wired into the
 * `GError`/`GTask` flow implemented in this file.
 *
 * No progress is reported to the UI while a call is in flight: the
 * `install_apps`/`uninstall_apps` vfuncs accept a `GsPluginProgressCallback`
 * (see gs-plugin-modulix.c) but this queue never invokes it. The only
 * feedback the UI gets before completion is the immediate state transition
 * applied in enqueue_lifecycle() (%GS_APP_STATE_INSTALLING /
 * %GS_APP_STATE_REMOVING), set on the main thread before the daemon call
 * even starts.
 *
 * Cancellation: the `GCancellable` passed in is stored on the #GTask but
 * never polled anywhere in this file — no code path checks
 * g_cancellable_is_cancelled() or connects to the cancellable. Once a batch
 * has been handed to lifecycle_execute(), the blocking `mx_store_*` call and
 * the rebuild it triggers run to completion regardless of cancellation; there
 * is no way to abort an in-flight rebuild from this process. Cancelling only
 * affects what gs_modulix_lifecycle_finish() reports afterwards
 * (%G_IO_ERROR_CANCELLED, via #GTask's default check-cancellable behaviour),
 * not whether the operation happened.
 */

#include "gs-modulix-lifecycle.h"

#include "gs-modulix-app.h"
#include "gs-modulix-config.h"
#include "gs-modulix-plugins-cache.h"

#include "modulix-store-client.h"

/**
 * @brief Instance data of the coalescing lifecycle queue.
 *
 * @var _GsModulixLifecycle::lock
 * Guards `pending` and `in_flight` against concurrent access from whichever
 * main-thread callers enqueue work (enqueue_lifecycle()) and the single
 * drain worker (drain_worker()) that runs concurrently with them.
 * @var _GsModulixLifecycle::pending
 * Queue of not-yet-started #PendingOp, owned, freed with pending_op_free().
 * Swapped out wholesale for a fresh empty array each time the worker takes a
 * batch to run (drain_worker()).
 * @var _GsModulixLifecycle::in_flight
 * TRUE while a drain worker thread is alive for this queue. Used to ensure
 * at most one worker runs at a time: only the transition FALSE→TRUE (done
 * under `lock`) spawns a new worker.
 */
struct _GsModulixLifecycle {
  GMutex lock;
  GPtrArray *pending; /* PendingOp*, free func = pending_op_free */
  gboolean in_flight; /* a daemon call is currently running */
};

/* ── operations ─────────────────────────────────────────────────────────── */

/**
 * @brief Signature shared by the batched `mx_store_{install,uninstall}_{packages,modules}`
 * entry points.
 *
 * @param names NULL-terminated array of package or module names to act on.
 * Borrowed for the duration of the call.
 * @param n Number of entries in @p names (excluding the NULL terminator).
 *
 * @return A newly allocated status string on success, which the caller must
 * free with `mx_store_free_string()`; NULL on any failure (bus unreachable,
 * D-Bus error, denied polkit authorisation, or daemon-side rebuild failure —
 * indistinguishable from one another at this level).
 */
typedef gchar *(*ModulixNamesFn)(const gchar *const *names, guint n);

/**
 * @brief Signature shared by `mx_store_{install,uninstall}_plugin`.
 *
 * @param module Module key owning the plugin. Borrowed for the duration of
 * the call.
 * @param plugin Plugin key within that module. Borrowed for the duration of
 * the call.
 *
 * @return A newly allocated status string on success, which the caller must
 * free with `mx_store_free_string()`; NULL on any failure, with the same
 * causes and the same loss of detail as #ModulixNamesFn.
 */
typedef gchar *(*ModulixPluginFn)(const gchar *module, const gchar *plugin);

/**
 * @brief Everything that differs between an install run and an uninstall run:
 * which `mx_store_*` symbols to call, and which #GsAppState an app should
 * carry before, on success, and on failure.
 *
 * @var LifecycleOps::pkg_fn
 * Batched call for apps of kind `"package"`; NULL-safe target of
 * #ModulixNamesFn (`mx_store_install_packages` / `mx_store_uninstall_packages`).
 * @var LifecycleOps::mod_fn
 * Batched call for apps of kind `"module"` (`mx_store_install_modules` /
 * `mx_store_uninstall_modules`).
 * @var LifecycleOps::plugin_fn
 * Per-app call for apps of kind `"plugin"` (`mx_store_install_plugin` /
 * `mx_store_uninstall_plugin`) — the daemon has no batched form for plugins.
 * @var LifecycleOps::in_progress
 * State applied immediately (main thread, in enqueue_lifecycle()) to every
 * owned app being queued, before any daemon call runs.
 * @var LifecycleOps::on_success
 * State applied to an app's kind-group once its `mx_store_*` call returns a
 * non-NULL status.
 * @var LifecycleOps::on_failure
 * State applied to an app's kind-group once its `mx_store_*` call returns
 * NULL — i.e. the state is rolled back to what it was before `in_progress`.
 */
typedef struct {
  ModulixNamesFn pkg_fn;
  ModulixNamesFn mod_fn;
  ModulixPluginFn plugin_fn;
  GsAppState in_progress;
  GsAppState on_success;
  GsAppState on_failure;
} LifecycleOps;

/**
 * @brief #LifecycleOps for an install run: `mx_store_install_*`,
 * %GS_APP_STATE_INSTALLING while in flight, %GS_APP_STATE_INSTALLED on
 * success, back to %GS_APP_STATE_AVAILABLE on failure.
 */
static const LifecycleOps install_ops = {
    .pkg_fn = mx_store_install_packages,
    .mod_fn = mx_store_install_modules,
    .plugin_fn = mx_store_install_plugin,
    .in_progress = GS_APP_STATE_INSTALLING,
    .on_success = GS_APP_STATE_INSTALLED,
    .on_failure = GS_APP_STATE_AVAILABLE,
};

/**
 * @brief #LifecycleOps for an uninstall run: `mx_store_uninstall_*`,
 * %GS_APP_STATE_REMOVING while in flight, %GS_APP_STATE_AVAILABLE on
 * success, back to %GS_APP_STATE_INSTALLED on failure.
 */
static const LifecycleOps uninstall_ops = {
    .pkg_fn = mx_store_uninstall_packages,
    .mod_fn = mx_store_uninstall_modules,
    .plugin_fn = mx_store_uninstall_plugin,
    .in_progress = GS_APP_STATE_REMOVING,
    .on_success = GS_APP_STATE_AVAILABLE,
    .on_failure = GS_APP_STATE_INSTALLED,
};

/**
 * @brief One queued install/uninstall request: the caller's task, its apps,
 * and which operation (install vs uninstall) to run.
 *
 * @var PendingOp::task
 * The #GTask returned to the vfunc caller. Owned; completed with the shared
 * batch result in run_group() and freed by pending_op_free().
 * @var PendingOp::apps
 * Ref'd #GsApp pointers this request covers (from collect_owned_apps()),
 * freed with `g_object_unref` via the array's free func.
 * @var PendingOp::ops
 * Points at the static #install_ops or #uninstall_ops; never owned/freed by
 * this struct.
 */
typedef struct {
  GTask *task;
  GPtrArray *apps;         /* ref'd apps (from collect_owned_apps) */
  const LifecycleOps *ops; /* &install_ops or &uninstall_ops */
} PendingOp;

/**
 * @brief #GDestroyNotify for #PendingOp, used as the free function of the
 * `pending`/batch #GPtrArray.
 *
 * @param data The #PendingOp to free, passed as `gpointer`. Must not be NULL
 * (callers are GLib container free-func invocations, which never pass NULL).
 *
 * @post @p data's `task` is unref'd, `apps` array is unref'd (dropping each
 * app's ref), and the #PendingOp struct itself is freed. Does not touch
 * `ops` (unowned, static storage).
 */
static void pending_op_free(gpointer data) {
  PendingOp *op = data;
  g_clear_object(&op->task);
  g_clear_pointer(&op->apps, g_ptr_array_unref);
  g_free(op);
}

/* ── daemon calls ───────────────────────────────────────────────────────── */

/**
 * @brief Tests whether @p app's `modulix::kind` metadata equals @p kind.
 *
 * @param app The app to inspect. Must be a valid #GsApp.
 * @param kind The kind string to compare against (`"package"`, `"module"`,
 * or `"plugin"`), or any other string — no validation is performed.
 *
 * @return TRUE if gs_modulix_app_kind(@p app) is non-NULL and equal to
 * @p kind, FALSE otherwise (including when @p app carries no Modulix kind at
 * all).
 */
static gboolean app_is_kind(GsApp *app, const gchar *kind) {
  return g_strcmp0(gs_modulix_app_kind(app), kind) == 0;
}

/**
 * @brief Sets @p state on every app of @p apps whose kind is @p kind.
 *
 * @param apps Array of #GsApp pointers to scan. Borrowed; not modified
 * structurally.
 * @param kind Kind to match (see app_is_kind()).
 * @param state State to apply via gs_app_set_state() to each matching app.
 *
 * @post Apps in @p apps not matching @p kind are left untouched. Apps whose
 * `gs_app_set_state` transition is a no-op or guarded (see
 * `state_is_transient` in gs-modulix-app.c) follow that function's own
 * semantics; this call does not special-case them.
 */
static void set_group_state(GPtrArray *apps, const gchar *kind,
                            GsAppState state) {
  for (guint i = 0; i < apps->len; i++) {
    GsApp *app = g_ptr_array_index(apps, i);
    if (app_is_kind(app, kind))
      gs_app_set_state(app, state);
  }
}

/**
 * @brief Collects the names of every @p kind app in @p apps into a
 * NULL-terminated array, ready for a #ModulixNamesFn call.
 *
 * @param apps Array of #GsApp pointers to scan. Borrowed.
 * @param kind Kind to match (see app_is_kind()).
 *
 * @return A newly allocated #GPtrArray of `const gchar *`, one entry per
 * matching app plus a trailing NULL terminator; the strings themselves are
 * borrowed from the apps (via gs_modulix_app_name()) and must not be freed —
 * only the array (`g_ptr_array_unref` / `g_autoptr`) is owned by the caller.
 * Contains just the NULL terminator when no app of @p kind is present.
 */
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

/**
 * @brief Tells whether an `mx_store_*` write call succeeded, and frees its
 * result either way.
 *
 * @param status The string returned by an `mx_store_install_*` /
 * `mx_store_uninstall_*` call: NULL on failure (bus down, D-Bus error, denied
 * polkit authorisation, or daemon-side rebuild failure — this string carries
 * no way to tell which), or a non-NULL status string on success. Ownership
 * transfers to this function either way.
 *
 * @return TRUE when @p status was non-NULL (the daemon replied without a
 * D-Bus error, i.e. the rebuild it performed succeeded), FALSE when it was
 * NULL.
 * @post @p status is freed with `mx_store_free_string()` when non-NULL; the
 * caller must not use or free it afterwards either way.
 */
static gboolean status_ok(gchar *status) {
  if (status == NULL)
    return FALSE;
  mx_store_free_string(status);
  return TRUE;
}

/**
 * @brief Runs one batched `mx_store_*` call for every @p kind app in @p apps,
 * then applies the resulting success/failure #GsAppState to that group.
 *
 * This is the call that performs the blocking NixOS rebuild for package and
 * module operations (`MODULIX_ENABLE_PACKAGES`/`MODULIX_ENABLE_MODULES`
 * gated in lifecycle_execute()): @p fn blocks the calling thread —
 * drain_worker(), never the main thread — for as long as the daemon takes to
 * authorise (polkit, daemon-side) and apply the change.
 *
 * @param apps Full merged batch of apps for this run (all kinds mixed). Only
 * the @p kind subset is read or have their state changed.
 * @param kind Kind to filter on (`"package"` or `"module"`).
 * @param fn The batched daemon-call function to invoke for this kind's names
 * (`ops->pkg_fn` or `ops->mod_fn`).
 * @param ops Supplies the success/failure #GsAppState to apply
 * (`ops->on_success` / `ops->on_failure`); `ops->in_progress` is not touched
 * here (already applied earlier, in enqueue_lifecycle()).
 *
 * @return TRUE if there was nothing of @p kind to do, or the call succeeded;
 * FALSE if the call was made and failed.
 * @post On a non-empty subset, every @p kind app in @p apps has its state set
 * to @p ops's `on_success` or `on_failure`, matching the call's outcome. When
 * no app of @p kind is present, no daemon call is made and no state changes.
 */
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

/**
 * @brief Runs one `mx_store_*` plugin call per `"plugin"`-kind app in
 * @p apps, stopping at the first failure.
 *
 * Module plugins are installed one call apiece — the daemon's
 * Install/Uninstall Plugin methods take a (module, plugin) pair, not a
 * batch — so, unlike call_for_kind(), each app here triggers its own
 * blocking, polkit-gated NixOS rebuild in sequence on the calling
 * (drain_worker()) thread.
 *
 * @param apps Full merged batch of apps for this run. Only `"plugin"`-kind
 * apps are read or have their state changed; each one's parent module and
 * plugin name are read from its `"modulix::parent"` /
 * `"modulix::plugin_name"` object data (set when the addon `GsApp` was
 * created — see gs-modulix-app.c).
 * @param ops Supplies `ops->plugin_fn` (the daemon call to make) and the
 * success/failure #GsAppState to apply.
 *
 * @return TRUE if every plugin call succeeded (or there were none); FALSE at
 * the first plugin call that failed — apps after that point in @p apps are
 * left as they were (neither called nor state-changed), since the loop
 * condition stops iterating on failure.
 * @post Each processed plugin app's state is set to @p ops's `on_success` or
 * `on_failure` per its own call's outcome. On success for a given plugin,
 * gs_modulix_plugins_cache_invalidate() is called for its parent module, so a
 * later refine of that module re-fetches its plugin list instead of serving
 * the now-stale `installed` bit cached before this call.
 */
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
    /* The plugins-list cache has no TTL of its own (see
     * gs-modulix-plugins-cache.c): without this, the JSON fetched before
     * this install/uninstall — its `installed` bit necessarily stale now —
     * would keep being served for the rest of the process's life. */
    if (pok)
      gs_modulix_plugins_cache_invalidate(parent);
    ok = pok;
  }

  return ok;
}

/**
 * @brief Runs the actual daemon calls for @p apps under @p ops: one batched
 * package call, one batched module call, then one call per plugin, stopping
 * at the first failure. Pure worker: touches no #GTask.
 *
 * Every call this function makes (through call_for_kind() /
 * call_for_plugins()) is a blocking, polkit-gated NixOS rebuild on
 * `mx-daemon`; this function itself blocks — synchronously, on whatever
 * thread calls it (always drain_worker() in this file) — for the sum of
 * however many of those calls it ends up making, easily minutes.
 *
 * @param apps Merged, deduplicated batch of apps to act on (all kinds).
 * Borrowed; each app's #GsAppState is mutated in place by the called
 * functions.
 * @param ops Which operation to run: #install_ops or #uninstall_ops.
 * @param error On failure, set to a %GS_PLUGIN_ERROR_FAILED #GError with a
 * generic "Modulix daemon call failed" message — the shim doesn't carry
 * structured D-Bus/polkit error detail back through its status string, only
 * success/failure, so no more specific reason (including "authorisation was
 * refused") can be reported here. Left untouched on success. May be NULL to
 * ignore the error (not done by this file's own callers, which always pass a
 * live `GError **`).
 *
 * @return TRUE if every call made succeeded (or a given kind/plugin had
 * nothing to do); FALSE at the first call that failed, at which point
 * remaining kinds/plugins are skipped (package before module before
 * plugins, per the `#if`/`if (ok)` chain below) and their apps keep the
 * `in_progress` state already applied in enqueue_lifecycle() — a state
 * inconsistent with the reported failure.
 * @post Package and module handling is compiled out entirely when
 * %MODULIX_ENABLE_PACKAGES / %MODULIX_ENABLE_MODULES (gs-modulix-config.h)
 * is FALSE, in which case apps of that kind are silently skipped rather than
 * causing a failure.
 */
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

/**
 * @brief Queues one install or uninstall batch and makes sure a worker is
 *        draining the queue.
 *
 * @param self The lifecycle queue. Not NULL.
 * @param plugin Plugin the apps belong to; reffed for the worker's lifetime
 *   when this call starts one. Not NULL.
 * @param list Apps the caller asked to operate on; only the ones this plugin
 *   owns are kept.
 * @param cancellable Cancellable of the GTask, or NULL.
 * @param callback Completion callback of the GTask, or NULL.
 * @param user_data Data passed to @p callback.
 * @param source_tag Source tag set on the GTask, used by the caller's `finish`
 *   to recognise its own result.
 * @param ops Operation table selecting install or uninstall behaviour,
 *   including the in-progress state to display. Not NULL.
 * @pre Main thread: the app states are set here, synchronously.
 * @post Every owned app is switched to the operation's in-progress state
 *   immediately, even one that will only be handled by a later daemon call. A
 *   batch with no owned app completes successfully at once, without touching
 *   the daemon. Exactly one worker thread drains the queue: the idle→busy
 *   transition starts it, and a second concurrent call only appends to the
 *   queue. The GTask completes from the worker, once the rebuild is over.
 * @return None.
 */
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

/**
 * @brief Releases the lifecycle queue.
 *
 * @param self The queue, or NULL (then nothing happens).
 * @pre No operation must still be in flight: the draining worker holds a raw
 *   pointer to @p self, so freeing it while a rebuild is running is undefined
 *   behaviour.
 * @post The pending queue and its tasks are freed and the mutex destroyed;
 *   @p self is dangling.
 * @return None.
 */
void gs_modulix_lifecycle_free(GsModulixLifecycle *self) {
  if (self == NULL)
    return;

  g_clear_pointer(&self->pending, g_ptr_array_unref);
  g_mutex_clear(&self->lock);
  g_free(self);
}

/**
 * @brief Starts installing every app of @p list this plugin owns.
 *
 * @param self The lifecycle queue. Not NULL.
 * @param plugin Plugin owning the apps. Not NULL.
 * @param list Apps to install; foreign apps are ignored.
 * @param cancellable Cancellable of the operation, or NULL. Cancelling stops
 *   the plugin from waiting, it does not abort a rebuild the daemon already
 *   started.
 * @param callback Called when the batch completes, or NULL.
 * @param user_data Data passed to @p callback.
 * @param source_tag Source tag for the GTask.
 * @pre Main thread.
 * @post As in enqueue_lifecycle(): states are set at once, the daemon call and
 *   its minutes-long rebuild happen on the queue's worker thread, and the
 *   result is collected with gs_modulix_lifecycle_finish().
 * @return None.
 */
void gs_modulix_lifecycle_install_async(GsModulixLifecycle *self,
                                        GsPlugin *plugin, GsAppList *list,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data,
                                        gpointer source_tag) {
  enqueue_lifecycle(self, plugin, list, cancellable, callback, user_data,
                    source_tag, &install_ops);
}

/**
 * @brief Starts uninstalling every app of @p list this plugin owns.
 *
 * @param self The lifecycle queue. Not NULL.
 * @param plugin Plugin owning the apps. Not NULL.
 * @param list Apps to uninstall; foreign apps are ignored.
 * @param cancellable Cancellable of the operation, or NULL.
 * @param callback Called when the batch completes, or NULL.
 * @param user_data Data passed to @p callback.
 * @param source_tag Source tag for the GTask.
 * @pre Main thread.
 * @post Same as gs_modulix_lifecycle_install_async(), with the uninstall
 *   operation table.
 * @return None.
 */
void gs_modulix_lifecycle_uninstall_async(GsModulixLifecycle *self,
                                          GsPlugin *plugin, GsAppList *list,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data,
                                          gpointer source_tag) {
  enqueue_lifecycle(self, plugin, list, cancellable, callback, user_data,
                    source_tag, &uninstall_ops);
}

/**
 * @brief Collects the outcome of an install or uninstall batch.
 *
 * @param result The GAsyncResult handed to the completion callback. Not NULL.
 * @param error Return location for the first error, or NULL to ignore it.
 * @pre Must be called once, from the callback of the matching `*_async` call.
 * @post None beyond consuming the task's result.
 * @return TRUE when the batch succeeded, including the case where it had no app
 *   to operate on; FALSE with @p error set otherwise — a failed rebuild and a
 *   refused polkit authorisation both surface this way.
 */
gboolean gs_modulix_lifecycle_finish(GAsyncResult *result, GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}
