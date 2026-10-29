/**
 * @file gs-modulix-lifecycle.c
 * @brief install / uninstall, called straight through to the daemon.
 *
 * GNOME Software can fire several install jobs in a row (a module and its
 * plugins, "install all updates", fast clicking). Each one is handled on its
 * own: the vfunc body builds a #GTask, applies the in-progress state on the
 * main thread, and runs the daemon calls on a #GTask worker thread
 * (g_task_run_in_thread(), the same pattern as gs-modulix-list.c,
 * gs-modulix-refine.c and gs-modulix-update.c). Nothing is queued, merged or
 * deferred in this process: `mx-daemon` already serializes its own write
 * commands (the `/tmp/mx-build-queue` rendezvous in
 * `modulix-core-utils/src/core/transaction/`), so a second call simply blocks
 * its own worker thread until the daemon gets to it, and each task completes
 * as soon as *its* call is done rather than waiting on unrelated requests.
 *
 * Each write call this file makes (via `plugin/src/dbus/gs-modulix-daemon1.c`)
 * is a full, synchronous NixOS rebuild carried out by `mx-daemon` (see
 * `modulix-daemon/src/command/`): the daemon checks polkit authorisation
 * before running the command (`Command::execute`'s precondition,
 * `modulix-daemon/src/command/mod.rs`), then runs the corresponding
 * blocking `modulix-core-utils` call via `spawn_blocking` (see the
 * `lifecycle_commands!` macro, `modulix-daemon/src/command/lifecycle.rs`).
 * `gs_modulix_bus_call()` is a *synchronous* GDBus call with no timeout for
 * a write (`G_MAXINT`), so the whole rebuild — commonly minutes — blocks
 * whichever thread in this process calls it; here that is always a #GTask
 * worker thread (lifecycle_thread()), never the main thread.
 *
 * Failure detail: unlike the old `mx_store_*` shim (which collapsed every
 * failure mode to an undifferentiated NULL), a GDBus call failure now
 * carries a real `GError` (D-Bus error name + message, including a denied
 * polkit prompt) — logged at `g_warning` by call_names()/call_plugin() — but
 * the operation still gets one generic %GS_PLUGIN_ERROR_FAILED #GError from
 * lifecycle_execute(), since GNOME Software has no richer per-cause UI for
 * this failure.
 *
 * No progress is reported to the UI while a call is in flight: the
 * `install_apps`/`uninstall_apps` vfuncs accept a `GsPluginProgressCallback`
 * (see gs-plugin-modulix.c) but this file never invokes it. The only
 * feedback the UI gets before completion is the immediate state transition
 * applied in start_lifecycle() (%GS_APP_STATE_INSTALLING /
 * %GS_APP_STATE_REMOVING), set on the main thread before the daemon call
 * even starts.
 *
 * Cancellation: the `GCancellable` passed in is stored on the #GTask but
 * never polled anywhere in this file — no code path checks
 * g_cancellable_is_cancelled() or connects to the cancellable. Once
 * lifecycle_thread() has started, the blocking daemon call and the rebuild it
 * triggers run to completion regardless of cancellation; there is no way to
 * abort an in-flight rebuild from this process. Cancelling only affects what
 * gs_modulix_lifecycle_finish() reports afterwards (%G_IO_ERROR_CANCELLED,
 * via #GTask's default check-cancellable behaviour), not whether the
 * operation happened.
 */

#include "gs-modulix-lifecycle.h"

#include "gs-modulix-app.h"
#include "gs-modulix-config.h"
#include "gs-modulix-plugins-cache.h"

#include "dbus/gs-modulix-daemon1.h"

/* ── operations ─────────────────────────────────────────────────────────── */

/**
 * @brief Everything that differs between an install run and an uninstall run:
 * which `org.modulix.Daemon` method names to call, and which #GsAppState an
 * app should carry before, on success, and on failure.
 *
 * @var LifecycleOps::pkg_method
 * D-Bus method for the batched call over apps of kind `"package"`
 * (`"InstallPackage"` / `"UninstallPackage"`), passed to
 * gs_modulix_daemon1_names_call().
 * @var LifecycleOps::mod_method
 * D-Bus method for the batched call over apps of kind `"module"`
 * (`"InstallModule"` / `"UninstallModule"`).
 * @var LifecycleOps::plugin_method
 * D-Bus method for the per-app call over apps of kind `"plugin"`
 * (`"InstallPlugin"` / `"UninstallPlugin"`), passed to
 * gs_modulix_daemon1_plugin_call() — the daemon has no batched form for
 * plugins.
 * @var LifecycleOps::in_progress
 * State applied immediately (main thread, in start_lifecycle()) to every
 * owned app of the request, before any daemon call runs.
 * @var LifecycleOps::on_success
 * State applied to an app's kind-group once its daemon call succeeds.
 * @var LifecycleOps::on_failure
 * State applied to an app's kind-group once its daemon call fails — i.e.
 * the state is rolled back to what it was before `in_progress`.
 */
typedef struct {
  const gchar *pkg_method;
  const gchar *mod_method;
  const gchar *plugin_method;
  GsAppState in_progress;
  GsAppState on_success;
  GsAppState on_failure;
} LifecycleOps;

/**
 * @brief #LifecycleOps for an install run: `Install*`,
 * %GS_APP_STATE_INSTALLING while in flight, %GS_APP_STATE_INSTALLED on
 * success, back to %GS_APP_STATE_AVAILABLE on failure.
 */
static const LifecycleOps install_ops = {
    .pkg_method = "InstallPackage",
    .mod_method = "InstallModule",
    .plugin_method = "InstallPlugin",
    .in_progress = GS_APP_STATE_INSTALLING,
    .on_success = GS_APP_STATE_INSTALLED,
    .on_failure = GS_APP_STATE_AVAILABLE,
};

/**
 * @brief #LifecycleOps for an uninstall run: `Uninstall*`,
 * %GS_APP_STATE_REMOVING while in flight, %GS_APP_STATE_AVAILABLE on
 * success, back to %GS_APP_STATE_INSTALLED on failure.
 */
static const LifecycleOps uninstall_ops = {
    .pkg_method = "UninstallPackage",
    .mod_method = "UninstallModule",
    .plugin_method = "UninstallPlugin",
    .in_progress = GS_APP_STATE_REMOVING,
    .on_success = GS_APP_STATE_AVAILABLE,
    .on_failure = GS_APP_STATE_INSTALLED,
};

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
 * deduplicated, NULL-terminated array, ready for
 * gs_modulix_daemon1_names_call().
 *
 * @param apps Array of #GsApp pointers to scan. Borrowed.
 * @param kind Kind to match (see app_is_kind()).
 *
 * @pre None; a NULL `gs_modulix_app_name()` is treated as the empty string
 * for deduplication purposes and still added once.
 * @post A name appearing on several apps of @p kind (two #GsApp objects for
 * the same nix attribute) is sent to the daemon only once.
 * @return A newly allocated #GPtrArray of `const gchar *`, one entry per
 * distinct matching name plus a trailing NULL terminator; the strings
 * themselves are borrowed from the apps (via gs_modulix_app_name()) and must
 * not be freed — only the array (`g_ptr_array_unref` / `g_autoptr`) is owned
 * by the caller. Contains just the NULL terminator when no app of @p kind is
 * present.
 */
static GPtrArray *names_of_kind(GPtrArray *apps, const gchar *kind) {
  GPtrArray *names = g_ptr_array_new(); /* borrowed const gchar* */
  g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);

  for (guint i = 0; i < apps->len; i++) {
    GsApp *app = g_ptr_array_index(apps, i);
    if (!app_is_kind(app, kind))
      continue;

    const gchar *name = gs_modulix_app_name(app);
    if (!g_hash_table_add(seen, (gpointer)(name ?: "")))
      continue;

    g_ptr_array_add(names, (gpointer)name);
  }
  g_ptr_array_add(names, NULL);

  return names;
}

/**
 * @brief Tells whether an `org.modulix.Daemon` names write call succeeded,
 * and frees its result either way.
 *
 * @param method D-Bus method name, used only in the warning logged on
 * failure.
 * @param names NULL-terminated array of package or module names, forwarded
 * to gs_modulix_daemon1_names_call().
 *
 * @return TRUE on success; FALSE on any failure (bus unreachable, D-Bus
 * error, denied polkit authorisation, or daemon-side rebuild failure) —
 * the underlying `GError`, now available where the old shim only had an
 * undifferentiated NULL, is logged at `g_warning` before being discarded,
 * since this file's callers still only need the boolean outcome.
 */
static gboolean call_names(const gchar *method, const gchar *const *names) {
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status =
      gs_modulix_daemon1_names_call(method, names, &error);
  if (status == NULL) {
    g_warning("[modulix] Daemon.%s: %s", method, error->message);
    return FALSE;
  }
  return TRUE;
}

/**
 * @brief Calls one `org.modulix.Daemon` plugin method and reports success.
 *
 * @param method D-Bus method name (`"InstallPlugin"`/`"UninstallPlugin"`),
 * used only in the warning logged on failure.
 * @param module Module key owning the plugin.
 * @param plugin Plugin key within that module.
 *
 * @return TRUE on success; FALSE on failure, with the same logging as
 * call_names().
 */
static gboolean call_plugin(const gchar *method, const gchar *module,
                            const gchar *plugin) {
  g_autoptr(GError) error = NULL;
  g_autofree gchar *status =
      gs_modulix_daemon1_plugin_call(method, module, plugin, &error);
  if (status == NULL) {
    g_warning("[modulix] Daemon.%s: %s", method, error->message);
    return FALSE;
  }
  return TRUE;
}

/**
 * @brief Runs one batched `org.modulix.Daemon` call for every @p kind app in
 * @p apps, then applies the resulting success/failure #GsAppState to that
 * group.
 *
 * This is the call that performs the blocking NixOS rebuild for package and
 * module operations (`MODULIX_ENABLE_PACKAGES`/`MODULIX_ENABLE_MODULES`
 * gated in lifecycle_execute()): call_names() blocks the calling thread —
 * lifecycle_thread(), never the main thread — for as long as the daemon takes
 * to authorise (polkit, daemon-side) and apply the change.
 *
 * @param apps Full batch of apps for this request (all kinds mixed). Only
 * the @p kind subset is read or have their state changed.
 * @param kind Kind to filter on (`"package"` or `"module"`).
 * @param method D-Bus method to invoke for this kind's names
 * (`ops->pkg_method` or `ops->mod_method`).
 * @param ops Supplies the success/failure #GsAppState to apply
 * (`ops->on_success` / `ops->on_failure`); `ops->in_progress` is not touched
 * here (already applied earlier, in start_lifecycle()).
 *
 * @return TRUE if there was nothing of @p kind to do, or the call succeeded;
 * FALSE if the call was made and failed.
 * @post On a non-empty subset, every @p kind app in @p apps has its state set
 * to @p ops's `on_success` or `on_failure`, matching the call's outcome. When
 * no app of @p kind is present, no daemon call is made and no state changes.
 */
static gboolean call_for_kind(GPtrArray *apps, const gchar *kind,
                              const gchar *method, const LifecycleOps *ops) {
  g_autoptr(GPtrArray) names = names_of_kind(apps, kind);

  if (names->len == 1) /* just the NULL terminator */
    return TRUE;

  gboolean ok = call_names(method, (const gchar *const *)names->pdata);
  set_group_state(apps, kind, ok ? ops->on_success : ops->on_failure);
  return ok;
}

/**
 * @brief Runs one `org.modulix.Daemon` plugin call per `"plugin"`-kind app in
 * @p apps, stopping at the first failure.
 *
 * Module plugins are installed one call apiece — the daemon's
 * Install/Uninstall Plugin methods take a (module, plugin) pair, not a
 * batch — so, unlike call_for_kind(), each app here triggers its own
 * blocking, polkit-gated NixOS rebuild in sequence on the calling
 * (lifecycle_thread()) thread.
 *
 * @param apps Full batch of apps for this request. Only `"plugin"`-kind
 * apps are read or have their state changed; each one's parent module and
 * plugin name are read from its `"modulix::parent"` /
 * `"modulix::plugin_name"` object data (set when the addon `GsApp` was
 * created — see gs-modulix-app.c).
 * @param ops Supplies `ops->plugin_method` (the daemon call to make) and the
 * success/failure #GsAppState to apply.
 *
 * @return TRUE if every plugin call succeeded (or there were none); FALSE at
 * the first plugin call that failed — apps after that point in @p apps are
 * left as they were (neither called nor state-changed) by this function,
 * since the loop condition stops iterating on failure; lifecycle_thread()
 * then rolls their in-progress state back.
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

    gboolean pok = call_plugin(ops->plugin_method, parent, pname);
    gs_app_set_state(app, pok ? ops->on_success : ops->on_failure);
    /* The plugins-list cache has no TTL of its own (see
     * gs-modulix-plugins-cache.c): without this, the payload fetched before
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
 * thread calls it (always lifecycle_thread() in this file) — for the sum of
 * however many of those calls it ends up making, easily minutes.
 *
 * @param apps Apps to act on (all kinds). Borrowed; each app's #GsAppState
 * is mutated in place by the called functions.
 * @param ops Which operation to run: #install_ops or #uninstall_ops.
 * @param error On failure, set to a %GS_PLUGIN_ERROR_FAILED #GError with a
 * generic "Modulix daemon call failed" message — GNOME Software has no
 * richer per-cause UI for this failure, though the real reason (including
 * "authorisation was refused") is logged by call_names()/call_plugin()
 * where the old shim's undifferentiated NULL could not surface it at all.
 * Left untouched on success. May be NULL to ignore the error (not done by
 * this file's own caller, which always passes a live `GError **`).
 *
 * @return TRUE if every call made succeeded (or a given kind/plugin had
 * nothing to do); FALSE at the first call that failed, at which point
 * remaining kinds/plugins are skipped (package before module before
 * plugins, per the `#if`/`if (ok)` chain below) and their apps still carry
 * the `in_progress` state applied in start_lifecycle() — lifecycle_thread()
 * is what rolls those back, so no app is left stranded.
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
    ok = call_for_kind(apps, "package", ops->pkg_method, ops);
#endif

#if MODULIX_ENABLE_MODULES
  if (ok)
    ok = call_for_kind(apps, "module", ops->mod_method, ops);
#endif

  if (ok)
    ok = call_for_plugins(apps, ops);

  if (!ok)
    g_set_error_literal(error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
                        "Modulix daemon call failed");

  return ok;
}

/* ── task ───────────────────────────────────────────────────────────────── */

/**
 * @brief #GTask task data of one install or uninstall request.
 *
 * @var LifecycleTask::apps
 * Ref'd #GsApp pointers this request covers (from collect_owned_apps()),
 * freed with `g_object_unref` via the array's free func. Never empty: an
 * empty request completes in start_lifecycle() without creating this struct.
 * @var LifecycleTask::ops
 * Points at the static #install_ops or #uninstall_ops; never owned/freed by
 * this struct.
 */
typedef struct {
  GPtrArray *apps;
  const LifecycleOps *ops;
} LifecycleTask;

/**
 * @brief #GDestroyNotify for #LifecycleTask, passed to
 * g_task_set_task_data().
 *
 * @param data The #LifecycleTask to free, passed as `gpointer`. Must not be
 * NULL (the only caller is #GTask's own task-data disposal, which never
 * passes NULL).
 *
 * @post @p data's `apps` array is unref'd (dropping each app's ref) and the
 * struct itself is freed. Does not touch `ops` (unowned, static storage).
 */
static void lifecycle_task_free(gpointer data) {
  LifecycleTask *ltask = data;
  g_clear_pointer(&ltask->apps, g_ptr_array_unref);
  g_free(ltask);
}

/**
 * @brief #GTaskThreadFunc body of one install or uninstall request: runs the
 * daemon calls, rolls back any app left in-progress by a partial failure, and
 * completes the task.
 *
 * @param task The task to complete. Owns the #LifecycleTask passed as
 * @p task_data.
 * @param source_object The owning #GsPlugin, unused here (the #GTask keeps it
 * alive for the whole call, which is what makes a plugin finalize unable to
 * race this thread).
 * @param task_data The request's #LifecycleTask. Not NULL.
 * @param cancellable Accepted for the #GTaskThreadFunc signature and never
 * consulted: an in-flight rebuild cannot be aborted (see the file comment).
 *
 * @pre Runs on a #GTask worker thread, never the main thread: every daemon
 * call below blocks for the whole rebuild.
 * @post The task is completed with TRUE, or with the
 * %GS_PLUGIN_ERROR_FAILED #GError from lifecycle_execute(). On failure,
 * every app still carrying `ops->in_progress` — the kinds lifecycle_execute()
 * skipped after the first failing call — is moved to `ops->on_failure`, so
 * no app is left stuck in %GS_APP_STATE_INSTALLING / %GS_APP_STATE_REMOVING.
 * @return None.
 */
static void lifecycle_thread(GTask *task,
                             gpointer source_object G_GNUC_UNUSED,
                             gpointer task_data,
                             GCancellable *cancellable G_GNUC_UNUSED) {
  LifecycleTask *ltask = task_data;
  g_autoptr(GError) error = NULL;

  if (lifecycle_execute(ltask->apps, ltask->ops, &error)) {
    g_task_return_boolean(task, TRUE);
    return;
  }

  for (guint i = 0; i < ltask->apps->len; i++) {
    GsApp *app = g_ptr_array_index(ltask->apps, i);
    if (gs_app_get_state(app) == ltask->ops->in_progress)
      gs_app_set_state(app, ltask->ops->on_failure);
  }

  g_task_return_error(task, g_steal_pointer(&error));
}

/**
 * @brief Collects the apps of @p list this plugin owns, taking a ref on each.
 *
 * @param list The app list handed to the vfunc. Borrowed.
 * @param plugin The owning #GsPlugin, used by gs_modulix_app_is_ours().
 *
 * @return A newly allocated #GPtrArray of ref'd #GsApp (free func
 * `g_object_unref`), possibly empty. Owned by the caller.
 */
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
 * @brief Starts one install or uninstall request: applies the in-progress
 *        state, then runs the daemon calls on a #GTask worker thread.
 *
 * @param plugin Plugin the apps belong to; used to filter @p list and as the
 *   #GTask source object (which refs it for the operation's lifetime, so a
 *   plugin finalize cannot race the worker). Not NULL.
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
 *   before this function returns. A request with no owned app completes
 *   successfully at once, without touching the daemon. Otherwise the daemon
 *   calls run on a #GTask worker thread and the task completes from there,
 *   once this request's own calls are over — no other request is waited on.
 * @return None.
 */
static void start_lifecycle(GsPlugin *plugin, GsAppList *list,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback, gpointer user_data,
                            gpointer source_tag, const LifecycleOps *ops) {
  g_autoptr(GTask) task = g_task_new(plugin, cancellable, callback, user_data);
  GPtrArray *apps = collect_owned_apps(list, plugin);

  g_task_set_source_tag(task, source_tag);

  if (apps->len == 0) {
    g_ptr_array_unref(apps);
    g_task_return_boolean(task, TRUE);
    return;
  }

  /* Reflect the in-progress state right away (this vfunc runs on the main
   * thread); it is the only feedback the UI gets before completion. */
  for (guint i = 0; i < apps->len; i++)
    gs_app_set_state(g_ptr_array_index(apps, i), ops->in_progress);

  LifecycleTask *ltask = g_new0(LifecycleTask, 1);
  ltask->apps = apps;
  ltask->ops = ops;

  g_task_set_task_data(task, ltask, lifecycle_task_free);
  g_task_run_in_thread(task, lifecycle_thread);
}

/* ── public API ─────────────────────────────────────────────────────────── */

/**
 * @brief Starts installing every app of @p list this plugin owns.
 *
 * @param plugin Plugin owning the apps. Not NULL.
 * @param list Apps to install; foreign apps are ignored.
 * @param cancellable Cancellable of the operation, or NULL. Cancelling stops
 *   the plugin from waiting, it does not abort a rebuild the daemon already
 *   started.
 * @param callback Called when the operation completes, or NULL.
 * @param user_data Data passed to @p callback.
 * @param source_tag Source tag for the GTask.
 * @pre Main thread.
 * @post As in start_lifecycle(): states are set at once, the daemon call and
 *   its minutes-long rebuild happen on a #GTask worker thread, and the
 *   result is collected with gs_modulix_lifecycle_finish().
 * @return None.
 */
void gs_modulix_lifecycle_install_async(GsPlugin *plugin, GsAppList *list,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data,
                                        gpointer source_tag) {
  start_lifecycle(plugin, list, cancellable, callback, user_data, source_tag,
                  &install_ops);
}

/**
 * @brief Starts uninstalling every app of @p list this plugin owns.
 *
 * @param plugin Plugin owning the apps. Not NULL.
 * @param list Apps to uninstall; foreign apps are ignored.
 * @param cancellable Cancellable of the operation, or NULL.
 * @param callback Called when the operation completes, or NULL.
 * @param user_data Data passed to @p callback.
 * @param source_tag Source tag for the GTask.
 * @pre Main thread.
 * @post Same as gs_modulix_lifecycle_install_async(), with the uninstall
 *   operation table.
 * @return None.
 */
void gs_modulix_lifecycle_uninstall_async(GsPlugin *plugin, GsAppList *list,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data,
                                          gpointer source_tag) {
  start_lifecycle(plugin, list, cancellable, callback, user_data, source_tag,
                  &uninstall_ops);
}

/**
 * @brief Collects the outcome of an install or uninstall operation.
 *
 * @param result The GAsyncResult handed to the completion callback. Not NULL.
 * @param error Return location for the first error, or NULL to ignore it.
 * @pre Must be called once, from the callback of the matching `*_async` call.
 * @post None beyond consuming the task's result.
 * @return TRUE when the operation succeeded, including the case where it had
 *   no app to operate on; FALSE with @p error set otherwise — a failed
 *   rebuild and a refused polkit authorisation both surface this way.
 */
gboolean gs_modulix_lifecycle_finish(GAsyncResult *result, GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}
