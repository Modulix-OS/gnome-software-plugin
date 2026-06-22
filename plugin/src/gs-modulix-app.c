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
#include "gs-modulix-enrichment-cache.h"
#include "gs-modulix-icon-cache.h"
#include "gs-modulix-json-utils.h"

#include "backend.h"
#include <glib/gi18n-lib.h>

/* ── internal accessors ─────────────────────────────────────────────────── */

static const gchar *app_kind(GsApp *app) {
  return g_object_get_data(G_OBJECT(app), "modulix::kind");
}

static const gchar *app_name_data(GsApp *app) {
  return g_object_get_data(G_OBJECT(app), "modulix::name");
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

/* ── GsApp construction ─────────────────────────────────────────────────── */

GsApp *gs_modulix_make_app_from_json(JsonObject *obj, GsPlugin *plugin,
                                     gboolean is_installed) {
  const gchar *name      = gs_modulix_json_str(obj, "name");
  const gchar *base_name = gs_modulix_json_str(obj, "base_name");
  const gchar *pname     = gs_modulix_json_str(obj, "pname");
  const gchar *a_name    = gs_modulix_json_str(obj, "app_name");
  const gchar *summary   = gs_modulix_json_str(obj, "summary");
  const gchar *version   = gs_modulix_json_str(obj, "version");
  const gchar *app_id    = gs_modulix_json_str(obj, "app_id");
  const gchar *icon      = gs_modulix_json_str(obj, "icon");
  const gchar *kind      = gs_modulix_json_str(obj, "kind");
  gboolean fp_pref       = gs_modulix_json_bool(obj, "flatpak_preferred");
  gint prio              = gs_modulix_json_int(obj, "priority");

  if (!name || !*name)
    return NULL;

  if (!base_name || !*base_name)
    base_name = name;

  gboolean is_module = (g_strcmp0(kind, "module") == 0);

  const gchar *app_unique_id = (app_id && *app_id) ? app_id : name;
  GsApp *app = gs_app_new(app_unique_id);

  gs_app_set_management_plugin(app, plugin);

  const gchar *fallback_name = (pname && *pname) ? pname : name;
  g_autofree gchar *display_name = NULL;
  if (!is_module && app_id && *app_id && a_name && *a_name)
    display_name = g_strdup_printf("%s (%s)", a_name, fallback_name);
  gs_app_set_name(app, GS_APP_QUALITY_NORMAL,
                  display_name ? display_name : fallback_name);
  gs_app_set_summary(app, GS_APP_QUALITY_NORMAL, summary);
  gs_app_set_kind(app, AS_COMPONENT_KIND_DESKTOP_APP);
  gs_app_set_scope(app, AS_COMPONENT_SCOPE_SYSTEM);
  gs_app_set_state(app, is_installed ? GS_APP_STATE_INSTALLED
                                     : GS_APP_STATE_AVAILABLE);
  gs_app_set_bundle_kind(app, AS_BUNDLE_KIND_PACKAGE);

  g_object_set_data(G_OBJECT(app), "modulix::priority", GINT_TO_POINTER(prio));

  if (version && *version)
    gs_app_set_version(app, version);

  /* Icon: prefer field value, fall back to cross-instance cache. */
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

  if (icon && *icon) {
    g_autoptr(GIcon) remote = gs_remote_icon_new(icon);
    gs_icon_set_width(remote, 256);
    gs_icon_set_height(remote, 256);
    gs_app_add_icon(app, remote);

    g_autoptr(GIcon) fallback = g_themed_icon_new(base_name);
    if (app_id && *app_id)
      g_themed_icon_append_name(G_THEMED_ICON(fallback), app_id);
    g_themed_icon_append_name(G_THEMED_ICON(fallback), "application-x-executable");
    gs_app_add_icon(app, fallback);
  } else {
    g_autoptr(GIcon) gicon = g_themed_icon_new(base_name);
    if (app_id && *app_id)
      g_themed_icon_append_name(G_THEMED_ICON(gicon), app_id);
    g_themed_icon_append_name(G_THEMED_ICON(gicon), "application-x-executable");
    gs_app_add_icon(app, gicon);
  }

  gs_app_set_origin(app, is_module ? "modulix" : name);
  gs_app_set_origin_hostname(app, "nixos.org");

  if (!is_module) {
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
  if (fp_pref)
    g_object_set_data(G_OBJECT(app), "modulix::flatpak_preferred",
                      GINT_TO_POINTER(1));

  return app;
}

/* ── module plugins ─────────────────────────────────────────────────────── */

void gs_modulix_add_module_plugins(GsApp *module_app, const gchar *module_name,
                                   GsPlugin *plugin) {
  gchar *json = backend_list_module_plugins(module_name);
  if (json == NULL)
    return;

  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(GError) err = NULL;
  if (!json_parser_load_from_data(parser, json, -1, &err)) {
    g_warning("[modulix] parse plugins JSON: %s", err->message);
    backend_free_string(json);
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
  backend_free_string(json);
}

/* ── app list population ────────────────────────────────────────────────── */

void gs_modulix_append_apps_from_json(GsAppList *list, const gchar *json,
                                      GsPlugin *plugin, gboolean is_installed,
                                      gboolean with_plugins,
                                      JsonParser *parser) {
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
    GsApp *app = gs_modulix_make_app_from_json(obj, plugin, is_installed);
    if (app == NULL)
      continue;
    if (with_plugins && g_strcmp0(app_kind(app), "module") == 0)
      gs_modulix_add_module_plugins(app, app_name_data(app), plugin);
    gs_app_list_add(list, app);
    g_object_unref(app);
  }
}
