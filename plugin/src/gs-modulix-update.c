/**
 * @file gs-modulix-update.c
 * @brief System-update integration; see the header for the full contract.
 */

#include "gs-modulix-update.h"

#include "gs-modulix-app.h"

#include "dbus/gs-modulix-daemon1.h"
#include "dbus/gs-modulix-store1.h"

#include <glib/gi18n-lib.h>

/** @brief Guards `last_check_outdated`/`last_check_time` below. */
G_LOCK_DEFINE_STATIC(check_lock);

/** @brief Last answer of `Store1.CheckUpdate`, which is authoritative where
 *   `ListOutdatedInputs` is merely descriptive: `diff_locks()`
 *   (`modulix-core-utils/src/update.rs`) only walks the lockfile root's
 *   *direct* inputs, so a candidate that moved a transitive one yields zero rows
 *   and is still a real update. Kept so gs_modulix_update_list() — which must
 *   not spend a live check — can still list the app in that case. Cleared once
 *   an update has been applied through this process. */
static gboolean last_check_outdated = FALSE;

/** @brief `g_get_monotonic_time()` when `last_check_outdated` was written, so
 *   the memo can expire. */
static gint64 last_check_time = 0;

/** @brief How long `last_check_outdated` is trusted.
 *
 * The memo only has to bridge one GNOME Software pass — `refresh_metadata` and
 * the `is-for-update` listing that `refresh_cache_finished_cb` issues right
 * after it, seconds apart — so a wide margin still expires long before it can
 * do harm. Without an expiry, an update applied *outside* GNOME Software
 * (`nixos-rebuild switch` from a terminal) would leave the flag set forever:
 * every Updates-page visit would force a detail-less row back on, and the
 * automatic monitor would spend a real multi-minute `UpdateSystem` on an update
 * that no longer exists. */
#define MODULIX_CHECK_MEMO_TTL_US (10 * 60 * G_USEC_PER_SEC)

/**
 * @brief Reads the memoized `CheckUpdate` answer, if still fresh.
 *
 * @pre None.
 * @post None (the memo is not cleared on expiry; it is simply not reported).
 * @return TRUE when the last gs_modulix_update_check() found an update less
 *   than #MODULIX_CHECK_MEMO_TTL_US ago; FALSE otherwise.
 */
static gboolean check_memo_is_outdated(void) {
  G_LOCK(check_lock);
  gboolean fresh =
      last_check_outdated &&
      (g_get_monotonic_time() - last_check_time) < MODULIX_CHECK_MEMO_TTL_US;
  G_UNLOCK(check_lock);
  return fresh;
}

/**
 * @brief Records a `CheckUpdate` answer, or retires the memo.
 *
 * @param outdated The answer to remember; FALSE also clears the timestamp.
 * @pre None.
 * @post `last_check_outdated` is @p outdated and `last_check_time` is now.
 */
static void check_memo_set(gboolean outdated) {
  G_LOCK(check_lock);
  last_check_outdated = outdated;
  last_check_time = g_get_monotonic_time();
  G_UNLOCK(check_lock);
}

/**
 * @brief Sets @p app's state and @p needs_reboot's worth of
 *   `GS_APP_QUIRK_NEEDS_REBOOT` in one place.
 *
 * The quirk is never implied by the state, it is always passed in, because the
 * two reasons to carry it are different facts:
 *
 * - `GS_APP_STATE_PENDING_INSTALL` always carries it: nothing has been
 *   activated yet, so the reboot *is* the installation.
 * - `GS_APP_STATE_INSTALLED` after an `UpdateSystem("switch")` carries it only
 *   when gs_modulix_store1_reboot_required() says the running kernel, kernel
 *   modules or initrd no longer match the activated system. A switch that
 *   moved only userspace needs no reboot at all.
 *
 * Every other state clears it, and that matters: `_get_app_section()`
 * (`src/gs-updates-page.c`) routes any `AS_COMPONENT_KIND_OPERATING_SYSTEM`
 * app carrying the quirk into the *offline* section, and
 * `_button_update_all_clicked_cb()` (`src/gs-updates-section.c`) then sets
 * `do_reboot` for that whole section — so a quirk left over from a previous
 * cycle turns a later interactive "Update" into an unprompted reboot. Routing
 * every state change through this function is what keeps that from happening.
 *
 * @param app GsApp to mutate. Not NULL.
 * @param state State to set.
 * @param needs_reboot Whether @p app must carry `GS_APP_QUIRK_NEEDS_REBOOT`
 *   in that state.
 * @pre @p needs_reboot is TRUE whenever @p state is
 *   `GS_APP_STATE_PENDING_INSTALL` — see above.
 * @post @p app is in @p state, and carries `GS_APP_QUIRK_NEEDS_REBOOT` iff
 *   @p needs_reboot.
 */
static void update_set_state(GsApp *app, GsAppState state,
                             gboolean needs_reboot) {
  gs_app_set_state(app, state);
  if (needs_reboot)
    gs_app_add_quirk(app, GS_APP_QUIRK_NEEDS_REBOOT);
  else
    gs_app_remove_quirk(app, GS_APP_QUIRK_NEEDS_REBOOT);
}

/** @brief GsApp id of the synthetic "Modulix OS" update row. */
#define MODULIX_UPDATE_APP_ID "org.modulix.ModulixOS"

/** @brief Plugin-cache key of the synthetic "Modulix OS" update row (same
 *   `"<namespace>\x1f<name>"` convention as gs-modulix-app.c). */
#define MODULIX_UPDATE_CACHE_KEY "update\x1fmodulix-os"

GsApp *gs_modulix_update_get_app(GsPlugin *plugin) {
  static GMutex cache_key_mutex;
  GsApp *app;
  {
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&cache_key_mutex);
    app = gs_plugin_cache_lookup(plugin, MODULIX_UPDATE_CACHE_KEY);
    if (app == NULL) {
      app = gs_app_new(MODULIX_UPDATE_APP_ID);
      gs_plugin_cache_add(plugin, MODULIX_UPDATE_CACHE_KEY, app);
    }
  }

  gs_app_set_management_plugin(app, plugin);
  /* Order matters: gs_app_set_special_kind() sets the kind to GENERIC
   * internally (lib/gs-app.c), and OPERATING_SYSTEM → GENERIC is a rejected
   * transition, so setting the kind first made every session log
   * "Kind change on org.modulix.ModulixOS … is not OK". UNKNOWN → GENERIC →
   * OPERATING_SYSTEM are both allowed, and both setters are no-ops on the
   * second call, so the cached app stays put. */
  gs_app_set_special_kind(app, GS_APP_SPECIAL_KIND_OS_UPDATE);
  gs_app_set_kind(app, AS_COMPONENT_KIND_OPERATING_SYSTEM);
  gs_app_set_bundle_kind(app, AS_BUNDLE_KIND_PACKAGE);
  gs_app_set_scope(app, AS_COMPONENT_SCOPE_SYSTEM);
  gs_app_set_name(app, GS_APP_QUALITY_NORMAL, _("Modulix OS"));
  gs_app_set_summary(app, GS_APP_QUALITY_NORMAL, _("System update"));
  gs_app_set_size_download(app, GS_SIZE_TYPE_UNKNOWABLE, 0);
  if (!gs_app_has_icons(app)) {
    g_autoptr(GIcon) icon = g_themed_icon_new("modulix-logo");
    gs_app_add_icon(app, icon);
  }
  g_object_set_data_full(G_OBJECT(app), "modulix::kind", g_strdup("update"),
                         g_free);

  return app;
}

/**
 * @brief Applies one `ListOutdatedInputs` reply to @p app's dynamic fields.
 *
 * @param app The "Modulix OS" GsApp to update. Not NULL.
 * @param array `aa{sv}` reply of `gs_modulix_store1_list_outdated_inputs()`,
 *   or NULL (the read failed — treated as "no outdated inputs"). Borrowed.
 * @param force_outdated TRUE to mark the system outdated whatever @p array
 *   holds: `CheckUpdate` is the authoritative answer, and it can legitimately
 *   report an update whose lockfile diff moved no *direct* input, in which
 *   case the row list is empty but the update is real.
 * @pre None.
 * @post @p app's state (unless transient), update-details-text and (when at
 *   least one row carried a usable `"input"`) update-version are rewritten —
 *   except when @p app is `GS_APP_STATE_PENDING_INSTALL` and nothing is
 *   outdated, where everything is left as the completed `"boot"` update left it.
 * @return TRUE if the system is outdated — at least one row was applied, or
 *   @p force_outdated was set; FALSE otherwise (including the
 *   `PENDING_INSTALL` early return: gs_modulix_update_list() lists that app on
 *   its state, not on this return value).
 */
static gboolean update_app_apply(GsApp *app, GVariant *array,
                                 gboolean force_outdated) {
  g_autoptr(GString) details = g_string_new(NULL);
  guint64 latest = 0;
  gboolean has_updates = FALSE;

  if (array != NULL) {
    GVariantIter iter;
    g_variant_iter_init(&iter, array);
    GVariant *dict;
    while ((dict = g_variant_iter_next_value(&iter)) != NULL) {
      const gchar *input = NULL, *current_rev = NULL, *new_rev = NULL;
      guint64 last_modified = 0;
      g_variant_lookup(dict, "input", "&s", &input);
      g_variant_lookup(dict, "current_rev", "&s", &current_rev);
      g_variant_lookup(dict, "new_rev", "&s", &new_rev);
      g_variant_lookup(dict, "last_modified", "t", &last_modified);
      g_variant_unref(dict);

      if (!input || !*input)
        continue;

      if (has_updates)
        g_string_append_c(details, '\n');
      has_updates = TRUE;

      if (last_modified > latest)
        latest = last_modified;

      g_autoptr(GDateTime) dt =
          g_date_time_new_from_unix_local((gint64)last_modified);
      g_autofree gchar *date =
          dt != NULL ? g_date_time_format(dt, "%d/%m/%Y") : g_strdup("");
      g_string_append_printf(details, "%s : %s \xe2\x86\x92 %s (%s)", input,
                             current_rev ? current_rev : "",
                             new_rev ? new_rev : "", date);
    }
  }

  has_updates = has_updates || force_outdated;

  /* PENDING_INSTALL is not transient (nothing is in flight), but a completed
   * "boot" update owns both the state and the details it left behind, and the
   * daemon's outdated-inputs cache is empty by then — so rewriting would drop
   * the "restart to apply" state and blank the row's description on the next
   * visit to the Updates page. A *newly discovered* update does take it over,
   * hence the !has_updates term: staying PENDING_INSTALL there would keep the
   * app out of _should_auto_update() and strand the new update unapplied. */
  GsAppState state = gs_app_get_state(app);
  if (state == GS_APP_STATE_PENDING_INSTALL && !has_updates)
    return FALSE;

  if (!gs_modulix_state_is_transient(state))
    update_set_state(app,
                     has_updates ? GS_APP_STATE_UPDATABLE_LIVE
                                 : GS_APP_STATE_INSTALLED,
                     FALSE);


  gs_app_set_update_details_text(app, details->str);

  if (latest > 0) {
    g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local((gint64)latest);
    g_autofree gchar *iso = g_date_time_format(dt, "%Y-%m-%d");
    gs_app_set_update_version(app, iso);
  }

  return has_updates;
}

gboolean gs_modulix_update_sync(GsPlugin *plugin, gboolean force_refresh) {
  g_autoptr(GVariant) array =
      gs_modulix_store1_list_outdated_inputs(force_refresh);
  g_autoptr(GsApp) app = gs_modulix_update_get_app(plugin);

  return update_app_apply(app, array, check_memo_is_outdated());
}

gboolean gs_modulix_update_check(GsPlugin *plugin) {
  gboolean available = gs_modulix_store1_check_update();
  g_autoptr(GVariant) array = gs_modulix_store1_list_outdated_inputs(FALSE);
  g_autoptr(GsApp) app = gs_modulix_update_get_app(plugin);

  check_memo_set(available);
  return update_app_apply(app, array, available);
}

void gs_modulix_update_list(GsPlugin *plugin, GsAppList *list) {
  /* gs_modulix_update_sync() already folds last_check_outdated in. */
  gboolean outdated = gs_modulix_update_sync(plugin, FALSE);
  g_autoptr(GsApp) app = gs_modulix_update_get_app(plugin);

  /* A finished "boot" update is still a row to show ("restart to apply"), even
   * though the daemon's outdated-inputs cache is empty by then. */
  if (!outdated && gs_app_get_state(app) != GS_APP_STATE_PENDING_INSTALL)
    return;

  gs_app_list_add(list, app);
}

/* ── refresh_metadata ──────────────────────────────────────────────────── */

typedef struct {
  GsPlugin *plugin; /* borrowed: the task's source object outlives this */
} RefreshData;

static void refresh_data_free(RefreshData *d) { g_free(d); }

static void refresh_thread(GTask *task, gpointer source_object G_GNUC_UNUSED,
                           gpointer task_data_ptr,
                           GCancellable *cancellable G_GNUC_UNUSED) {
  RefreshData *d = task_data_ptr;
  gs_modulix_update_check(d->plugin);
  g_task_return_boolean(task, TRUE);
}

void gs_modulix_update_refresh_metadata_async(
    GsPlugin *plugin, guint64 cache_age_secs G_GNUC_UNUSED,
    GCancellable *cancellable, GAsyncReadyCallback callback,
    gpointer user_data, gpointer source_tag) {
  GTask *task = g_task_new(plugin, cancellable, callback, user_data);
  g_task_set_source_tag(task, source_tag);

  RefreshData *data = g_new0(RefreshData, 1);
  data->plugin = plugin;

  g_task_set_task_data(task, data, (GDestroyNotify)refresh_data_free);
  g_task_run_in_thread(task, refresh_thread);
  g_object_unref(task);
}

gboolean gs_modulix_update_refresh_metadata_finish(GAsyncResult *result,
                                                    GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ── update_apps ────────────────────────────────────────────────────────── */

/** @brief Guards `update_in_flight` below: at most one system update runs at
 *   a time process-wide (see gs_modulix_update_apps_async()'s doc). */
G_LOCK_DEFINE_STATIC(update_lock);
static gboolean update_in_flight = FALSE;

gboolean gs_modulix_update_try_acquire(void) {
  G_LOCK(update_lock);
  gboolean already = update_in_flight;
  if (!already)
    update_in_flight = TRUE;
  G_UNLOCK(update_lock);
  return !already;
}

void gs_modulix_update_release(void) {
  G_LOCK(update_lock);
  update_in_flight = FALSE;
  G_UNLOCK(update_lock);
}

/**
 * @brief Maps GNOME Software's update flags onto a `Daemon.UpdateSystem` mode.
 *
 * GNOME Software applies an update in two jobs: a `NO_APPLY` one ("download"),
 * then — only if the app is still `GS_APP_STATE_UPDATABLE_LIVE` when the first
 * finishes (`_should_auto_update()` in `src/gs-update-monitor.c`) — a second
 * one that applies. `NO_APPLY` therefore decides *whether* anything is
 * activated, and `INTERACTIVE` decides *when*.
 *
 * `INTERACTIVE` is GNOME Software's manual/automatic discriminant: every
 * click-driven path sets it (`src/gs-updates-section.c`, `src/gs-page.c`),
 * `gs-update-monitor.c` never does. Reading it is what makes a user who
 * clicked "Update Now" get the update *now* — `UpdateSystem("switch")`,
 * a `nixos-rebuild switch` that replaces the running system — while the
 * monitor's unattended apply stays `UpdateSystem("boot")`, which only
 * pre-builds; `mx-apply-update.service` promotes it at shutdown. That split is
 * the daemon's own design (`modulix-daemon/src/command/update.rs`): `"switch"`
 * is documented there as the manual path and the only mode that touches the
 * running system.
 *
 * Note the daemon does **not** restart itself across a `"switch"`
 * (`restartIfChanged = false`), so an update carrying a new daemon keeps being
 * served by the old binary until the next boot: a client must not assume a
 * fresh version's methods exist right after one.
 *
 * The "Modulix OS" row stays on the Updates page so an update remains
 * reachable when GNOME Software's own automatic updates are switched off
 * (GSettings `download-updates`, read by `should_download_updates()` in
 * `src/gs-update-monitor.c`).
 *
 * @param flags Flags the vfunc was called with.
 * @pre None.
 * @post None (pure function).
 * @return `"build"` when `NO_APPLY` is set (realise the closure, activate
 *   nothing) — including for a click, whose download job carries both flags;
 *   `"switch"` for an interactive apply (activate now); `"boot"` for an
 *   unattended one (activate at the next boot). Never NULL.
 */
static const gchar *update_mode_for_flags(GsPluginUpdateAppsFlags flags) {
  if (flags & GS_PLUGIN_UPDATE_APPS_FLAGS_NO_APPLY)
    return "build";
  if (flags & GS_PLUGIN_UPDATE_APPS_FLAGS_INTERACTIVE)
    return "switch";
  return "boot";
}

/**
 * @brief Runs the blocking `UpdateSystem` call for one update, and applies
 *   its outcome to @p app's state.
 *
 * @param plugin Unused (kept for symmetry with the rest of the plugin's
 *   internal APIs; the daemon call needs no plugin-specific context).
 * @param app The "Modulix OS" GsApp being updated. Not NULL. Mutated: state
 *   and, when the update was applied, the `GS_APP_QUIRK_NEEDS_REBOOT` quirk.
 * @param flags Selects the mode, see update_mode_for_flags().
 * @param error Set to a generic `GS_PLUGIN_ERROR_FAILED` on failure; the
 *   daemon's real `GError` is logged here, not propagated further (same
 *   convention as gs-modulix-lifecycle.c's call_names()).
 * @pre Called off the main thread: `UpdateSystem` blocks for the whole
 *   rebuild, and the `"switch"` branch adds a `RebootRequired` round-trip.
 * @post On success, one of three outcomes:
 *   - `"build"` restores `GS_APP_STATE_UPDATABLE_LIVE` (nothing was applied,
 *     and that state is what lets GNOME Software issue the apply job that
 *     follows);
 *   - `"switch"` retires the `CheckUpdate` memo and lands on
 *     `GS_APP_STATE_INSTALLED` — the update *is* applied — carrying
 *     `GS_APP_QUIRK_NEEDS_REBOOT` only if
 *     gs_modulix_store1_reboot_required() says the running kernel, kernel
 *     modules or initrd no longer match the activated system;
 *   - `"boot"` retires the memo and lands on `GS_APP_STATE_PENDING_INSTALL`
 *     with the quirk added unconditionally: nothing is activated before the
 *     reboot.
 *   In every case the update-details/-version are kept: they describe what the
 *   pending apply, respectively the next boot, will activate — or, after a
 *   switch, what was just activated. The quirk is only ever set through
 *   update_set_state(). On failure, @p app reverts to
 *   `GS_APP_STATE_UPDATABLE_LIVE`.
 * @return TRUE on success, FALSE with @p error set otherwise.
 */
static gboolean gs_modulix_update_run(GsPlugin *plugin G_GNUC_UNUSED,
                                      GsApp *app,
                                      GsPluginUpdateAppsFlags flags,
                                      GError **error) {
  const gchar *mode = update_mode_for_flags(flags);

  g_autoptr(GError) daemon_error = NULL;
  g_autofree gchar *status =
      gs_modulix_daemon1_update_system(mode, &daemon_error);

  if (status == NULL) {
    g_warning("[modulix] Daemon.UpdateSystem(%s): %s", mode,
              daemon_error->message);
    update_set_state(app, GS_APP_STATE_UPDATABLE_LIVE, FALSE);
    g_set_error_literal(error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
                        "Modulix system update failed");
    return FALSE;
  }

  if (g_strcmp0(mode, "build") == 0) {
    /* Nothing was applied; the app must be back to UPDATABLE_LIVE by the time
     * the job completes, or _should_auto_update() drops it and the apply job is
     * never issued. */
    update_set_state(app, GS_APP_STATE_UPDATABLE_LIVE, FALSE);
    return TRUE;
  }

  check_memo_set(FALSE);

  if (g_strcmp0(mode, "switch") == 0) {
    /* Applied to the running system, so the row is done — but the kernel,
     * modules and initrd in use are still the booted ones, which only the
     * daemon can compare. */
    update_set_state(app, GS_APP_STATE_INSTALLED,
                     gs_modulix_store1_reboot_required());
    return TRUE;
  }

  update_set_state(app, GS_APP_STATE_PENDING_INSTALL, TRUE);
  return TRUE;
}

typedef struct {
  GsPlugin *plugin; /* borrowed: the task's source object outlives this */
  GsApp *app;       /* owned */
  GsPluginUpdateAppsFlags flags;
} UpdateRunData;

static void update_run_data_free(UpdateRunData *d) {
  g_clear_object(&d->app);
  g_free(d);
}

static void update_run_thread(GTask *task,
                              gpointer source_object G_GNUC_UNUSED,
                              gpointer task_data_ptr,
                              GCancellable *cancellable G_GNUC_UNUSED) {
  UpdateRunData *d = task_data_ptr;
  GError *error = NULL;
  gboolean ok = gs_modulix_update_run(d->plugin, d->app, d->flags, &error);

  gs_modulix_update_release();

  if (ok)
    g_task_return_boolean(task, TRUE);
  else
    g_task_return_error(task, error);
}

/**
 * @brief Finds the "Modulix OS" GsApp among @p list's entries, if any.
 *
 * @param plugin Ownership test (gs_modulix_app_is_ours()). Not NULL.
 * @param list Apps to scan. Not NULL. Not mutated.
 * @pre None.
 * @post None.
 * @return (transfer none) (nullable): the matching GsApp, borrowed from
 *   @p list, or NULL if none of @p list's entries is ours with
 *   `modulix::kind == "update"`.
 */
static GsApp *find_update_app(GsPlugin *plugin, GsAppList *list) {
  for (guint i = 0; i < gs_app_list_length(list); i++) {
    GsApp *candidate = gs_app_list_index(list, i);
    if (gs_modulix_app_is_ours(candidate, plugin) &&
        g_strcmp0(gs_modulix_app_kind(candidate), "update") == 0)
      return candidate;
  }
  return NULL;
}

void gs_modulix_update_apps_async(GsPlugin *plugin, GsAppList *list,
                                  GsPluginUpdateAppsFlags flags,
                                  GsPluginProgressCallback progress_cb,
                                  gpointer progress_data,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data, gpointer source_tag) {
  GTask *task = g_task_new(plugin, cancellable, callback, user_data);
  g_task_set_source_tag(task, source_tag);

  GsApp *app = find_update_app(plugin, list);
  gboolean no_op = (flags & GS_PLUGIN_UPDATE_APPS_FLAGS_NO_DOWNLOAD) &&
                   (flags & GS_PLUGIN_UPDATE_APPS_FLAGS_NO_APPLY);

  if (app == NULL || no_op) {
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
    return;
  }

  if (!gs_modulix_update_try_acquire()) {
    g_task_return_new_error(task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
                            "A Modulix system update is already in progress");
    g_object_unref(task);
    return;
  }

  update_set_state(app,
                   (flags & GS_PLUGIN_UPDATE_APPS_FLAGS_NO_APPLY)
                       ? GS_APP_STATE_DOWNLOADING
                       : GS_APP_STATE_INSTALLING,
                   FALSE);
  gs_app_set_progress(app, GS_APP_PROGRESS_UNKNOWN);
  if (progress_cb != NULL)
    progress_cb(plugin, GS_APP_PROGRESS_UNKNOWN, progress_data);

  UpdateRunData *data = g_new0(UpdateRunData, 1);
  data->plugin = plugin;
  data->app = g_object_ref(app);
  data->flags = flags;

  g_task_set_task_data(task, data, (GDestroyNotify)update_run_data_free);
  g_task_run_in_thread(task, update_run_thread);
  g_object_unref(task);
}

gboolean gs_modulix_update_apps_finish(GAsyncResult *result, GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}
