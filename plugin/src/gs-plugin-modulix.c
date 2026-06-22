/*
 * gs-plugin-modulix.c — GNOME Software plugin for Modulix-OS (gnome-software
 * 49/50)
 *
 * Two channels:
 *   - READ  (search / list / metadata) → FFI into the Rust `backend` crate.
 *   - WRITE (install / uninstall)       → GDBus to org.modulix.Daemon (system
 * bus).
 *
 * Apps map to GsApp as: nix package → desktop app, Modulix module → desktop app
 * (with its plugins attached as ADDON addons), module plugin → ADDON. The
 * canonical AppStream id is used as the GsApp id so entries deduplicate with
 * the Flatpak/AppStream of the same app. Per-source ordering (module > nix >
 * flatpak, unless flatpak_preferred) is computed by the backend and stashed in
 * "modulix::priority"; note GsApp has no public priority setter (see
 * CLAUDE.md).
 */

#ifndef I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#endif

/* ─── Feature flags ────────────────────────────────────────────────────────
 * Set to FALSE to compile out the corresponding source entirely.
 * Useful during development or on hardware where one path is not available. */
#define MODULIX_ENABLE_PACKAGES TRUE
#define MODULIX_ENABLE_MODULES  TRUE

#include "gs-modulix-app.h"
#include "gs-modulix-enrichment-cache.h"
#include "gs-modulix-icon-cache.h"
#include "gs-modulix-json-utils.h"
#include "gs-modulix-markup.h"

#include "backend.h"
#include <glib/gi18n-lib.h>
#include <gnome-software.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

#define MODULIX_SEARCH_LIMIT 50

/* ─── GObject type ──────────────────────────────────────────────────────── */

#define GS_TYPE_PLUGIN_MODULIX (gs_plugin_modulix_get_type())

G_DECLARE_FINAL_TYPE(GsPluginModulix, gs_plugin_modulix, GS, PLUGIN_MODULIX,
                     GsPlugin)

struct _GsPluginModulix {
  GsPlugin parent_instance;
};

G_DEFINE_TYPE(GsPluginModulix, gs_plugin_modulix, GS_TYPE_PLUGIN)

/* ─── Ownership / kind helpers ──────────────────────────────────────────── */

static gboolean app_is_ours(GsApp *app, GsPlugin *plugin) {
  return gs_app_has_management_plugin(app, plugin);
}

static const gchar *app_kind(GsApp *app) {
  return g_object_get_data(G_OBJECT(app), "modulix::kind");
}

static const gchar *app_name(GsApp *app) {
  return g_object_get_data(G_OBJECT(app), "modulix::name");
}

/* ─── D-Bus to org.modulix.Daemon ───────────────────────────────────────── */

static gboolean daemon_call(const gchar *method, GVariant *params,
                            GError **error) {
  g_autoptr(GDBusConnection) conn =
      g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, error);
  if (!conn) {
    if (params)
      g_variant_unref(g_variant_ref_sink(params));
    return FALSE;
  }

  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
      conn, "org.modulix.Daemon", "/org/modulix/Daemon", "org.modulix.Daemon",
      method, params, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 120000,
      NULL, error);
  return reply != NULL;
}

/* ─── Task data ─────────────────────────────────────────────────────────── */

typedef struct {
  GsPlugin  *plugin;
  GsAppList *list;
  GPtrArray *apps;
  gchar     *query;
  gchar     *alternate_of;
  gboolean   installed;
} TaskData;

static void task_data_free(TaskData *d) {
  g_clear_object(&d->list);
  g_clear_pointer(&d->apps, g_ptr_array_unref);
  g_free(d->query);
  g_free(d->alternate_of);
  g_free(d);
}

/* ─── setup ─────────────────────────────────────────────────────────────── */

static void gs_plugin_modulix_setup_async(GsPlugin *plugin,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
  GTask *task = g_task_new(plugin, cancellable, callback, user_data);
  g_task_set_source_tag(task, gs_plugin_modulix_setup_async);

  bindtextdomain(GETTEXT_PACKAGE, MODULIX_LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");

  GdkDisplay *display = gdk_display_get_default();
  if (display != NULL) {
    GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);
    if (icon_theme != NULL) {
      gtk_icon_theme_add_resource_path(icon_theme,
                                       "/org/modulix/software/icons");

      g_autoptr(GPtrArray) icon_dirs = g_ptr_array_new_with_free_func(g_free);
      g_ptr_array_add(icon_dirs,
                      g_strdup("/run/current-system/sw/share/icons"));
      g_ptr_array_add(icon_dirs,
                      g_build_filename(g_get_home_dir(), ".nix-profile",
                                       "share", "icons", NULL));
      for (const gchar *const *d = g_get_system_data_dirs(); d && *d; d++)
        g_ptr_array_add(icon_dirs, g_build_filename(*d, "icons", NULL));
      g_ptr_array_add(icon_dirs,
                      g_build_filename(g_get_user_data_dir(), "icons", NULL));

      for (guint i = 0; i < icon_dirs->len; i++) {
        const gchar *dir = g_ptr_array_index(icon_dirs, i);
        if (g_file_test(dir, G_FILE_TEST_IS_DIR))
          gtk_icon_theme_add_search_path(icon_theme, dir);
      }
    }
  }

  if (backend_init() != BACKEND_OK) {
    g_task_return_new_error(task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
                            "failed to initialise the Modulix backend");
  } else {
    g_task_return_boolean(task, TRUE);
  }
  g_object_unref(task);
}

static gboolean gs_plugin_modulix_setup_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                               GAsyncResult *result,
                                               GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ─── list_apps ─────────────────────────────────────────────────────────── */

static void list_apps_thread(GTask *task, gpointer source_object G_GNUC_UNUSED,
                             gpointer task_data_ptr,
                             GCancellable *cancellable G_GNUC_UNUSED) {
  TaskData *data = task_data_ptr;
  GsAppList *list = gs_app_list_new();
  gchar *json;
  /* Reuse a single parser for all JSON blobs in this task. */
  g_autoptr(JsonParser) parser = json_parser_new();

  if (data->alternate_of != NULL) {
    json = backend_packages_for_app_id(data->alternate_of);
    gs_modulix_append_apps_from_json(list, json, data->plugin, FALSE, FALSE,
                                     parser);
    if (json)
      backend_free_string(json);
  } else if (data->installed) {
#if MODULIX_ENABLE_PACKAGES
    json = backend_list_installed_packages();
    gs_modulix_append_apps_from_json(list, json, data->plugin, TRUE, FALSE,
                                     parser);
    if (json)
      backend_free_string(json);
#endif
#if MODULIX_ENABLE_MODULES
    json = backend_list_installed_modules();
    gs_modulix_append_apps_from_json(list, json, data->plugin, TRUE, TRUE,
                                     parser);
    if (json)
      backend_free_string(json);
#endif
  } else if (data->query != NULL && *data->query) {
#if MODULIX_ENABLE_PACKAGES
    json = backend_search_packages(data->query, MODULIX_SEARCH_LIMIT);
    gs_modulix_append_apps_from_json(list, json, data->plugin, FALSE, FALSE,
                                     parser);
    if (json)
      backend_free_string(json);
#endif
#if MODULIX_ENABLE_MODULES
    json = backend_search_modules(data->query, MODULIX_SEARCH_LIMIT);
    gs_modulix_append_apps_from_json(list, json, data->plugin, FALSE, TRUE,
                                     parser);
    if (json)
      backend_free_string(json);
#endif
  }

  g_debug("[modulix] list_apps → %u apps", gs_app_list_length(list));
  g_task_return_pointer(task, list, g_object_unref);
}

static void gs_plugin_modulix_list_apps_async(
    GsPlugin *plugin, GsAppQuery *query,
    GsPluginListAppsFlags flags G_GNUC_UNUSED,
    GsPluginEventCallback event_cb G_GNUC_UNUSED,
    gpointer event_data G_GNUC_UNUSED, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  GTask *task = g_task_new(plugin, cancellable, callback, user_data);
  TaskData *data = g_new0(TaskData, 1);
  data->plugin = plugin;

  g_task_set_source_tag(task, gs_plugin_modulix_list_apps_async);

  if (query != NULL) {
    GsApp *alt = gs_app_query_get_alternate_of(query);
    if (alt != NULL)
      data->alternate_of = g_strdup(gs_app_get_id(alt));

    if (gs_app_query_get_is_installed(query) == GS_APP_QUERY_TRISTATE_TRUE)
      data->installed = TRUE;

    const gchar *const *kws = gs_app_query_get_keywords(query);
    if (kws != NULL && kws[0] != NULL)
      data->query = g_strjoinv(" ", (gchar **)kws);
  }

  g_task_set_task_data(task, data, (GDestroyNotify)task_data_free);
  g_task_run_in_thread(task, list_apps_thread);
  g_object_unref(task);
}

static GsAppList *
gs_plugin_modulix_list_apps_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                   GAsyncResult *result, GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}

/* ─── refine ────────────────────────────────────────────────────────────── */

static void refine_one(GsApp *app) {
  const gchar *kind   = app_kind(app);
  const gchar *app_id = g_object_get_data(G_OBJECT(app), "modulix::app_id");

  if (g_strcmp0(kind, "plugin") == 0)
    return;
  if (app_id == NULL || *app_id == '\0')
    return;

  g_autofree gchar *cached = gs_modulix_enrichment_cache_get_or_fetch(app_id);
  if (cached == NULL)
    return;

  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(GError) err = NULL;
  if (!json_parser_load_from_data(parser, cached, -1, &err)) {
    g_warning("[modulix] refine JSON parse: %s", err->message);
    return;
  }

  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
  const gchar *html_desc = gs_modulix_json_str(obj, "description");

  if (html_desc && *html_desc) {
    g_autofree gchar *pango = gs_modulix_html_to_pango(html_desc);
    if (pango && *pango)
      gs_app_set_description(app, GS_APP_QUALITY_NORMAL, pango);
  }

  gs_modulix_add_app_screenshots(app, obj);
}

static void refine_thread(GTask *task, gpointer source_object G_GNUC_UNUSED,
                          gpointer task_data_ptr,
                          GCancellable *cancellable G_GNUC_UNUSED) {
  TaskData *data = task_data_ptr;
  GsAppList *list = data->list;

  for (guint i = 0; i < gs_app_list_length(list); i++) {
    GsApp *app = gs_app_list_index(list, i);
    if (app_is_ours(app, data->plugin))
      refine_one(app);
  }
  g_task_return_boolean(task, TRUE);
}

static void gs_plugin_modulix_refine_async(
    GsPlugin *plugin, GsAppList *list,
    GsPluginRefineFlags job_flags G_GNUC_UNUSED,
    GsPluginRefineRequireFlags require_flags G_GNUC_UNUSED,
    GsPluginEventCallback event_cb G_GNUC_UNUSED,
    gpointer event_data G_GNUC_UNUSED, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  GTask *task = g_task_new(plugin, cancellable, callback, user_data);
  TaskData *data = g_new0(TaskData, 1);
  data->plugin = plugin;
  data->list   = g_object_ref(list);

  g_task_set_source_tag(task, gs_plugin_modulix_refine_async);
  g_task_set_task_data(task, data, (GDestroyNotify)task_data_free);
  g_task_run_in_thread(task, refine_thread);
  g_object_unref(task);
}

static gboolean gs_plugin_modulix_refine_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                                GAsyncResult *result,
                                                GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ─── install / uninstall ───────────────────────────────────────────────── */

static void set_group_state(GPtrArray *apps, const gchar *kind,
                            GsAppState state) {
  for (guint i = 0; i < apps->len; i++) {
    GsApp *app = g_ptr_array_index(apps, i);
    if (g_strcmp0(app_kind(app), kind) == 0)
      gs_app_set_state(app, state);
  }
}

typedef struct {
  const gchar *pkg_method;
  const gchar *mod_method;
  const gchar *plugin_method;
  GsAppState   in_progress;
  GsAppState   on_success;
  GsAppState   on_failure;
} LifecycleOps;

static gboolean lifecycle_call_kind(GPtrArray *apps, const gchar *kind,
                                    const gchar *method, GsAppState on_success,
                                    GsAppState on_failure, GError **error) {
  guint n = 0;
  for (guint i = 0; i < apps->len; i++)
    if (g_strcmp0(app_kind(g_ptr_array_index(apps, i)), kind) == 0)
      n++;

  if (n == 0)
    return TRUE;

  gchar **names = g_new(gchar *, n + 1);
  guint idx = 0;
  for (guint i = 0; i < apps->len; i++) {
    GsApp *app = g_ptr_array_index(apps, i);
    if (g_strcmp0(app_kind(app), kind) == 0)
      names[idx++] = (gchar *)app_name(app); /* borrowed */
  }
  names[idx] = NULL;

  gboolean ok = daemon_call(method, g_variant_new("(^as)", names), error);
  g_free(names);

  set_group_state(apps, kind, ok ? on_success : on_failure);
  return ok;
}

static void lifecycle_run(GTask *task, TaskData *data,
                          const LifecycleOps *ops) {
  GPtrArray *apps = data->apps;
  g_autoptr(GError) err = NULL;
  gboolean ok = TRUE;

  for (guint i = 0; i < apps->len; i++)
    gs_app_set_state(g_ptr_array_index(apps, i), ops->in_progress);

#if MODULIX_ENABLE_PACKAGES
  if (ok)
    ok = lifecycle_call_kind(apps, "package", ops->pkg_method, ops->on_success,
                             ops->on_failure, &err);
#endif

#if MODULIX_ENABLE_MODULES
  if (ok)
    ok = lifecycle_call_kind(apps, "module", ops->mod_method, ops->on_success,
                             ops->on_failure, &err);
#endif

  for (guint i = 0; ok && i < apps->len; i++) {
    GsApp *app = g_ptr_array_index(apps, i);
    if (g_strcmp0(app_kind(app), "plugin") != 0)
      continue;
    const gchar *parent = g_object_get_data(G_OBJECT(app), "modulix::parent");
    const gchar *pname  = g_object_get_data(G_OBJECT(app), "modulix::plugin_name");
    gboolean pok = daemon_call(ops->plugin_method,
                               g_variant_new("(ss)", parent, pname), &err);
    gs_app_set_state(app, pok ? ops->on_success : ops->on_failure);
    if (!pok)
      ok = FALSE;
  }

  if (ok)
    g_task_return_boolean(task, TRUE);
  else
    g_task_return_error(task, g_steal_pointer(&err));
}

static void install_thread(GTask *task, gpointer source_object G_GNUC_UNUSED,
                           gpointer task_data_ptr,
                           GCancellable *cancellable G_GNUC_UNUSED) {
  static const LifecycleOps ops = {
      .pkg_method    = "InstallPackage",
      .mod_method    = "InstallModule",
      .plugin_method = "InstallPlugin",
      .in_progress   = GS_APP_STATE_INSTALLING,
      .on_success    = GS_APP_STATE_INSTALLED,
      .on_failure    = GS_APP_STATE_AVAILABLE,
  };
  lifecycle_run(task, task_data_ptr, &ops);
}

static void uninstall_thread(GTask *task, gpointer source_object G_GNUC_UNUSED,
                             gpointer task_data_ptr,
                             GCancellable *cancellable G_GNUC_UNUSED) {
  static const LifecycleOps ops = {
      .pkg_method    = "UninstallPackage",
      .mod_method    = "UninstallModule",
      .plugin_method = "UninstallPlugin",
      .in_progress   = GS_APP_STATE_REMOVING,
      .on_success    = GS_APP_STATE_AVAILABLE,
      .on_failure    = GS_APP_STATE_INSTALLED,
  };
  lifecycle_run(task, task_data_ptr, &ops);
}

static GPtrArray *collect_owned_apps(GsAppList *list, GsPlugin *plugin) {
  GPtrArray *apps = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; i < gs_app_list_length(list); i++) {
    GsApp *app = gs_app_list_index(list, i);
    if (app_is_ours(app, plugin))
      g_ptr_array_add(apps, g_object_ref(app));
  }
  return apps;
}

static void run_lifecycle_async(GsPlugin *plugin, GsAppList *list,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data, gpointer source_tag,
                                GTaskThreadFunc thread_fn) {
  GTask *task = g_task_new(plugin, cancellable, callback, user_data);
  GPtrArray *apps = collect_owned_apps(list, plugin);

  g_task_set_source_tag(task, source_tag);

  if (apps->len == 0) {
    g_ptr_array_unref(apps);
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
    return;
  }

  TaskData *data = g_new0(TaskData, 1);
  data->plugin = plugin;
  data->apps   = apps;
  g_task_set_task_data(task, data, (GDestroyNotify)task_data_free);
  g_task_run_in_thread(task, thread_fn);
  g_object_unref(task);
}

static void gs_plugin_modulix_install_apps_async(
    GsPlugin *plugin, GsAppList *list,
    GsPluginInstallAppsFlags flags G_GNUC_UNUSED,
    GsPluginProgressCallback progress_cb G_GNUC_UNUSED,
    gpointer progress_data G_GNUC_UNUSED,
    GsPluginEventCallback event_cb G_GNUC_UNUSED,
    gpointer event_data G_GNUC_UNUSED,
    GsPluginAppNeedsUserActionCallback action_cb G_GNUC_UNUSED,
    gpointer action_data G_GNUC_UNUSED, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  run_lifecycle_async(plugin, list, cancellable, callback, user_data,
                      gs_plugin_modulix_install_apps_async, install_thread);
}

static gboolean
gs_plugin_modulix_install_apps_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                      GAsyncResult *result, GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

static void gs_plugin_modulix_uninstall_apps_async(
    GsPlugin *plugin, GsAppList *list,
    GsPluginUninstallAppsFlags flags G_GNUC_UNUSED,
    GsPluginProgressCallback progress_cb G_GNUC_UNUSED,
    gpointer progress_data G_GNUC_UNUSED,
    GsPluginEventCallback event_cb G_GNUC_UNUSED,
    gpointer event_data G_GNUC_UNUSED,
    GsPluginAppNeedsUserActionCallback action_cb G_GNUC_UNUSED,
    gpointer action_data G_GNUC_UNUSED, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  run_lifecycle_async(plugin, list, cancellable, callback, user_data,
                      gs_plugin_modulix_uninstall_apps_async, uninstall_thread);
}

static gboolean
gs_plugin_modulix_uninstall_apps_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                        GAsyncResult *result, GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ─── GObject ───────────────────────────────────────────────────────────── */

static void gs_plugin_modulix_init(GsPluginModulix *self G_GNUC_UNUSED) {
  gs_plugin_add_rule(GS_PLUGIN(self), GS_PLUGIN_RULE_RUN_AFTER, "appstream");
}

static void gs_plugin_modulix_finalize(GObject *object) {
  backend_shutdown();
  gs_modulix_icon_cache_clear();
  gs_modulix_enrichment_cache_clear();
  G_OBJECT_CLASS(gs_plugin_modulix_parent_class)->finalize(object);
}

static void gs_plugin_modulix_class_init(GsPluginModulixClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GsPluginClass *plugin_class = GS_PLUGIN_CLASS(klass);

  object_class->finalize = gs_plugin_modulix_finalize;

  plugin_class->setup_async             = gs_plugin_modulix_setup_async;
  plugin_class->setup_finish            = gs_plugin_modulix_setup_finish;
  plugin_class->list_apps_async         = gs_plugin_modulix_list_apps_async;
  plugin_class->list_apps_finish        = gs_plugin_modulix_list_apps_finish;
  plugin_class->refine_async            = gs_plugin_modulix_refine_async;
  plugin_class->refine_finish           = gs_plugin_modulix_refine_finish;
  plugin_class->install_apps_async      = gs_plugin_modulix_install_apps_async;
  plugin_class->install_apps_finish     = gs_plugin_modulix_install_apps_finish;
  plugin_class->uninstall_apps_async    = gs_plugin_modulix_uninstall_apps_async;
  plugin_class->uninstall_apps_finish   = gs_plugin_modulix_uninstall_apps_finish;
}

/* ─── Entry point ───────────────────────────────────────────────────────── */

GType gs_plugin_query_type(void) { return GS_TYPE_PLUGIN_MODULIX; }
