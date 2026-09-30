/**
 * @file gs-modulix-upgrade.c
 * @brief Distro-upgrade banner integration; see the header for the full
 *   contract.
 */

#include "gs-modulix-upgrade.h"

#include "gs-modulix-app.h"
#include "gs-modulix-update.h"

#include "dbus/gs-modulix-daemon1.h"
#include "dbus/gs-modulix-store1.h"

#include <appstream.h>
#include <glib/gi18n-lib.h>

/** @brief GsApp id prefix of the synthetic release-upgrade row; the release
 *   number is appended so a new release is a new app rather than a mutated
 *   one. */
#define MODULIX_UPGRADE_APP_ID "org.modulix.ModulixOS"

/** @brief Running system's `VERSION_ID`, read once by
 *   gs_modulix_upgrade_init(). NULL when `/etc/os-release` carried none, which
 *   disables the whole feature rather than guessing. */
static gchar *current_version = NULL;

void gs_modulix_upgrade_init(void) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GsOsRelease) os_release = gs_os_release_new(&error);

  if (os_release == NULL) {
    g_warning("[modulix] os-release: %s", error->message);
    return;
  }

  const gchar *version = gs_os_release_get_version_id(os_release);
  if (version == NULL || *version == '\0') {
    g_warning("[modulix] os-release carries no VERSION_ID");
    return;
  }

  g_free(current_version);
  current_version = g_strdup(version);
}

/**
 * @brief Builds — or reuses — the GsApp announcing @p version.
 *
 * Reuse goes through the plugin cache, the same "one GsApp per key" rule as
 * gs-modulix-app.c and gs-modulix-update.c: the banner re-lists on every visit
 * to the Updates page, and a fresh instance would land back in
 * `GS_APP_STATE_AVAILABLE` and undo a download that already completed.
 *
 * @param plugin The owning plugin. Not NULL.
 * @param version Release number. Not NULL, not empty.
 * @param code_name Release name, used as the banner's summary line; may be
 *   NULL or empty, in which case a generic blurb is used.
 * @pre None.
 * @post The app is in the plugin cache under `"upgrade\x1f<version>"`. Its
 *   state is set only on creation — never on reuse, which is what preserves a
 *   completed download.
 * @return (transfer full): the app.
 */
static GsApp *upgrade_app(GsPlugin *plugin, const gchar *version,
                          const gchar *code_name) {
  g_autofree gchar *cache_key = g_strdup_printf("upgrade\x1f%s", version);
  GsApp *app = gs_plugin_cache_lookup(plugin, cache_key);
  gboolean is_new = (app == NULL);

  if (is_new) {
    g_autofree gchar *app_id =
        g_strdup_printf("%s.%s", MODULIX_UPGRADE_APP_ID, version);
    app = gs_app_new(app_id);
    gs_plugin_cache_add(plugin, cache_key, app);
  }

  gs_app_set_management_plugin(app, plugin);
  gs_app_set_kind(app, AS_COMPONENT_KIND_OPERATING_SYSTEM);
  gs_app_set_bundle_kind(app, AS_BUNDLE_KIND_PACKAGE);
  gs_app_set_scope(app, AS_COMPONENT_SCOPE_SYSTEM);
  gs_app_set_name(app, GS_APP_QUALITY_NORMAL, _("Modulix OS"));
  gs_app_set_version(app, version);
  if (code_name != NULL && *code_name != '\0')
    gs_app_set_summary(app, GS_APP_QUALITY_NORMAL, code_name);
  else
    gs_app_set_summary(
        app, GS_APP_QUALITY_NORMAL,
        _("Upgrade for the latest features, performance and stability "
          "improvements."));
  gs_app_set_size_installed(app, GS_SIZE_TYPE_UNKNOWABLE, 0);
  gs_app_set_size_download(app, GS_SIZE_TYPE_UNKNOWABLE, 0);
  gs_app_add_quirk(app, GS_APP_QUIRK_NEEDS_REBOOT);
  gs_app_add_quirk(app, GS_APP_QUIRK_PROVENANCE);
  gs_app_add_quirk(app, GS_APP_QUIRK_NOT_REVIEWABLE);
  if (!gs_app_has_icons(app)) {
    g_autoptr(GIcon) icon = g_themed_icon_new("modulix-logo");
    gs_app_add_icon(app, icon);
  }
  g_object_set_data_full(G_OBJECT(app), "modulix::kind", g_strdup("upgrade"),
                         g_free);

  /* Only on creation: a reused app may already be UPDATABLE (downloaded) or
   * PENDING_INSTALL (prepared), and the banner reads exactly that state. */
  if (is_new)
    gs_app_set_state(app, GS_APP_STATE_AVAILABLE);

  return app;
}

/* ── list_distro_upgrades ───────────────────────────────────────────────── */

static void upgrade_list_thread(GTask *task, gpointer source_object,
                                gpointer task_data G_GNUC_UNUSED,
                                GCancellable *cancellable) {
  GsPlugin *plugin = GS_PLUGIN(source_object);
  g_autoptr(GsAppList) list = gs_app_list_new();

  if (current_version == NULL) {
    g_task_return_pointer(task, g_steal_pointer(&list), g_object_unref);
    return;
  }

  g_autoptr(GVariant) dict = gs_modulix_store1_get_remote_release();
  const gchar *version = NULL, *code_name = NULL;
  if (dict != NULL) {
    g_variant_lookup(dict, "version", "&s", &version);
    g_variant_lookup(dict, "code_name", "&s", &code_name);
  }

  if (version == NULL || *version == '\0') {
    g_debug("[modulix] list_distro_upgrades → no upstream release");
    g_task_return_pointer(task, g_steal_pointer(&list), g_object_unref);
    return;
  }

  if (as_vercmp_simple(version, current_version) <= 0) {
    g_debug("[modulix] list_distro_upgrades → %s is not newer than %s", version,
            current_version);
    g_task_return_pointer(task, g_steal_pointer(&list), g_object_unref);
    return;
  }

  if (!g_cancellable_is_cancelled(cancellable)) {
    g_autoptr(GsApp) app = upgrade_app(plugin, version, code_name);
    gs_app_list_add(list, app);
    g_debug("[modulix] list_distro_upgrades → %s (%s)", version,
            code_name ? code_name : "");
  }

  g_task_return_pointer(task, g_steal_pointer(&list), g_object_unref);
}

void gs_modulix_upgrade_list_async(GsPlugin *plugin,
                                   GsPluginListDistroUpgradesFlags flags
                                       G_GNUC_UNUSED,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data, gpointer source_tag) {
  GTask *task = g_task_new(plugin, cancellable, callback, user_data);
  g_task_set_source_tag(task, source_tag);
  g_task_run_in_thread(task, upgrade_list_thread);
  g_object_unref(task);
}

GsAppList *gs_modulix_upgrade_list_finish(GAsyncResult *result,
                                          GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── download_upgrade / trigger_upgrade ─────────────────────────────────── */

/**
 * @brief Whether @p app is the release-upgrade row this file owns.
 *
 * Both upgrade jobs call every plugin that implements the vfunc, with no
 * management-plugin filtering, so this is what keeps us from acting on another
 * plugin's app.
 *
 * @param app App the job targets; may be NULL.
 * @param plugin The owning plugin. Not NULL.
 * @pre None.
 * @post None.
 * @return TRUE when @p app is ours and carries `modulix::kind == "upgrade"`.
 */
static gboolean upgrade_app_is_ours(GsApp *app, GsPlugin *plugin) {
  return app != NULL && gs_modulix_app_is_ours(app, plugin) &&
         g_strcmp0(gs_modulix_app_kind(app), "upgrade") == 0;
}

typedef struct {
  GsApp *app;            /* owned */
  const gchar *mode;     /* static string: "build" or "boot" */
  GsAppState done_state; /* state to set on success */
  GsAppState fail_state; /* state to restore on failure */
} UpgradeRunData;

static void upgrade_run_data_free(UpgradeRunData *d) {
  g_clear_object(&d->app);
  g_free(d);
}

static void upgrade_run_thread(GTask *task,
                               gpointer source_object G_GNUC_UNUSED,
                               gpointer task_data,
                               GCancellable *cancellable G_GNUC_UNUSED) {
  UpgradeRunData *d = task_data;
  g_autoptr(GError) daemon_error = NULL;
  g_autofree gchar *status =
      gs_modulix_daemon1_update_system(d->mode, &daemon_error);

  gs_modulix_update_release();

  if (status == NULL) {
    g_warning("[modulix] upgrade: Daemon.UpdateSystem(%s): %s", d->mode,
              daemon_error->message);
    gs_app_set_state(d->app, d->fail_state);
    g_task_return_new_error(task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
                            "Modulix release upgrade failed");
    return;
  }

  /* The banner shares Daemon.UpdateSystem with the "Modulix OS" update row:
   * once a mode that applies something has run, the row's memoized CheckUpdate
   * answer would keep offering the very update just prepared. */
  if (g_strcmp0(d->mode, "build") != 0)
    gs_modulix_update_forget_check();

  gs_app_set_state(d->app, d->done_state);
  g_task_return_boolean(task, TRUE);
}

/**
 * @brief Shared body of the two action vfuncs.
 *
 * @param plugin The owning plugin. Not NULL.
 * @param app App the job targets; a foreign app completes the task
 *   successfully without touching anything.
 * @param mode `Daemon.UpdateSystem` mode, a static string.
 * @param busy_state State @p app takes while the call runs.
 * @param done_state State @p app takes on success.
 * @param fail_state State @p app returns to on failure.
 * @param cancellable Optional.
 * @param callback Completion callback.
 * @param user_data Data for @p callback.
 * @param source_tag Source tag set on the task.
 * @pre None.
 * @post Exactly one system rebuild runs at a time process-wide: a second
 *   trigger fails immediately rather than queueing (the flag is shared with
 *   gs-modulix-update.c, which drives the same daemon call).
 */
static void upgrade_run_async(GsPlugin *plugin, GsApp *app, const gchar *mode,
                              GsAppState busy_state, GsAppState done_state,
                              GsAppState fail_state, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data,
                              gpointer source_tag) {
  GTask *task = g_task_new(plugin, cancellable, callback, user_data);
  g_task_set_source_tag(task, source_tag);

  if (!upgrade_app_is_ours(app, plugin)) {
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

  gs_app_set_state(app, busy_state);
  /* No progress channel daemon-side: an unknown progress makes the banner
   * pulse its bar instead of pinning it at 0%. */
  gs_app_set_progress(app, GS_APP_PROGRESS_UNKNOWN);

  UpgradeRunData *data = g_new0(UpgradeRunData, 1);
  data->app = g_object_ref(app);
  data->mode = mode;
  data->done_state = done_state;
  data->fail_state = fail_state;

  g_task_set_task_data(task, data, (GDestroyNotify)upgrade_run_data_free);
  g_task_run_in_thread(task, upgrade_run_thread);
  g_object_unref(task);
}

void gs_modulix_upgrade_download_async(GsPlugin *plugin, GsApp *app,
                                       GsPluginDownloadUpgradeFlags flags
                                           G_GNUC_UNUSED,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data,
                                       gpointer source_tag) {
  upgrade_run_async(plugin, app, "build", GS_APP_STATE_DOWNLOADING,
                    GS_APP_STATE_UPDATABLE, GS_APP_STATE_AVAILABLE, cancellable,
                    callback, user_data, source_tag);
}

gboolean gs_modulix_upgrade_download_finish(GAsyncResult *result,
                                            GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

void gs_modulix_upgrade_trigger_async(GsPlugin *plugin, GsApp *app,
                                      GsPluginTriggerUpgradeFlags flags
                                          G_GNUC_UNUSED,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data,
                                      gpointer source_tag) {
  upgrade_run_async(plugin, app, "boot", GS_APP_STATE_INSTALLING,
                    GS_APP_STATE_PENDING_INSTALL, GS_APP_STATE_UPDATABLE,
                    cancellable, callback, user_data, source_tag);
}

gboolean gs_modulix_upgrade_trigger_finish(GAsyncResult *result,
                                           GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}
