/*
 * gs-plugin-modulix.c — GNOME Software plugin for Modulix-OS (gnome-software
 * 49/50)
 *
 * Both reads (search / list / metadata) and writes (install / uninstall) go
 * through the `modulix-store-client` shim (`modulix-store-client.h`,
 * `mx_store_*`), a thin C ABI over two D-Bus interfaces served by the
 * `mx-daemon` system daemon: `org.modulix.Store1` (reads, JSON payloads
 * reshaped by the shim from `a{sv}`) and `org.modulix.Daemon` (writes,
 * polkit-gated on the daemon side).
 *
 * Apps map to GsApp as: nix package → desktop app, Modulix module → desktop app
 * (with its plugins attached as ADDON addons), module plugin → ADDON. The
 * canonical AppStream id is used as the GsApp id so entries deduplicate with
 * the Flatpak/AppStream of the same app.
 *
 * Dedup priority against other plugins (module/nix > Flatpak) comes from
 * GS_PLUGIN_RULE_BETTER_THAN edges declared in gs_plugin_modulix_init(), the
 * only lever a plugin has — there is no public per-app priority setter (see
 * CLAUDE.md). Module > nix package, when both come from us, is instead
 * enforced by emission order + a per-task seen_ids table in list_apps_thread
 * (modules first). The "GnomeSoftware::SortKey" metadata, computed here from
 * the daemon's neutral `kind`/`variant_rank` fields (see gs-modulix-app.c),
 * only orders the details-page "Sources" popover, not this dedup.
 */

#ifndef I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#endif

/* ─── Feature flags ────────────────────────────────────────────────────────
 * Set to FALSE to compile out the corresponding source entirely.
 * Useful during development or on hardware where one path is not available. */
#define MODULIX_ENABLE_PACKAGES TRUE
#define MODULIX_ENABLE_MODULES TRUE

#include "gs-modulix-app.h"
#include "gs-modulix-enrichment-cache.h"
#include "gs-modulix-icon-cache.h"
#include "gs-modulix-icon-resolver.h"
#include "gs-modulix-json-utils.h"
#include "gs-modulix-markup.h"
#include "gs-modulix-plugins-cache.h"

#include "modulix-store-client.h"
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

  /* Coalescing queue for install/uninstall D-Bus calls: at most one call is in
   * flight to the daemon at a time; everything enqueued while a call runs is
   * merged into the next single call. Guarded by `lock`. */
  GMutex lock;
  GPtrArray *pending; /* PendingOp*, free func = pending_op_free */
  gboolean in_flight; /* a daemon call is currently running */
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

/* ─── Task data ─────────────────────────────────────────────────────────── */

typedef struct {
  GsPlugin *plugin;
  GsAppList *list;
  GPtrArray *apps;
  gchar *query;
  gchar *alternate_of;
  gboolean installed;
  guint max_results;
  GsPluginRefineRequireFlags require_flags;
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
      /* Where Papirus/Modulix-OS actually live on a NixOS system-profile
       * install (`~/.nix-profile` resolves elsewhere and carries no
       * `icons/`): without this, outside a full GNOME session the theme
       * index built by gs-modulix-icon-resolver.c stays empty and every
       * icon falls straight through to level 2+. */
      g_ptr_array_add(icon_dirs,
                      g_build_filename("/etc/profiles/per-user",
                                       g_get_user_name(), "share", "icons",
                                       NULL));
      for (const gchar *const *d = g_get_system_data_dirs(); d && *d; d++)
        g_ptr_array_add(icon_dirs, g_build_filename(*d, "icons", NULL));
      g_ptr_array_add(icon_dirs,
                      g_build_filename(g_get_user_data_dir(), "icons", NULL));

      /* A single gtk_icon_theme_set_search_path() call instead of N
       * gtk_icon_theme_add_search_path() calls: each add invalidates the
       * theme and re-resolves every already-loaded icon. */
      g_auto(GStrv) existing = gtk_icon_theme_get_search_path(icon_theme);
      g_autoptr(GPtrArray) new_paths =
          g_ptr_array_new_with_free_func(g_free);
      for (GStrv p = existing; p && *p; p++)
        g_ptr_array_add(new_paths, g_strdup(*p));
      for (guint i = 0; i < icon_dirs->len; i++) {
        const gchar *dir = g_ptr_array_index(icon_dirs, i);
        if (g_file_test(dir, G_FILE_TEST_IS_DIR))
          g_ptr_array_add(new_paths, g_strdup(dir));
      }
      g_ptr_array_add(new_paths, NULL);
      gtk_icon_theme_set_search_path(icon_theme,
                                     (const gchar *const *)new_paths->pdata);
    }
  }

  gs_modulix_icon_resolver_init();

  if (mx_store_init() != STORE_OK) {
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

/* Returns TRUE (and completes `task` with a cancellation error, freeing
 * `list_to_free`) if `cancellable` has been cancelled. Callers must return
 * immediately when this returns TRUE. */
static gboolean bail_if_cancelled(GTask *task, GCancellable *cancellable,
                                  GsAppList *list_to_free) {
  g_autoptr(GError) err = NULL;
  if (!g_cancellable_set_error_if_cancelled(cancellable, &err))
    return FALSE;
  g_clear_object(&list_to_free);
  g_task_return_error(task, g_steal_pointer(&err));
  return TRUE;
}

static void list_apps_thread(GTask *task, gpointer source_object G_GNUC_UNUSED,
                             gpointer task_data_ptr,
                             GCancellable *cancellable) {
  TaskData *data = task_data_ptr;
  GsAppList *list = gs_app_list_new();
  gchar *json;
  gint64 t0;
  /* Reuse a single parser for all JSON blobs in this task. */
  g_autoptr(JsonParser) parser = json_parser_new();

  g_debug("[modulix] list_apps(query=%s, installed=%d, alternate_of=%s, "
          "max_results=%u) enter",
          data->query ? data->query : "(null)", data->installed,
          data->alternate_of ? data->alternate_of : "(null)",
          data->max_results);

  if (data->alternate_of != NULL) {
    if (bail_if_cancelled(task, cancellable, list))
      return;
    t0 = g_get_monotonic_time();
    json = mx_store_packages_for_app_id(data->alternate_of);
    g_debug("[modulix] mx_store_packages_for_app_id(%s) %.0fms",
            data->alternate_of, (g_get_monotonic_time() - t0) / 1000.0);
    /* seen_ids = NULL: every entry here intentionally shares the same GsApp
     * id (the app-id being queried) so they stack in the "Sources" popover —
     * deduping by id would collapse them to a single row. */
    gs_modulix_append_apps_from_json(list, json, data->plugin, FALSE, parser,
                                     NULL, TRUE);
    if (json)
      mx_store_free_string(json);

    /* Never fail this job on cancellation. gs-details-page shares one
     * alternate-of job across the whole "Sources" popover and hides it
     * entirely (Flatpak row included) on any error, cancellation included —
     * so a stale request cancelled by fast navigation must still return
     * whatever it has instead of poisoning the popover for the new page. */
    g_debug("[modulix] list_apps → %u apps", gs_app_list_length(list));
    g_task_return_pointer(task, list, g_object_unref);
    return;
  } else if (data->installed) {
    /* Modules emitted before packages: intra-plugin dedup (seen_ids) then
     * keeps the module and drops a package sharing the same GsApp id, so a
     * module always wins its own dedup regardless of list order downstream. */
    g_autoptr(GHashTable) seen_ids =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
#if MODULIX_ENABLE_MODULES
    if (bail_if_cancelled(task, cancellable, list))
      return;
    t0 = g_get_monotonic_time();
    json = mx_store_list_installed_modules();
    g_debug("[modulix] mx_store_list_installed_modules() %.0fms",
            (g_get_monotonic_time() - t0) / 1000.0);
    gs_modulix_append_apps_from_json(list, json, data->plugin, TRUE, parser,
                                     seen_ids, TRUE);
    if (json)
      mx_store_free_string(json);
#endif
#if MODULIX_ENABLE_PACKAGES
    if (bail_if_cancelled(task, cancellable, list))
      return;
    t0 = g_get_monotonic_time();
    json = mx_store_list_installed_packages();
    g_debug("[modulix] mx_store_list_installed_packages() %.0fms",
            (g_get_monotonic_time() - t0) / 1000.0);
    gs_modulix_append_apps_from_json(list, json, data->plugin, TRUE, parser,
                                     seen_ids, TRUE);
    if (json)
      mx_store_free_string(json);
#endif
  } else if (data->query != NULL && *data->query) {
    g_autoptr(GHashTable) seen_ids =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
#if MODULIX_ENABLE_MODULES
    if (bail_if_cancelled(task, cancellable, list))
      return;
    t0 = g_get_monotonic_time();
    json = mx_store_search_modules(data->query, data->max_results);
    g_debug("[modulix] mx_store_search_modules(%s) %.0fms", data->query,
            (g_get_monotonic_time() - t0) / 1000.0);
    gs_modulix_append_apps_from_json(list, json, data->plugin, FALSE, parser,
                                     seen_ids, FALSE);
    if (json)
      mx_store_free_string(json);
#endif
#if MODULIX_ENABLE_PACKAGES
    if (bail_if_cancelled(task, cancellable, list))
      return;
    t0 = g_get_monotonic_time();
    json = mx_store_search_packages(data->query, data->max_results);
    g_debug("[modulix] mx_store_search_packages(%s) %.0fms", data->query,
            (g_get_monotonic_time() - t0) / 1000.0);
    gs_modulix_append_apps_from_json(list, json, data->plugin, FALSE, parser,
                                     seen_ids, FALSE);
    if (json)
      mx_store_free_string(json);
#endif
  }

  if (bail_if_cancelled(task, cancellable, list))
    return;

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

  g_task_set_source_tag(task, gs_plugin_modulix_list_apps_async);

  GsApp *alt = NULL;
  GsAppQueryTristate installed_tristate = GS_APP_QUERY_TRISTATE_UNSET;
  const gchar *const *kws = NULL;
  guint max_results = 0;

  if (query != NULL) {
    alt = gs_app_query_get_alternate_of(query);
    installed_tristate = gs_app_query_get_is_installed(query);
    kws = gs_app_query_get_keywords(query);
    max_results = gs_app_query_get_max_results(query);
  }

  /* We only answer: keywords search, is_installed==TRUE, or alternate_of —
   * exactly one at a time. Everything else (overview/category/featured jobs
   * with none of our properties set) is rejected synchronously, mirroring
   * gs-plugin-packagekit.c, so we never spawn a thread for a query we cannot
   * answer. */
  gboolean supported = (alt != NULL) ||
                      (installed_tristate == GS_APP_QUERY_TRISTATE_TRUE) ||
                      (kws != NULL && kws[0] != NULL);

  if (query == NULL || !supported ||
      gs_app_query_get_n_properties_set(query) != 1) {
    g_debug("[modulix] list_apps → NOT_SUPPORTED");
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Unsupported query");
    g_object_unref(task);
    return;
  }

  TaskData *data = g_new0(TaskData, 1);
  data->plugin = plugin;
  data->max_results = (max_results > 0) ? max_results : MODULIX_SEARCH_LIMIT;

  if (alt != NULL)
    data->alternate_of = g_strdup(gs_app_get_id(alt));
  if (installed_tristate == GS_APP_QUERY_TRISTATE_TRUE)
    data->installed = TRUE;
  if (kws != NULL && kws[0] != NULL)
    data->query = g_strjoinv(" ", (gchar **)kws);

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

#define MODULIX_REFINE_FLAGS_WE_HANDLE                                        \
  (GS_PLUGIN_REFINE_REQUIRE_FLAGS_DESCRIPTION |                              \
   GS_PLUGIN_REFINE_REQUIRE_FLAGS_SCREENSHOTS |                              \
   GS_PLUGIN_REFINE_REQUIRE_FLAGS_ADDONS |                                   \
   GS_PLUGIN_REFINE_REQUIRE_FLAGS_ICON)

static void refine_one(GsApp *app, GsPluginRefineRequireFlags require_flags,
                       GsPlugin *plugin) {
  const gchar *kind = app_kind(app);
  const gchar *app_id = g_object_get_data(G_OBJECT(app), "modulix::app_id");

  if (g_strcmp0(kind, "plugin") == 0)
    return;

  /* Addons (module plugins) are only needed by the details page
   * (GS_PLUGIN_REFINE_REQUIRE_FLAGS_ADDONS); building them costs a
   * full-namespace `nix eval`, so it must never happen on the search path. */
  if ((require_flags & GS_PLUGIN_REFINE_REQUIRE_FLAGS_ADDONS) &&
      g_strcmp0(kind, "module") == 0) {
    const gchar *name = app_name(app);
    if (name != NULL && *name != '\0') {
      gint64 t0 = g_get_monotonic_time();
      gs_modulix_add_module_plugins(app, name, plugin);
      g_debug("[modulix] add_module_plugins(%s) %.0fms", name,
              (g_get_monotonic_time() - t0) / 1000.0);
    }
  }

  /* ICON refine is local-only (theme/file-cache lookups, no network): handled
   * unconditionally here rather than folded into the DESCRIPTION/SCREENSHOTS
   * Flathub-fetch path below. Only replayed when the app still has no icon at
   * all — e.g. an `alternate_of` row, whose backend entry carries `icon:
   * None` (see CLAUDE.md "Icons"). */
  if ((require_flags & GS_PLUGIN_REFINE_REQUIRE_FLAGS_ICON) &&
      !gs_app_has_icons(app)) {
    const gchar *r_icon_name =
        g_object_get_data(G_OBJECT(app), "modulix::icon_name");
    const gchar *r_base_name =
        g_object_get_data(G_OBJECT(app), "modulix::base_name");
    const gchar *r_icon = g_object_get_data(G_OBJECT(app), "modulix::icon");
    const gchar *r_name = app_name(app);
    if (gs_modulix_icon_resolve(app, app_id, r_icon_name, r_name, r_base_name,
                                r_icon))
      g_object_set_data(G_OBJECT(app), "modulix::icon-themed",
                        GINT_TO_POINTER(1));
  }

  if (!(require_flags & (GS_PLUGIN_REFINE_REQUIRE_FLAGS_DESCRIPTION |
                         GS_PLUGIN_REFINE_REQUIRE_FLAGS_SCREENSHOTS)))
    return;
  if (app_id == NULL || *app_id == '\0')
    return;

  gint64 t0 = g_get_monotonic_time();
  g_autofree gchar *cached = gs_modulix_enrichment_cache_get_or_fetch(app_id);
  g_debug("[modulix] enrichment(%s) %.0fms", app_id,
          (g_get_monotonic_time() - t0) / 1000.0);
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
  const gchar *icon = gs_modulix_json_str(obj, "icon");

  if (html_desc && *html_desc) {
    g_autofree gchar *pango = gs_modulix_html_to_pango(html_desc);
    if (pango && *pango)
      gs_app_set_description(app, GS_APP_QUALITY_NORMAL, pango);
  }

  if (icon && *icon) {
    gs_modulix_icon_cache_put(app_id, icon);
    /* Once a themed (Papirus/…) icon has won at listing time, adding this
     * remote one would let gs_app_get_icon_for_size()'s first (sized-icon)
     * pass override it as soon as the download completes — repaint the
     * enrichment icon URL into the cache for other instances, but don't
     * re-add it to this app. */
    if (!g_object_get_data(G_OBJECT(app), "modulix::icon-themed")) {
      g_autoptr(GIcon) remote = gs_modulix_remote_icon_new(icon);
      gs_app_add_icon(app, remote);
    }
  }

  gs_modulix_add_app_screenshots(app, obj);
}

/* Collects the distinct app_ids of `list` entries that will need enrichment
 * (DESCRIPTION/SCREENSHOTS on a non-plugin app we own) and warms the
 * enrichment cache for all of them in one batched backend call, so the
 * per-app fetches in refine_one become cache hits. */
static void prefetch_enrichment(GsAppList *list, GsPlugin *plugin) {
  g_autoptr(GPtrArray) ids =
      g_ptr_array_new(); /* borrowed const gchar*, deduplicated */
  g_autoptr(GHashTable) seen =
      g_hash_table_new(g_str_hash, g_str_equal); /* borrowed keys */

  for (guint i = 0; i < gs_app_list_length(list); i++) {
    GsApp *app = gs_app_list_index(list, i);
    if (!app_is_ours(app, plugin))
      continue;
    if (g_strcmp0(app_kind(app), "plugin") == 0)
      continue;
    const gchar *app_id = g_object_get_data(G_OBJECT(app), "modulix::app_id");
    if (app_id == NULL || *app_id == '\0')
      continue;
    if (g_hash_table_add(seen, (gpointer)app_id))
      g_ptr_array_add(ids, (gpointer)app_id);
  }

  if (ids->len > 0)
    gs_modulix_enrichment_cache_prefetch_many((const gchar *const *)ids->pdata,
                                              ids->len);
}

static void refine_thread(GTask *task, gpointer source_object G_GNUC_UNUSED,
                          gpointer task_data_ptr,
                          GCancellable *cancellable) {
  TaskData *data = task_data_ptr;
  GsAppList *list = data->list;

  if (data->require_flags & (GS_PLUGIN_REFINE_REQUIRE_FLAGS_DESCRIPTION |
                             GS_PLUGIN_REFINE_REQUIRE_FLAGS_SCREENSHOTS))
    prefetch_enrichment(list, data->plugin);

  for (guint i = 0; i < gs_app_list_length(list); i++) {
    g_autoptr(GError) err = NULL;
    if (g_cancellable_set_error_if_cancelled(cancellable, &err)) {
      g_task_return_error(task, g_steal_pointer(&err));
      return;
    }
    GsApp *app = gs_app_list_index(list, i);
    if (app_is_ours(app, data->plugin))
      refine_one(app, data->require_flags, data->plugin);
  }
  g_task_return_boolean(task, TRUE);
}

static void gs_plugin_modulix_refine_async(
    GsPlugin *plugin, GsAppList *list,
    GsPluginRefineFlags job_flags G_GNUC_UNUSED,
    GsPluginRefineRequireFlags require_flags,
    GsPluginEventCallback event_cb G_GNUC_UNUSED,
    gpointer event_data G_GNUC_UNUSED, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  GTask *task = g_task_new(plugin, cancellable, callback, user_data);
  g_task_set_source_tag(task, gs_plugin_modulix_refine_async);

  g_debug("[modulix] refine(require_flags=0x%x, n_apps=%u) enter",
          require_flags, gs_app_list_length(list));

  /* Nothing we handle was requested (e.g. ICON/ID-only refine from the
   * overview page): skip the thread hop entirely rather than doing the
   * blocking Flathub fetch for flags nobody asked for. */
  if (!(require_flags & MODULIX_REFINE_FLAGS_WE_HANDLE)) {
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
    return;
  }

  TaskData *data = g_new0(TaskData, 1);
  data->plugin = plugin;
  data->list = g_object_ref(list);
  data->require_flags = require_flags;

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

static gboolean lifecycle_call_kind(GPtrArray *apps, const gchar *kind,
                                    ModulixNamesFn fn, GsAppState on_success,
                                    GsAppState on_failure) {
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

  gchar *status = fn((const gchar *const *)names, n);
  gboolean ok = status != NULL;
  if (status)
    mx_store_free_string(status);
  g_free(names);

  set_group_state(apps, kind, ok ? on_success : on_failure);
  return ok;
}

/* Runs the actual daemon calls for `apps` under `ops`. Pure worker: touches no
 * GTask. Packages/modules go out as one batched call each; plugins are one
 * call apiece. Sets each app's success/failure state. `error` is filled with
 * a generic failure on the first `mx_store_*` call that returns NULL — the
 * shim doesn't carry structured D-Bus error detail back through its status
 * string, only success/failure. */
static gboolean lifecycle_execute(GPtrArray *apps, const LifecycleOps *ops,
                                  GError **error) {
  gboolean ok = TRUE;

#if MODULIX_ENABLE_PACKAGES
  if (ok)
    ok = lifecycle_call_kind(apps, "package", ops->pkg_fn, ops->on_success,
                             ops->on_failure);
#endif

#if MODULIX_ENABLE_MODULES
  if (ok)
    ok = lifecycle_call_kind(apps, "module", ops->mod_fn, ops->on_success,
                             ops->on_failure);
#endif

  for (guint i = 0; ok && i < apps->len; i++) {
    GsApp *app = g_ptr_array_index(apps, i);
    if (g_strcmp0(app_kind(app), "plugin") != 0)
      continue;
    const gchar *parent = g_object_get_data(G_OBJECT(app), "modulix::parent");
    const gchar *pname =
        g_object_get_data(G_OBJECT(app), "modulix::plugin_name");
    gchar *status = ops->plugin_fn(parent, pname);
    gboolean pok = status != NULL;
    if (status)
      mx_store_free_string(status);
    gs_app_set_state(app, pok ? ops->on_success : ops->on_failure);
    if (!pok)
      ok = FALSE;
  }

  if (!ok)
    g_set_error_literal(error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
                        "Modulix daemon call failed");

  return ok;
}

/* Merges every PendingOp of one operation (all install, or all uninstall) into
 * a single deduplicated app list, runs it once, and completes each op's task
 * with the shared result. */
static void run_group(GPtrArray *ops_list, const LifecycleOps *ops) {
  GPtrArray *merged = g_ptr_array_new(); /* borrowed refs */
  g_autoptr(GHashTable) seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < ops_list->len; i++) {
    PendingOp *op = g_ptr_array_index(ops_list, i);
    for (guint j = 0; j < op->apps->len; j++) {
      GsApp *app = g_ptr_array_index(op->apps, j);
      /* dedup by (kind, name) so overlapping requests don't send an app twice;
       * \x1f separates the fields (absent from kinds and package names) */
      gchar *key =
          g_strdup_printf("%s\x1f%s", app_kind(app) ?: "", app_name(app) ?: "");
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

  g_ptr_array_unref(merged);
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

/* Single serialized worker: drains all pending ops, runs them (coalesced), then
 * loops if more piled up during the run. Exactly one instance is alive at a
 * time (guarded by `in_flight`). */
static gpointer drain_worker(gpointer plugin_ptr) {
  GsPluginModulix *self = plugin_ptr;

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

  g_object_unref(plugin_ptr);
  return NULL;
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

static void enqueue_lifecycle(GsPlugin *plugin, GsAppList *list,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data,
                              gpointer source_tag, const LifecycleOps *ops) {
  GsPluginModulix *self = GS_PLUGIN_MODULIX(plugin);
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
  if (start)
    g_thread_unref(
        g_thread_new("modulix-lifecycle", drain_worker, g_object_ref(self)));
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
  enqueue_lifecycle(plugin, list, cancellable, callback, user_data,
                    gs_plugin_modulix_install_apps_async, &install_ops);
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
  enqueue_lifecycle(plugin, list, cancellable, callback, user_data,
                    gs_plugin_modulix_uninstall_apps_async, &uninstall_ops);
}

static gboolean
gs_plugin_modulix_uninstall_apps_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                        GAsyncResult *result, GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ─── GObject ───────────────────────────────────────────────────────────── */

static void gs_plugin_modulix_init(GsPluginModulix *self) {
  g_mutex_init(&self->lock);
  self->pending = g_ptr_array_new_with_free_func(pending_op_free);
  self->in_flight = FALSE;
  gs_plugin_add_rule(GS_PLUGIN(self), GS_PLUGIN_RULE_RUN_AFTER, "appstream");
  /* Inter-plugin dedup priority (gs_app_get_priority(), lib/gs-app.c) comes
   * from the fixed point over these BETTER_THAN edges, not from plugin
   * order: flatpak declares BETTER_THAN packagekit (priority 1), so modulix
   * must beat both to win the search-page / installed-page dedup against a
   * same-id Flatpak. packagekit is the fallback target if flatpak is
   * disabled; an unresolvable rule target is only a g_debug, not an error. */
  gs_plugin_add_rule(GS_PLUGIN(self), GS_PLUGIN_RULE_BETTER_THAN, "flatpak");
  gs_plugin_add_rule(GS_PLUGIN(self), GS_PLUGIN_RULE_BETTER_THAN, "packagekit");
  /* A GsRemoteIcon we add during a DESCRIPTION refine must still be queued
   * for download in the same job — the "icons" plugin (RUN_AFTER appstream
   * only) needs to run after us to pick it up. */
  gs_plugin_add_rule(GS_PLUGIN(self), GS_PLUGIN_RULE_RUN_BEFORE, "icons");
}

static void gs_plugin_modulix_finalize(GObject *object) {
  GsPluginModulix *self = GS_PLUGIN_MODULIX(object);
  mx_store_shutdown();
  gs_modulix_icon_cache_clear();
  gs_modulix_enrichment_cache_clear();
  gs_modulix_plugins_cache_clear();
  gs_modulix_icon_resolver_shutdown();
  /* A running worker holds a plugin ref, so finalize cannot race a drain. */
  g_clear_pointer(&self->pending, g_ptr_array_unref);
  g_mutex_clear(&self->lock);
  G_OBJECT_CLASS(gs_plugin_modulix_parent_class)->finalize(object);
}

static void gs_plugin_modulix_class_init(GsPluginModulixClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GsPluginClass *plugin_class = GS_PLUGIN_CLASS(klass);

  object_class->finalize = gs_plugin_modulix_finalize;

  plugin_class->setup_async = gs_plugin_modulix_setup_async;
  plugin_class->setup_finish = gs_plugin_modulix_setup_finish;
  plugin_class->list_apps_async = gs_plugin_modulix_list_apps_async;
  plugin_class->list_apps_finish = gs_plugin_modulix_list_apps_finish;
  plugin_class->refine_async = gs_plugin_modulix_refine_async;
  plugin_class->refine_finish = gs_plugin_modulix_refine_finish;
  plugin_class->install_apps_async = gs_plugin_modulix_install_apps_async;
  plugin_class->install_apps_finish = gs_plugin_modulix_install_apps_finish;
  plugin_class->uninstall_apps_async = gs_plugin_modulix_uninstall_apps_async;
  plugin_class->uninstall_apps_finish = gs_plugin_modulix_uninstall_apps_finish;
}

/* ─── Entry point ───────────────────────────────────────────────────────── */

GType gs_plugin_query_type(void) { return GS_TYPE_PLUGIN_MODULIX; }
