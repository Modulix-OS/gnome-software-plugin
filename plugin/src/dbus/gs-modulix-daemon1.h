/**
 * @file gs-modulix-daemon1.h
 * @brief One wrapper per `org.modulix.Daemon` write method, each a
 *   synchronous `gs_modulix_bus_call()` unwrapped to the daemon's status
 *   string.
 *
 * Every call here is a full, polkit-gated NixOS rebuild carried out
 * daemon-side and can block for minutes — see `gs-modulix-lifecycle.c`,
 * the only caller. Unlike the old `mx_store_*` shim, a failure here reaches
 * the caller as a real `GError` (D-Bus error name + message), not an
 * undifferentiated NULL — `gs-modulix-lifecycle.c` still only surfaces a
 * generic status today, but the detail is now available for a future caller
 * that wants it.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * @brief `InstallPackage(as names) -> s` / `UninstallPackage(as names) -> s`
 *   / `InstallModule(as names) -> s` / `UninstallModule(as names) -> s`.
 *
 * One function serves all four: the D-Bus method name is the only
 * difference between them.
 *
 * @param method D-Bus method name (`"InstallPackage"`, `"UninstallPackage"`,
 *   `"InstallModule"` or `"UninstallModule"`).
 * @param names NULL-terminated array of package/module names.
 * @param error Set on failure (denied polkit authorisation, failed rebuild,
 *   D-Bus error). May be NULL.
 * @pre None beyond gs_modulix_bus_init() having succeeded.
 * @post Blocks for the whole rebuild (no timeout).
 * @return (transfer full) (nullable): the daemon's status string, which the
 *   caller must g_free(); NULL on failure, with @p error set.
 */
gchar *gs_modulix_daemon1_names_call(const gchar *method,
                                     const gchar *const *names,
                                     GError **error);

/**
 * @brief `InstallPlugin(s module, s plugin) -> s` /
 *   `UninstallPlugin(s module, s plugin) -> s`.
 *
 * @param method D-Bus method name (`"InstallPlugin"` or
 *   `"UninstallPlugin"`).
 * @param module Module key owning the plugin.
 * @param plugin Plugin key within that module.
 * @param error Set on failure, as in gs_modulix_daemon1_names_call().
 * @pre None beyond gs_modulix_bus_init() having succeeded.
 * @post Blocks for the whole rebuild (no timeout).
 * @return (transfer full) (nullable): the daemon's status string, which the
 *   caller must g_free(); NULL on failure, with @p error set.
 */
gchar *gs_modulix_daemon1_plugin_call(const gchar *method,
                                      const gchar *module,
                                      const gchar *plugin, GError **error);

/**
 * @brief `UpdateSystem(s mode) -> s`.
 *
 * @param mode `"switch"` (apply now, all cores), `"boot"` (apply on next boot,
 *   half the cores) or `"build"` (realise the new closure without activating
 *   anything, half the cores — the "download" step); treated as `""` if NULL.
 * @param error Set on failure, as in gs_modulix_daemon1_names_call().
 * @pre None beyond gs_modulix_bus_init() having succeeded.
 * @post Blocks for the whole `nix flake update` + `nixos-rebuild` (no
 *   timeout) — commonly minutes. `"build"` leaves the system, the boot entries
 *   and the configuration repository untouched, and leaves the daemon's
 *   candidate lockfile pending for the `"switch"`/`"boot"` that follows.
 * @return (transfer full) (nullable): the daemon's status string, which the
 *   caller must g_free(); NULL on failure, with @p error set.
 */
gchar *gs_modulix_daemon1_update_system(const gchar *mode, GError **error);

G_END_DECLS
