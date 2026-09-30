/**
 * @file gs-modulix-store1.c
 * @brief `org.modulix.Store1` method wrappers; see the header for the
 *   per-method contract.
 */

#include "gs-modulix-store1.h"
#include "gs-modulix-bus.h"

/**
 * @brief Shared body of every wrapper below: calls @p method with @p params,
 *   logging and returning NULL on failure.
 *
 * @param method D-Bus method name, used in the warning on failure.
 * @param params (transfer floating) Arguments tuple; consumed either way.
 * @param reply_type Expected reply signature, wrapped in a tuple.
 * @param timeout_ms Call timeout in milliseconds.
 * @return (transfer full) (nullable): the unwrapped reply, or NULL.
 */
static GVariant *store1_call(const gchar *method, GVariant *params,
                             const GVariantType *reply_type,
                             gint timeout_ms) {
  g_autoptr(GError) error = NULL;
  GVariant *result =
      gs_modulix_bus_call("org.modulix.Store1", method, params, reply_type,
                          timeout_ms, &error);
  if (result == NULL)
    g_warning("[modulix] Store1.%s: %s", method, error->message);
  return result;
}

GVariant *gs_modulix_store1_search_packages(const gchar *query, guint max) {
  return store1_call(
      "SearchPackages",
      g_variant_new("(su)", query ? query : "", max),
      G_VARIANT_TYPE("(aa{sv})"), MODULIX_STORE1_TIMEOUT_MS);
}

GVariant *gs_modulix_store1_search_modules(const gchar *query, guint max) {
  return store1_call(
      "SearchModules",
      g_variant_new("(su)", query ? query : "", max),
      G_VARIANT_TYPE("(aa{sv})"), MODULIX_STORE1_TIMEOUT_MS);
}

GVariant *gs_modulix_store1_list_installed_packages(void) {
  return store1_call("ListInstalledPackages", NULL,
                     G_VARIANT_TYPE("(aa{sv})"), MODULIX_STORE1_TIMEOUT_MS);
}

GVariant *gs_modulix_store1_list_installed_modules(void) {
  return store1_call("ListInstalledModules", NULL,
                     G_VARIANT_TYPE("(aa{sv})"), MODULIX_STORE1_TIMEOUT_MS);
}

GVariant *gs_modulix_store1_list_module_plugins(const gchar *module) {
  return store1_call("ListModulePlugins",
                     g_variant_new("(s)", module ? module : ""),
                     G_VARIANT_TYPE("(aa{sv})"), MODULIX_STORE1_TIMEOUT_MS);
}

GVariant *gs_modulix_store1_list_installed_plugins(const gchar *module) {
  return store1_call("ListInstalledPlugins",
                     g_variant_new("(s)", module ? module : ""),
                     G_VARIANT_TYPE("(aa{sv})"), MODULIX_STORE1_TIMEOUT_MS);
}

GVariant *gs_modulix_store1_packages_for_app_id(const gchar *app_id) {
  return store1_call("PackagesForAppId",
                     g_variant_new("(s)", app_id ? app_id : ""),
                     G_VARIANT_TYPE("(aa{sv})"), MODULIX_STORE1_TIMEOUT_MS);
}

GVariant *gs_modulix_store1_get_app_enrichment(const gchar *const *app_ids,
                                               guint n) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
  for (guint i = 0; i < n; i++)
    g_variant_builder_add(&builder, "s", app_ids[i] ? app_ids[i] : "");

  return store1_call("GetAppEnrichment",
                     g_variant_new("(as)", &builder),
                     G_VARIANT_TYPE("(a{sa{sv}})"), MODULIX_STORE1_TIMEOUT_MS);
}

GVariant *gs_modulix_store1_get_package_licenses(const gchar *const *attrs,
                                                 guint n) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
  for (guint i = 0; i < n; i++)
    g_variant_builder_add(&builder, "s", attrs[i] ? attrs[i] : "");

  return store1_call("GetPackageLicenses",
                     g_variant_new("(as)", &builder),
                     G_VARIANT_TYPE("(a{ss})"), MODULIX_STORE1_TIMEOUT_MS);
}

GVariant *gs_modulix_store1_list_outdated_inputs(gboolean force_refresh) {
  return store1_call("ListOutdatedInputs",
                     g_variant_new("(b)", force_refresh),
                     G_VARIANT_TYPE("(aa{sv})"),
                     force_refresh ? MODULIX_STORE1_REFRESH_TIMEOUT_MS
                                   : MODULIX_STORE1_TIMEOUT_MS);
}

gboolean gs_modulix_store1_check_update(void) {
  g_autoptr(GVariant) reply =
      store1_call("CheckUpdate", NULL, G_VARIANT_TYPE("(b)"),
                  MODULIX_STORE1_CHECK_TIMEOUT_MS);
  return reply != NULL && g_variant_get_boolean(reply);
}

GVariant *gs_modulix_store1_get_remote_release(void) {
  return store1_call("GetRemoteRelease", NULL, G_VARIANT_TYPE("(a{sv})"),
                     MODULIX_STORE1_TIMEOUT_MS);
}
