/**
 * @file gs-modulix-daemon1.c
 * @brief `org.modulix.Daemon` write-method wrappers; see the header for the
 *   per-method contract.
 */

#include "gs-modulix-daemon1.h"
#include "gs-modulix-bus.h"

/**
 * @brief No timeout: a write is a full NixOS rebuild and can legitimately
 *   take minutes. The old shim's `daemon_call()` used to cap this at 120s,
 *   which was too short for a real rebuild; the shim itself had no timeout
 *   at all.
 */
#define MODULIX_DAEMON1_NO_TIMEOUT G_MAXINT

/**
 * @brief Extracts the daemon's status string from a `(s)`-typed reply.
 *
 * @param reply (transfer full) The `s` GVariant returned by
 *   gs_modulix_bus_call(), or NULL.
 * @return (transfer full) (nullable): a newly allocated copy of the string,
 *   or NULL if @p reply was NULL. @p reply is unreffed either way.
 */
static gchar *take_status(GVariant *reply) {
  if (reply == NULL)
    return NULL;
  gchar *status = g_variant_dup_string(reply, NULL);
  g_variant_unref(reply);
  return status;
}

gchar *gs_modulix_daemon1_names_call(const gchar *method,
                                     const gchar *const *names,
                                     GError **error) {
  GVariant *reply =
      gs_modulix_bus_call("org.modulix.Daemon", method,
                          g_variant_new("(^as)", names),
                          G_VARIANT_TYPE("(s)"), MODULIX_DAEMON1_NO_TIMEOUT,
                          error);
  return take_status(reply);
}

gchar *gs_modulix_daemon1_plugin_call(const gchar *method,
                                      const gchar *module,
                                      const gchar *plugin, GError **error) {
  GVariant *reply = gs_modulix_bus_call(
      "org.modulix.Daemon", method,
      g_variant_new("(ss)", module ? module : "", plugin ? plugin : ""),
      G_VARIANT_TYPE("(s)"), MODULIX_DAEMON1_NO_TIMEOUT, error);
  return take_status(reply);
}

gchar *gs_modulix_daemon1_update_system(const gchar *mode, GError **error) {
  GVariant *reply = gs_modulix_bus_call(
      "org.modulix.Daemon", "UpdateSystem",
      g_variant_new("(s)", mode ? mode : ""), G_VARIANT_TYPE("(s)"),
      MODULIX_DAEMON1_NO_TIMEOUT, error);
  return take_status(reply);
}
