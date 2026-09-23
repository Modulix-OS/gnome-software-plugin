/**
 * @file gs-modulix-bus.h
 * @brief The single system-bus connection to `mx-daemon`, and the generic
 *   synchronous call helper every `dbus/` wrapper is built on.
 *
 * Replaces `modulix-store-client`'s `mx_store_init`/`mx_store_shutdown`: this
 * plugin now talks GDBus/GVariant directly to `org.modulix.Daemon`
 * (`/org/modulix/Daemon`, system bus), never JSON.
 */
#pragma once

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

/**
 * @brief Well-known bus name `mx-daemon` owns, and the object path both of
 *   its interfaces (`org.modulix.Store1`, `org.modulix.Daemon`) are served
 *   at. Shared by gs-modulix-store1.c and gs-modulix-daemon1.c.
 */
#define MODULIX_BUS_NAME "org.modulix.Daemon"
#define MODULIX_OBJECT_PATH "/org/modulix/Daemon"

/**
 * @brief Opens (or reuses) the system-bus connection used by every
 *   `gs_modulix_bus_call()`.
 *
 * @param error Set on failure (bus unreachable). May be NULL.
 * @pre None. Safe to call once, from `setup_async` on the main thread, per
 *   the same contract `mx_store_init()` used to have.
 * @post Idempotent: a call after a successful one returns TRUE immediately
 *   without reconnecting. `GDBusConnection` is thread-safe for method calls,
 *   so every `dbus/` wrapper may be called from any worker thread afterwards.
 * @return TRUE once a live connection is held, FALSE otherwise.
 */
gboolean gs_modulix_bus_init(GError **error);

/**
 * @brief Releases the system-bus connection.
 *
 * @pre Called once, from plugin finalize. No `gs_modulix_bus_call()` may run
 *   concurrently with or after this.
 * @post The connection is unreffed and the internal pointer cleared.
 */
void gs_modulix_bus_shutdown(void);

/**
 * @brief Synchronous call to one method of `mx-daemon`'s
 *   `org.modulix.Store1`/`org.modulix.Daemon` interface, unwrapped to its
 *   single return value.
 *
 * @param iface Interface name (`"org.modulix.Store1"` or
 *   `"org.modulix.Daemon"`).
 * @param method D-Bus method name (e.g. `"SearchPackages"`).
 * @param params (transfer floating) The method's arguments as a tuple
 *   `GVariant` (e.g. `g_variant_new("(su)", query, max)`), or NULL for a
 *   no-argument method. Sunk and consumed by this call either way.
 * @param reply_type Expected reply signature, wrapped in a tuple (e.g.
 *   `G_VARIANT_TYPE("(aa{sv})")` for a method returning one `aa{sv})`. Used
 *   to validate the reply; NULL to skip validation.
 * @param timeout_ms Call timeout in milliseconds, or `G_MAXINT` for "no
 *   timeout" (a write call whose rebuild can take minutes).
 * @param error Set on failure (no connection, D-Bus error, polkit refusal,
 *   reply not matching @p reply_type). May be NULL.
 * @pre gs_modulix_bus_init() must have succeeded.
 * @post None beyond the D-Bus round-trip; blocks the calling thread for its
 *   duration.
 * @return (transfer full) (nullable): the reply tuple's first (and only)
 *   child, e.g. the `aa{sv}` array itself rather than the `(aa{sv})` tuple
 *   wrapping it — every method this plugin calls returns exactly one value.
 *   NULL on failure, with @p error set.
 */
GVariant *gs_modulix_bus_call(const gchar *iface, const gchar *method,
                              GVariant *params,
                              const GVariantType *reply_type, gint timeout_ms,
                              GError **error);

G_END_DECLS
