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
  /* A GsApp now outlives the query that produced it (see the plugin cache in
   * gs_modulix_make_app_from_json), so refine can run on one that already has
   * its screenshots — appending would show each of them twice. */
  GPtrArray *existing = gs_app_get_screenshots(app);
  if (existing != NULL && existing->len > 0)
    return;
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

/* States owned by an install/uninstall already under way (`enqueue_lifecycle`
 * in gs-modulix-lifecycle.c sets them on the main thread, the drain worker
 * resolves them later). A listing that happens to run in between must not
 * stamp the daemon's — necessarily pre-operation — state over them. */
static gboolean state_is_transient(GsAppState state) {
  switch (state) {
  case GS_APP_STATE_INSTALLING:
  case GS_APP_STATE_REMOVING:
  case GS_APP_STATE_QUEUED_FOR_INSTALL:
  case GS_APP_STATE_PURCHASING:
  case GS_APP_STATE_DOWNLOADING:
    return TRUE;
  default:
    return FALSE;
  }
}

GsApp *gs_modulix_make_app_from_json(JsonObject *obj, GsPlugin *plugin,
                                     gboolean is_installed) {
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

  /* The daemon stamps `installed` on every entry it emits, on every path
   * (modulix-daemon/src/store/entry.rs). `is_installed` is only the fallback
   * for a daemon predating that field: before it, the caller's path decided
   * the state, so search results and Sources-popover rows were born
   * AVAILABLE no matter what the system actually had installed. */
  gboolean installed = json_object_has_member(obj, "installed")
                           ? gs_modulix_json_bool(obj, "installed")
                           : is_installed;

  /* The GsApp id is the backend's grouping key (group_id): the AppStream id
   * when known, else the pname — this is what stacks same-pname variants and a
   * package's extra outputs into one row. Falls back to app_id then name. */
  const gchar *app_unique_id = (group_id && *group_id) ? group_id
                               : (app_id && *app_id)    ? app_id
                                                        : name;
  /* One GsApp per (kind, install identifier), reused across queries through
   * the plugin cache — the same pattern gs-plugin-flatpak/packagekit/epiphany
   * use. The key is deliberately *not* the GsApp id: the alternate_of path
   * emits several entries sharing one id so they stack in the Sources popover
   * (see gs-modulix-list.c). `name` — the nix attribute or module name — is
   * what is unique per entry, and what install/uninstall act on. Same \x1f
   * separator as gs-modulix-lifecycle.c's run_group()'s dedup key.
   *
   * Reuse is what keeps state alive across queries. gs-details-page.c swaps
   * its displayed app for the matching row of the alternate_of reply
   * (_set_app() in gs_details_page_get_alternates_cb), and that reply is
   * re-issued when the page reloads after an install: a freshly built
   * instance would arrive AVAILABLE and flip the button back to "Install"
   * the moment the install finished.
   *
   * gs_plugin_cache_{lookup,add} each take the loader's cache mutex, but the
   * miss-then-insert pair must be atomic *as a whole*: gs_details_page_reload
   * fires the refine job and the alternate_of list job in parallel, both
   * landing on worker threads, so two threads can miss the same key, both
   * gs_app_new(), and the second gs_plugin_cache_add() evict the first — the
   * UI would then hold an instance the cache no longer hands out, which is
   * exactly the stale-state bug the cache exists to prevent. */
  g_autofree gchar *cache_key =
      g_strdup_printf("%s\x1f%s", (kind && *kind) ? kind : "package", name);
  static GMutex cache_key_mutex;
  GsApp *app;
  {
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&cache_key_mutex);
    app = gs_plugin_cache_lookup(plugin, cache_key);
    if (app == NULL) {
      app = gs_app_new(app_unique_id);
      gs_plugin_cache_add(plugin, cache_key, app);
    }
  }

  gs_app_set_management_plugin(app, plugin);

  const gchar *display_name = (a_name && *a_name)   ? a_name
                              : (pname && *pname)    ? pname
                                                      : name;
  gs_app_set_name(app, GS_APP_QUALITY_NORMAL, display_name);
  gs_app_set_summary(app, GS_APP_QUALITY_NORMAL, summary);
  gs_app_set_kind(app, AS_COMPONENT_KIND_DESKTOP_APP);
  gs_app_set_scope(app, AS_COMPONENT_SCOPE_SYSTEM);
  if (!state_is_transient(gs_app_get_state(app)))
    gs_app_set_state(app, installed ? GS_APP_STATE_INSTALLED
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

  /* Only ever once per GsApp: the resolver's contract is that an app carries
   * exactly one GIcon (see CLAUDE.md, "Icons"), which replaying it on a
   * cached instance would break — a second, differently-sized icon would let
   * gs_app_get_icon_for_size()'s first pass win over the themed one. */
  if (!gs_app_has_icons(app) &&
      gs_modulix_icon_resolve(app, app_id, icon_name, name, base_name, icon))
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

/* ── ownership / kind accessors ─────────────────────────────────────────── */

gboolean gs_modulix_app_is_ours(GsApp *app, GsPlugin *plugin) {
  return gs_app_has_management_plugin(app, plugin);
}

const gchar *gs_modulix_app_kind(GsApp *app) {
  return g_object_get_data(G_OBJECT(app), "modulix::kind");
}

const gchar *gs_modulix_app_name(GsApp *app) {
  return g_object_get_data(G_OBJECT(app), "modulix::name");
}

/* ── module plugins ─────────────────────────────────────────────────────── */

void gs_modulix_add_module_plugins(GsApp *module_app, const gchar *module_name,
                                   GsPlugin *plugin) {
  /* Same reason as gs_modulix_add_app_screenshots(): a cached GsApp can be
   * refined more than once. There is no public addon getter
   * (gs_app_dup_addons lives in the unexported gs-app-private.h), so the
   * "already done" bit is kept on the app itself. */
  if (g_object_get_data(G_OBJECT(module_app), "modulix::addons-added"))
    return;

  g_autofree gchar *json = gs_modulix_plugins_cache_get_or_fetch(module_name);
  if (json == NULL)
    return;

  g_autoptr(JsonParser) parser = json_parser_new();
  JsonArray *array = gs_modulix_json_parse_array(parser, json, "plugins");
  if (array == NULL)
    return;

  g_autoptr(GsAppList) addons = gs_app_list_new();
  for (guint i = 0; i < json_array_get_length(array); i++) {
    JsonObject *obj = json_array_get_object_element(array, i);
    const gchar *pname = gs_modulix_json_str(obj, "name");
    const gchar *desc = gs_modulix_json_str(obj, "description");
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
    g_object_set_data_full(G_OBJECT(addon), "modulix::kind", g_strdup("plugin"),
                           g_free);
    g_object_set_data_full(G_OBJECT(addon), "modulix::parent",
                           g_strdup(module_name), g_free);
    g_object_set_data_full(G_OBJECT(addon), "modulix::plugin_name",
                           g_strdup(pname), g_free);
    gs_app_list_add(addons, addon);
    g_object_unref(addon);
  }

  if (gs_app_list_length(addons) > 0) {
    gs_app_add_addons(module_app, addons);
    g_object_set_data(G_OBJECT(module_app), "modulix::addons-added",
                      GINT_TO_POINTER(1));
  }
}

/* ── app list population ────────────────────────────────────────────────── */

void gs_modulix_append_apps_from_json(GsAppList *list, const gchar *json,
                                      GsPlugin *plugin, gboolean is_installed,
                                      JsonParser *parser,
                                      GHashTable *seen_ids) {
  JsonArray *array = gs_modulix_json_parse_array(parser, json, "apps");
  if (array == NULL)
    return;

  for (guint i = 0; i < json_array_get_length(array); i++) {
    JsonObject *obj = json_array_get_object_element(array, i);
    GsApp *app = gs_modulix_make_app_from_json(obj, plugin, is_installed);
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
