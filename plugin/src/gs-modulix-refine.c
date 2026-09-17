/*
 * gs-modulix-refine.c — the refine vfunc: addons, icons, descriptions,
 * licenses and Flathub enrichment for the apps we manage.
 *
 * Two batched prefetches run before the per-app pass, so the per-app work is
 * pure cache reads:
 *   - enrichment (one daemon call for every distinct app-id of the list),
 *   - licenses (one daemon call, details page only — it costs a `nix eval`
 *     per attribute daemon-side).
 */

#include "gs-modulix-refine.h"

#include "gs-modulix-app.h"
#include "gs-modulix-enrichment-cache.h"
#include "gs-modulix-icon-cache.h"
#include "gs-modulix-icon-resolver.h"
#include "gs-modulix-json-utils.h"
#include "gs-modulix-markup.h"

#include "modulix-store-client.h"
#include <json-glib/json-glib.h>

typedef struct {
  GsPlugin *plugin; /* borrowed: the task's source object outlives the data */
  GsAppList *list;
  GsPluginRefineRequireFlags require_flags;
  /* nix attribute (owned) -> SPDX expression (owned), filled by
   * prefetch_licenses() on the details-page refine only. */
  GHashTable *licenses;
} RefineData;

static void refine_data_free(RefineData *d) {
  g_clear_object(&d->list);
  g_clear_pointer(&d->licenses, g_hash_table_unref);
  g_free(d);
}

static const gchar *app_id_of(GsApp *app) {
  return g_object_get_data(G_OBJECT(app), "modulix::app_id");
}

/* ── per-app refine steps ───────────────────────────────────────────────── */

/* Addons (module plugins) are only needed by the details page
 * (GS_PLUGIN_REFINE_REQUIRE_FLAGS_ADDONS); building them costs a
 * full-namespace `nix eval`, so it must never happen on the search path. */
static void refine_addons(GsApp *app, GsPluginRefineRequireFlags require_flags,
                          GsPlugin *plugin) {
  if (!(require_flags & GS_PLUGIN_REFINE_REQUIRE_FLAGS_ADDONS))
    return;
  if (g_strcmp0(gs_modulix_app_kind(app), "module") != 0)
    return;

  const gchar *name = gs_modulix_app_name(app);
  if (name == NULL || *name == '\0')
    return;

  gint64 t0 = g_get_monotonic_time();
  gs_modulix_add_module_plugins(app, name, plugin);
  g_debug("[modulix] add_module_plugins(%s) %.0fms", name,
          (g_get_monotonic_time() - t0) / 1000.0);
}

/* ICON refine is local-only (theme/file-cache lookups, no network): handled
 * unconditionally here rather than folded into the DESCRIPTION/SCREENSHOTS
 * Flathub-fetch path. Only replayed when the app still has no icon at all —
 * e.g. an `alternate_of` row, whose backend entry carries `icon: None` (see
 * CLAUDE.md "Icons"). */
static void refine_icon(GsApp *app, GsPluginRefineRequireFlags require_flags,
                        const gchar *app_id) {
  if (!(require_flags & GS_PLUGIN_REFINE_REQUIRE_FLAGS_ICON))
    return;
  if (gs_app_has_icons(app))
    return;

  const gchar *icon_name = g_object_get_data(G_OBJECT(app), "modulix::icon_name");
  const gchar *base_name = g_object_get_data(G_OBJECT(app), "modulix::base_name");
  const gchar *icon = g_object_get_data(G_OBJECT(app), "modulix::icon");

  if (gs_modulix_icon_resolve(app, app_id, icon_name, gs_modulix_app_name(app),
                              base_name, icon))
    g_object_set_data(G_OBJECT(app), "modulix::icon-themed", GINT_TO_POINTER(1));
}

/* A NULL description is enough on its own to hide an app: the Installed page
 * drops every row that has none (`gs_installed_page_is_actual_app`,
 * src/gs-installed-page.c). Ours only ever comes from Flathub enrichment,
 * which the early returns in refine_one() skip entirely for an app with no
 * app_id — so a nix package with no Flatpak counterpart vanished from the
 * Installed list altogether. nixpkgs has nothing better to offer either
 * (`meta.longDescription` is empty for these attributes), so seed the summary
 * at LOWEST quality: `gs_app_set_description` (lib/gs-app.c) ignores a write
 * of lower quality than the one stored, so the Flathub description set at
 * NORMAL still wins whenever there is one. */
static void seed_description_from_summary(GsApp *app,
                                          GsPluginRefineRequireFlags flags) {
  if (!(flags & GS_PLUGIN_REFINE_REQUIRE_FLAGS_DESCRIPTION) &&
      gs_app_get_description(app) != NULL)
    return;

  const gchar *summary = gs_app_get_summary(app);
  if (summary == NULL || *summary == '\0')
    return;

  /* Unlike the summary, the description is rendered as Pango markup
   * (gs_description_box_set_text -> gtk_label_set_markup), so a bare `&` or
   * `<` in meta.description -- common enough -- would fail the markup parser
   * and blank the whole block. The Flathub description takes the same route
   * through gs_modulix_html_to_pango(). */
  g_autofree gchar *escaped = g_markup_escape_text(summary, -1);
  gs_app_set_description(app, GS_APP_QUALITY_LOWEST, escaped);
}

/* The nixpkgs `meta.license` of the attribute we would install, set at HIGHEST
 * quality because it describes the exact build the user gets, and must
 * therefore override a Flathub license a previous (search page) refine of the
 * *same* GsApp may already have set at NORMAL. @licenses is only populated on
 * the details-page refine — see prefetch_licenses(). Returns TRUE when it won,
 * i.e. when the Flathub fallback must not be applied. */
static gboolean refine_license_from_nix(GsApp *app,
                                        GsPluginRefineRequireFlags flags,
                                        GHashTable *licenses) {
  if (!(flags & GS_PLUGIN_REFINE_REQUIRE_FLAGS_LICENSE) || licenses == NULL)
    return FALSE;

  const gchar *attr = gs_modulix_app_name(app);
  const gchar *license =
      attr != NULL ? g_hash_table_lookup(licenses, attr) : NULL;
  if (license == NULL)
    return FALSE;

  gs_app_set_license(app, GS_APP_QUALITY_HIGHEST, license);
  return TRUE;
}

/* Flathub payload: description, screenshots, icon URL and the fallback
 * `project_license` (modules, and attributes `nix eval` could not resolve). */
static void refine_from_enrichment(GsApp *app, const gchar *app_id,
                                   gboolean license_set) {
  gint64 t0 = g_get_monotonic_time();
  g_autofree gchar *cached = gs_modulix_enrichment_cache_get_or_fetch(app_id);
  g_debug("[modulix] enrichment(%s) %.0fms", app_id,
          (g_get_monotonic_time() - t0) / 1000.0);
  if (cached == NULL)
    return;

  g_autoptr(JsonParser) parser = json_parser_new();
  JsonObject *obj = gs_modulix_json_parse_object(parser, cached, "refine");
  if (obj == NULL)
    return;

  const gchar *html_desc = gs_modulix_json_str(obj, "description");
  const gchar *icon = gs_modulix_json_str(obj, "icon");
  const gchar *license = gs_modulix_json_str(obj, "license");

  if (!license_set && license && *license)
    gs_app_set_license(app, GS_APP_QUALITY_NORMAL, license);

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

/* The enrichment payload carries the fallback license too, so a LICENSE-only
 * refine still has to fetch it — unless the nixpkgs license already won. */
static gboolean want_enrichment(GsPluginRefineRequireFlags flags,
                                gboolean license_set) {
  if (flags & (GS_PLUGIN_REFINE_REQUIRE_FLAGS_DESCRIPTION |
               GS_PLUGIN_REFINE_REQUIRE_FLAGS_SCREENSHOTS))
    return TRUE;
  return (flags & GS_PLUGIN_REFINE_REQUIRE_FLAGS_LICENSE) && !license_set;
}

static void refine_one(GsApp *app, GsPluginRefineRequireFlags require_flags,
                       GsPlugin *plugin, GHashTable *licenses) {
  const gchar *app_id = app_id_of(app);

  if (g_strcmp0(gs_modulix_app_kind(app), "plugin") == 0)
    return;

  refine_addons(app, require_flags, plugin);
  refine_icon(app, require_flags, app_id);
  seed_description_from_summary(app, require_flags);

  gboolean license_set = refine_license_from_nix(app, require_flags, licenses);

  if (!want_enrichment(require_flags, license_set))
    return;
  if (app_id == NULL || *app_id == '\0')
    return;

  refine_from_enrichment(app, app_id, license_set);
}

/* ── batched prefetches ─────────────────────────────────────────────────── */

/* Collects the distinct values @key_fn returns over the entries of @list we
 * manage whose kind passes @kind_filter (NULL = any non-plugin kind). The
 * returned array borrows the strings from the apps. */
static GPtrArray *collect_distinct(GsAppList *list, GsPlugin *plugin,
                                   const gchar *kind_filter,
                                   const gchar *(*key_fn)(GsApp *app)) {
  GPtrArray *keys = g_ptr_array_new(); /* borrowed const gchar* */
  g_autoptr(GHashTable) seen =
      g_hash_table_new(g_str_hash, g_str_equal); /* borrowed keys */

  for (guint i = 0; i < gs_app_list_length(list); i++) {
    GsApp *app = gs_app_list_index(list, i);
    if (!gs_modulix_app_is_ours(app, plugin))
      continue;

    const gchar *kind = gs_modulix_app_kind(app);
    if (kind_filter != NULL ? g_strcmp0(kind, kind_filter) != 0
                            : g_strcmp0(kind, "plugin") == 0)
      continue;

    const gchar *key = key_fn(app);
    if (key == NULL || *key == '\0')
      continue;
    if (g_hash_table_add(seen, (gpointer)key))
      g_ptr_array_add(keys, (gpointer)key);
  }

  return keys;
}

/* Warms the enrichment cache for every app-id of @list needing enrichment, in
 * one batched backend call, so the per-app fetches in refine_one() become
 * cache hits. */
static void prefetch_enrichment(GsAppList *list, GsPlugin *plugin) {
  g_autoptr(GPtrArray) ids = collect_distinct(list, plugin, NULL, app_id_of);

  if (ids->len > 0)
    gs_modulix_enrichment_cache_prefetch_many((const gchar *const *)ids->pdata,
                                              ids->len);
}

/* Fills @licenses (nix attr → SPDX, both owned) for every nix package of
 * @list we manage, in one batched daemon call.
 *
 * Only ever called on a details-page refine (see refine_thread): the daemon
 * runs one `nix eval` per uncached attribute, which the search page — which
 * also asks for LICENSE, over its whole result list — must never trigger.
 * Search results keep the Flathub license from the enrichment payload.
 * Modules and module plugins have no nixpkgs attribute to evaluate. */
static void prefetch_licenses(GsAppList *list, GsPlugin *plugin,
                              GHashTable *licenses) {
  g_autoptr(GPtrArray) attrs =
      collect_distinct(list, plugin, "package", gs_modulix_app_name);
  if (attrs->len == 0)
    return;

  gint64 t0 = g_get_monotonic_time();
  gchar *json =
      mx_store_package_licenses((const gchar *const *)attrs->pdata, attrs->len);
  g_debug("[modulix] licenses(n=%u) %.0fms", attrs->len,
          (g_get_monotonic_time() - t0) / 1000.0);
  if (json == NULL)
    return;

  g_autoptr(JsonParser) parser = json_parser_new();
  JsonObject *obj = gs_modulix_json_parse_object(parser, json, "licenses");
  mx_store_free_string(json);
  if (obj == NULL)
    return;

  g_autoptr(GList) members = json_object_get_members(obj);
  for (GList *l = members; l != NULL; l = l->next) {
    const gchar *attr = l->data;
    const gchar *license = gs_modulix_json_str(obj, attr);
    if (license != NULL && *license != '\0')
      g_hash_table_insert(licenses, g_strdup(attr), g_strdup(license));
  }
}

/* ── vfunc ──────────────────────────────────────────────────────────────── */

#define MODULIX_REFINE_FLAGS_WE_HANDLE                                         \
  (GS_PLUGIN_REFINE_REQUIRE_FLAGS_DESCRIPTION |                                \
   GS_PLUGIN_REFINE_REQUIRE_FLAGS_SCREENSHOTS |                                \
   GS_PLUGIN_REFINE_REQUIRE_FLAGS_ADDONS |                                     \
   GS_PLUGIN_REFINE_REQUIRE_FLAGS_LICENSE |                                    \
   GS_PLUGIN_REFINE_REQUIRE_FLAGS_ICON)

static void refine_thread(GTask *task, gpointer source_object G_GNUC_UNUSED,
                          gpointer task_data_ptr, GCancellable *cancellable) {
  RefineData *data = task_data_ptr;
  GsAppList *list = data->list;

  if (data->require_flags & (GS_PLUGIN_REFINE_REQUIRE_FLAGS_DESCRIPTION |
                             GS_PLUGIN_REFINE_REQUIRE_FLAGS_SCREENSHOTS))
    prefetch_enrichment(list, data->plugin);

  /* ADDONS is the details page's own marker (the search page never asks for
   * it), and the details page is the only caller allowed to pay for a per-
   * attribute `nix eval` — see prefetch_licenses(). */
  if ((data->require_flags & GS_PLUGIN_REFINE_REQUIRE_FLAGS_LICENSE) &&
      (data->require_flags & GS_PLUGIN_REFINE_REQUIRE_FLAGS_ADDONS)) {
    data->licenses =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    prefetch_licenses(list, data->plugin, data->licenses);
  }

  for (guint i = 0; i < gs_app_list_length(list); i++) {
    g_autoptr(GError) err = NULL;
    if (g_cancellable_set_error_if_cancelled(cancellable, &err)) {
      g_task_return_error(task, g_steal_pointer(&err));
      return;
    }
    GsApp *app = gs_app_list_index(list, i);
    if (gs_modulix_app_is_ours(app, data->plugin))
      refine_one(app, data->require_flags, data->plugin, data->licenses);
  }
  g_task_return_boolean(task, TRUE);
}

void gs_modulix_refine_async(GsPlugin *plugin, GsAppList *list,
                             GsPluginRefineRequireFlags require_flags,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data,
                             gpointer source_tag) {
  GTask *task = g_task_new(plugin, cancellable, callback, user_data);
  g_task_set_source_tag(task, source_tag);

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

  RefineData *data = g_new0(RefineData, 1);
  data->plugin = plugin;
  data->list = g_object_ref(list);
  data->require_flags = require_flags;

  g_task_set_task_data(task, data, (GDestroyNotify)refine_data_free);
  g_task_run_in_thread(task, refine_thread);
  g_object_unref(task);
}

gboolean gs_modulix_refine_finish(GAsyncResult *result, GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}
