/**
 * @file gs-modulix-update.c
 * @brief System-update integration; see the header for the full contract.
 */

#include "gs-modulix-update.h"

#include "gs-modulix-app.h"

#include "dbus/gs-modulix-daemon1.h"
#include "dbus/gs-modulix-store1.h"

#include <glib/gi18n-lib.h>

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
  gs_app_set_kind(app, AS_COMPONENT_KIND_OPERATING_SYSTEM);
  gs_app_set_special_kind(app, GS_APP_SPECIAL_KIND_OS_UPDATE);
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
 * @pre None.
 * @post @p app's state (unless transient), update-details-text and (when at
 *   least one row carried a usable `"input"`) update-version are rewritten.
 * @return TRUE if at least one row was applied (the system is outdated),
 *   FALSE otherwise.
 */
static gboolean update_app_apply(GsApp *app, GVariant *array) {
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

  if (!gs_modulix_state_is_transient(gs_app_get_state(app)))
    gs_app_set_state(app, has_updates ? GS_APP_STATE_UPDATABLE_LIVE
                                      : GS_APP_STATE_INSTALLED);
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
  return update_app_apply(app, array);
}

void gs_modulix_update_list(GsPlugin *plugin, GsAppList *list) {
  if (!gs_modulix_update_sync(plugin, FALSE))
    return;
  g_autoptr(GsApp) app = gs_modulix_update_get_app(plugin);
  gs_app_list_add(list, app);
}

/* ── refresh_metadata ──────────────────────────────────────────────────── */

typedef struct {
  GsPlugin *plugin; /* borrowed: the task's source object outlives this */
  gboolean force_refresh;
} RefreshData;

static void refresh_data_free(RefreshData *d) { g_free(d); }

static void refresh_thread(GTask *task, gpointer source_object G_GNUC_UNUSED,
                           gpointer task_data_ptr,
                           GCancellable *cancellable G_GNUC_UNUSED) {
  RefreshData *d = task_data_ptr;
  gs_modulix_update_sync(d->plugin, d->force_refresh);
  g_task_return_boolean(task, TRUE);
}

void gs_modulix_update_refresh_metadata_async(GsPlugin *plugin,
                                              guint64 cache_age_secs,
                                              GCancellable *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data,
                                              gpointer source_tag) {
  GTask *task = g_task_new(plugin, cancellable, callback, user_data);
  g_task_set_source_tag(task, source_tag);

  RefreshData *data = g_new0(RefreshData, 1);
  data->plugin = plugin;
  data->force_refresh = (cache_age_secs == 0);

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

/**
 * @brief Runs the blocking `UpdateSystem` call for one update, and applies
 *   its outcome to @p app's state.
 *
 * @param plugin Unused (kept for symmetry with the rest of the plugin's
 *   internal APIs; the daemon call needs no plugin-specific context).
 * @param app The "Modulix OS" GsApp being updated. Not NULL. Mutated: state
 *   and (on success) update-details/-version.
 * @param flags Selects `"boot"` (`NO_APPLY` set) vs `"switch"` (otherwise).
 * @param error Set to a generic `GS_PLUGIN_ERROR_FAILED` on failure; the
 *   daemon's real `GError` is logged here, not propagated further (same
 *   convention as gs-modulix-lifecycle.c's call_names()).
 * @pre Called off the main thread: `UpdateSystem` blocks for the whole
 *   rebuild.
 * @post On success, @p app is `GS_APP_STATE_PENDING_INSTALL` (`"boot"`) or
 *   `GS_APP_STATE_INSTALLED` (`"switch"`), with update-details/-version
 *   cleared. On failure, @p app reverts to `GS_APP_STATE_UPDATABLE_LIVE`.
 * @return TRUE on success, FALSE with @p error set otherwise.
 */
static gboolean gs_modulix_update_run(GsPlugin *plugin G_GNUC_UNUSED,
                                      GsApp *app,
                                      GsPluginUpdateAppsFlags flags,
                                      GError **error) {
  gboolean no_apply = (flags & GS_PLUGIN_UPDATE_APPS_FLAGS_NO_APPLY) != 0;
  const gchar *mode = no_apply ? "boot" : "switch";

  g_autoptr(GError) daemon_error = NULL;
  g_autofree gchar *status =
      gs_modulix_daemon1_update_system(mode, &daemon_error);

  if (status == NULL) {
    g_warning("[modulix] Daemon.UpdateSystem(%s): %s", mode,
              daemon_error->message);
    gs_app_set_state(app, GS_APP_STATE_UPDATABLE_LIVE);
    g_set_error_literal(error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
                        "Modulix system update failed");
    return FALSE;
  }

  gs_app_set_state(app, no_apply ? GS_APP_STATE_PENDING_INSTALL
                                 : GS_APP_STATE_INSTALLED);
  gs_app_set_update_details_text(app, NULL);
  gs_app_set_update_version(app, NULL);
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

  G_LOCK(update_lock);
  update_in_flight = FALSE;
  G_UNLOCK(update_lock);

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

  G_LOCK(update_lock);
  gboolean already = update_in_flight;
  if (!already)
    update_in_flight = TRUE;
  G_UNLOCK(update_lock);

  if (already) {
    g_task_return_new_error(task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
                            "A Modulix system update is already in progress");
    g_object_unref(task);
    return;
  }

  gs_app_set_state(app, GS_APP_STATE_INSTALLING);
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
