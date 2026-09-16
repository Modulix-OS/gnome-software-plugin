/*
 * gs-modulix-app.c — GsApp construction from Modulix JSON payloads
 *
 * Covers:
 *   - gs_modulix_make_app_from_json()       : single JSON object → GsApp
 *   - gs_modulix_append_apps_from_json()    : JSON array → GsAppList
 *   - gs_modulix_add_module_plugins()       : attach ADDON plugins to a module
 *   - gs_modulix_add_app_screenshots()      : attach AsScreenshot objects
 */

#ifndef I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#endif

#include "gs-modulix-app.h"
#include "gs-modulix-icon-cache.h"
#include "gs-modulix-icon-resolver.h"
#include "gs-modulix-json-utils.h"
#include "gs-modulix-plugins-cache.h"

#include <glib/gi18n-lib.h>

/* ── SortKey / match-value (ported from the former `backend` crate) ──────────
 *
 * The daemon (`modulix-daemon/src/store/entry.rs`) only emits neutral fields
 * (`kind`, `variant_rank`, `score`) — no GNOME-Software-specific number
 * crosses the bus. This is the one place that turns them into the two
 * GNOME Software conventions: `GnomeSoftware::SortKey` metadata (orders the
 * details-page "Sources" popover) and `GsApp::match-value` (orders the
 * search-results page). See CLAUDE.md "GsApp mapping & dedup". */

/* Lower sorts first in the "Sources" popover. A Modulix module must sort
 * below the lowest in-tree plugin value (flatpak: 100) to come first; a nix
 * variant sorts within PACKAGE_SORT_BASE + variant_rank, comfortably above
 * all of them (a Flatpak with none of this metadata falls back to the
 * patched gnome-software's default of 1000, still below a nix variant). */
#define MODULIX_MODULE_SORT_KEY 50
#define MODULIX_PACKAGE_SORT_BASE 2000

/* Maximum match-value reachable by AppStream once the id-match bit is
 * stripped (gs_appstream_add_search_hit, lib/gs-appstream.c). Nix packages
 * are scaled onto this same range so they interleave correctly with
 * Flatpak/AppStream hits in the search page's sort key. */
#define MODULIX_MATCH_SCALE_MAX 0x7F
/* score() value of a perfect name match: 1000 (exact) + 500 (substring at
 * position 0) + 200 (levenshtein distance 0). */
#define MODULIX_MATCH_EXACT_SCORE 1700
/* Floor reserved for modules: strictly above any AppStream/nix match value,
 * so a relevant module always sorts ahead of its own Flatpak. */
#define MODULIX_MATCH_MODULE_BONUS 0x80

/* Scales a raw relevance `score` (0..=MODULIX_MATCH_EXACT_SCORE) into the
 * GsApp::match-value range used by the search page's sort key
 * (kind : state : match_value : rating : kudos). Packages land in
 * 1..=0x7F; modules get 0x80 added on top so they always precede a
 * package/Flatpak of equal relevance. */
static guint modulix_match_value(gint score, gboolean is_module) {
  gint capped = MIN(score, MODULIX_MATCH_EXACT_SCORE);
  guint scaled =
      MAX(1u, (guint)((capped * MODULIX_MATCH_SCALE_MAX) / MODULIX_MATCH_EXACT_SCORE));
  return is_module ? MODULIX_MATCH_MODULE_BONUS + scaled : scaled;
}

/* ── screenshots ────────────────────────────────────────────────────────── */

void gs_modulix_add_app_screenshots(GsApp *app, JsonObject *meta) {
  if (!json_object_has_member(meta, "screenshots"))
    return;
  JsonNode *node = json_object_get_member(meta, "screenshots");
  if (!JSON_NODE_HOLDS_ARRAY(node))
    return;

  JsonArray *shots = json_node_get_array(node);
  for (guint i = 0; i < json_array_get_length(shots); i++) {
    JsonObject *shot = json_array_get_object_element(shots, i);
    const gchar *caption = gs_modulix_json_str(shot, "caption");
    gboolean is_default = gs_modulix_json_bool(shot, "default");

    if (!json_object_has_member(shot, "images"))
      continue;
    JsonNode *imgs_node = json_object_get_member(shot, "images");
    if (!JSON_NODE_HOLDS_ARRAY(imgs_node))
      continue;
    JsonArray *imgs = json_node_get_array(imgs_node);
    if (json_array_get_length(imgs) == 0)
      continue;

    g_autoptr(AsScreenshot) ss = as_screenshot_new();
    as_screenshot_set_kind(ss, is_default ? AS_SCREENSHOT_KIND_DEFAULT
                                          : AS_SCREENSHOT_KIND_EXTRA);
    if (caption && *caption)
      as_screenshot_set_caption(ss, caption, NULL);

    for (guint j = 0; j < json_array_get_length(imgs); j++) {
      JsonObject *img = json_array_get_object_element(imgs, j);
      const gchar *url = gs_modulix_json_str(img, "url");
      if (!url || !*url)
        continue;
      g_autoptr(AsImage) image = as_image_new();
      as_image_set_kind(image, AS_IMAGE_KIND_SOURCE);
      as_image_set_url(image, url);
      as_image_set_width(image, (guint)gs_modulix_json_int(img, "width"));
      as_image_set_height(image, (guint)gs_modulix_json_int(img, "height"));
      as_screenshot_add_image(ss, image);
    }
    gs_app_add_screenshot(app, ss);
  }
}

/* ── icons ──────────────────────────────────────────────────────────────── */

GIcon *gs_modulix_remote_icon_new(const gchar *url) {
  /* Flathub artwork is 128×128, and the icon downloader caps requests at
   * 160 logical px anyway (gs-plugin-icons.c) — a bigger claimed width used
   * to win gs_app_get_icon_for_size()'s first pass unconditionally, then get
   * silently corrected once downloaded, changing the displayed icon under
   * the user's eyes. */
  GIcon *remote = gs_remote_icon_new(url);
  gs_icon_set_width(remote, 128);
  gs_icon_set_height(remote, 128);
  return remote;
}

/* ── GsApp construction ─────────────────────────────────────────────────── */

GsApp *gs_modulix_make_app_from_json(JsonObject *obj, GsPlugin *plugin,
                                     gboolean is_installed,
                                     gboolean label_variant) {
  const gchar *name      = gs_modulix_json_str(obj, "name");
  const gchar *base_name = gs_modulix_json_str(obj, "base_name");
  const gchar *pname     = gs_modulix_json_str(obj, "pname");
  const gchar *a_name    = gs_modulix_json_str(obj, "app_name");
  const gchar *summary   = gs_modulix_json_str(obj, "summary");
  const gchar *version   = gs_modulix_json_str(obj, "version");
  const gchar *app_id    = gs_modulix_json_str(obj, "app_id");
  const gchar *group_id  = gs_modulix_json_str(obj, "group_id");
  const gchar *icon      = gs_modulix_json_str(obj, "icon");
  const gchar *icon_name = gs_modulix_json_str(obj, "icon_name");
  const gchar *kind      = gs_modulix_json_str(obj, "kind");
  gboolean fp_pref       = gs_modulix_json_bool(obj, "flatpak_preferred");
  gint variant_rank      = gs_modulix_json_int(obj, "variant_rank");
  gboolean has_score     = json_object_has_member(obj, "score");
  gint score             = has_score ? gs_modulix_json_int(obj, "score") : 0;

  if (!name || !*name)
    return NULL;

  if (!base_name || !*base_name)
    base_name = name;

  gboolean is_module = (g_strcmp0(kind, "module") == 0);

  /* The GsApp id is the backend's grouping key (group_id): the AppStream id
   * when known, else the pname — this is what stacks same-pname variants and a
   * package's extra outputs into one row. Falls back to app_id then name. */
  const gchar *app_unique_id = (group_id && *group_id) ? group_id
                               : (app_id && *app_id)    ? app_id
                                                        : name;
  GsApp *app = gs_app_new(app_unique_id);

  gs_app_set_management_plugin(app, plugin);

  const gchar *fallback_name = (pname && *pname) ? pname : name;
  g_autofree gchar *display_name = NULL;
  if (label_variant && !is_module && app_id && *app_id && a_name && *a_name)
    display_name = g_strdup_printf("%s (%s)", a_name, fallback_name);
  gs_app_set_name(app, GS_APP_QUALITY_NORMAL,
                  display_name ? display_name : fallback_name);
  gs_app_set_summary(app, GS_APP_QUALITY_NORMAL, summary);
  gs_app_set_kind(app, AS_COMPONENT_KIND_DESKTOP_APP);
  gs_app_set_scope(app, AS_COMPONENT_SCOPE_SYSTEM);
  gs_app_set_state(app, is_installed ? GS_APP_STATE_INSTALLED
                                     : GS_APP_STATE_AVAILABLE);
  gs_app_set_bundle_kind(app, AS_BUNDLE_KIND_PACKAGE);

  {
    gint sort_key = is_module ? MODULIX_MODULE_SORT_KEY
                              : MODULIX_PACKAGE_SORT_BASE + variant_rank;
    g_autofree gchar *sort_key_str = g_strdup_printf("%d", sort_key);
    gs_app_set_metadata(app, "GnomeSoftware::SortKey", sort_key_str);
  }

  if (has_score)
    gs_app_set_match_value(app, modulix_match_value(score, is_module));

  if (version && *version)
    gs_app_set_version(app, version);

  /* Icon: theme first, then local caches, then the remote URL (see
   * gs-modulix-icon-resolver.c). The url fed to the resolver prefers the
   * field value, falling back to the cross-instance cache when this entry
   * (e.g. an `alternate_of` row) carries none. */
  g_autofree gchar *cached_icon = NULL;
  if (app_id && *app_id) {
    if (icon && *icon) {
      gs_modulix_icon_cache_put(app_id, icon);
    } else {
      cached_icon = gs_modulix_icon_cache_get(app_id);
      if (cached_icon != NULL)
        icon = cached_icon;
    }
  }

  if (gs_modulix_icon_resolve(app, app_id, icon_name, name, base_name, icon))
    g_object_set_data(G_OBJECT(app), "modulix::icon-themed", GINT_TO_POINTER(1));

  gs_app_set_origin(app, is_module ? "modulix" : name);
  gs_app_set_origin_hostname(app, "nixos.org");

  if (is_module) {
    gs_app_set_origin_ui(app, _("Modulix OS"));
    gs_app_set_metadata(app, "GnomeSoftware::PackagingFormat", _("Module"));
    gs_app_set_metadata(app, "GnomeSoftware::PackagingIcon", "modulix-logo");
    gs_app_set_metadata(app, "GnomeSoftware::PackagingBaseCssColor", "accent_color");
  } else {
    gs_app_set_origin_ui(app, name);
    gs_app_set_metadata(app, "GnomeSoftware::PackagingFormat", _("Nix Package"));
    gs_app_set_metadata(app, "GnomeSoftware::PackagingIcon", "nix-snowflake");
    gs_app_set_metadata(app, "GnomeSoftware::PackagingBaseCssColor", "error_color");
  }

  g_object_set_data_full(G_OBJECT(app), "modulix::kind",
                         g_strdup(kind && *kind ? kind : "package"), g_free);
  g_object_set_data_full(G_OBJECT(app), "modulix::name", g_strdup(name), g_free);
  if (app_id && *app_id)
    g_object_set_data_full(G_OBJECT(app), "modulix::app_id",
                           g_strdup(app_id), g_free);
  if (icon && *icon)
    g_object_set_data_full(G_OBJECT(app), "modulix::icon", g_strdup(icon), g_free);
  if (icon_name && *icon_name)
    g_object_set_data_full(G_OBJECT(app), "modulix::icon_name",
                           g_strdup(icon_name), g_free);
  g_object_set_data_full(G_OBJECT(app), "modulix::base_name",
                         g_strdup(base_name), g_free);
  if (fp_pref)
    g_object_set_data(G_OBJECT(app), "modulix::flatpak_preferred",
                      GINT_TO_POINTER(1));

  return app;
}

/* ── module plugins ─────────────────────────────────────────────────────── */

void gs_modulix_add_module_plugins(GsApp *module_app, const gchar *module_name,
                                   GsPlugin *plugin) {
  g_autofree gchar *json = gs_modulix_plugins_cache_get_or_fetch(module_name);
  if (json == NULL)
    return;

  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(GError) err = NULL;
  if (!json_parser_load_from_data(parser, json, -1, &err)) {
    g_warning("[modulix] parse plugins JSON: %s", err->message);
    return;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (root && JSON_NODE_HOLDS_ARRAY(root)) {
    g_autoptr(GsAppList) addons = gs_app_list_new();
    JsonArray *array = json_node_get_array(root);
    for (guint i = 0; i < json_array_get_length(array); i++) {
      JsonObject *obj = json_array_get_object_element(array, i);
      const gchar *pname = gs_modulix_json_str(obj, "name");
      const gchar *desc  = gs_modulix_json_str(obj, "description");
      if (!pname || !*pname)
        continue;

      g_autofree gchar *id = g_strdup_printf("%s/%s", module_name, pname);
      GsApp *addon = gs_app_new(id);
      gs_app_set_management_plugin(addon, plugin);
      gs_app_set_name(addon, GS_APP_QUALITY_NORMAL, pname);
      gs_app_set_summary(addon, GS_APP_QUALITY_NORMAL, desc);
      gs_app_set_kind(addon, AS_COMPONENT_KIND_ADDON);
      gs_app_set_state(addon, GS_APP_STATE_AVAILABLE);
      gs_app_set_bundle_kind(addon, AS_BUNDLE_KIND_PACKAGE);
      g_object_set_data_full(G_OBJECT(addon), "modulix::kind",
                             g_strdup("plugin"), g_free);
      g_object_set_data_full(G_OBJECT(addon), "modulix::parent",
                             g_strdup(module_name), g_free);
      g_object_set_data_full(G_OBJECT(addon), "modulix::plugin_name",
                             g_strdup(pname), g_free);
      gs_app_list_add(addons, addon);
      g_object_unref(addon);
    }
    if (gs_app_list_length(addons) > 0)
      gs_app_add_addons(module_app, addons);
  }
}

/* ── app list population ────────────────────────────────────────────────── */

void gs_modulix_append_apps_from_json(GsAppList *list, const gchar *json,
                                      GsPlugin *plugin, gboolean is_installed,
                                      JsonParser *parser,
                                      GHashTable *seen_ids,
                                      gboolean label_variant) {
  if (json == NULL)
    return;

  g_autoptr(GError) err = NULL;
  if (!json_parser_load_from_data(parser, json, -1, &err)) {
    g_warning("[modulix] parse apps JSON: %s", err->message);
    return;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_ARRAY(root))
    return;

  JsonArray *array = json_node_get_array(root);
  for (guint i = 0; i < json_array_get_length(array); i++) {
    JsonObject *obj = json_array_get_object_element(array, i);
    GsApp *app =
        gs_modulix_make_app_from_json(obj, plugin, is_installed, label_variant);
    if (app == NULL)
      continue;
    if (seen_ids != NULL) {
      const gchar *id = gs_app_get_id(app);
      if (g_hash_table_contains(seen_ids, id)) {
        g_object_unref(app);
        continue;
      }
      g_hash_table_add(seen_ids, g_strdup(id));
    }
    /* Module plugins (addons) are attached later, in refine, only for the
     * details page (GS_PLUGIN_REFINE_REQUIRE_FLAGS_ADDONS) — building them
     * costs a full-namespace `nix eval` per module. */
    gs_app_list_add(list, app);
    g_object_unref(app);
  }
}
