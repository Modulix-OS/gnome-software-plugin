/**
 * @file gs-modulix-bus.c
 * @brief The single system-bus connection and the generic call helper; see
 *   the header for the full contract.
 */

#include "gs-modulix-bus.h"

/** @brief The process-wide system-bus connection, or NULL before init /
 *   after shutdown. */
static GDBusConnection *modulix_bus;

gboolean gs_modulix_bus_init(GError **error) {
  if (modulix_bus != NULL)
    return TRUE;

  modulix_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, error);
  return modulix_bus != NULL;
}

void gs_modulix_bus_shutdown(void) { g_clear_object(&modulix_bus); }

GVariant *gs_modulix_bus_call(const gchar *iface, const gchar *method,
                              GVariant *params,
                              const GVariantType *reply_type, gint timeout_ms,
                              GError **error) {
  if (modulix_bus == NULL) {
    g_clear_pointer(&params, g_variant_unref);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                        "modulix system-bus connection not initialised");
    return NULL;
  }

  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
      modulix_bus, MODULIX_BUS_NAME, MODULIX_OBJECT_PATH, iface, method,
      params, reply_type, G_DBUS_CALL_FLAGS_NONE, timeout_ms, NULL, error);
  if (reply == NULL)
    return NULL;

  return g_variant_get_child_value(reply, 0);
}
