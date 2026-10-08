/**
 * @file gs-modulix-store1.h
 * @brief One wrapper per `org.modulix.Store1` method (reads), each a
 *   synchronous `gs_modulix_bus_call()` returning the reply unwrapped to its
 *   `GVariant` payload.
 *
 * Every wrapper here replaces one `mx_store_*` JSON call. Field names/types
 * in the returned `a{sv}`/`aa{sv}`/`a{sa{sv}}`/`a{ss}` payloads match
 * `AppEntry`/`PluginEntry`/`EnrichEntry` in
 * `modulix-daemon/src/store/entry.rs` exactly — see that file for the
 * authoritative field-by-field wire contract (which key is always present,
 * which is a `s`/`b`/`i`/`u`, which is omitted when absent).
 *
 * A failed call (bus unreachable, D-Bus error) is logged here (`g_warning`)
 * and returns NULL — callers treat NULL exactly like an empty result, same
 * as the old shim's NULL-on-failure JSON strings, so no caller-side error
 * plumbing changes.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/** @brief Timeout for every read below: generous, but bounded, since a
 *   hung Store1 call must not wedge a worker thread forever. */
#define MODULIX_STORE1_TIMEOUT_MS 30000

/** @brief Timeout for gs_modulix_store1_list_outdated_inputs() when called
 *   with `force_refresh = TRUE`: that path runs one `nix flake metadata
 *   --refresh` per tracked input daemon-side, which the regular 30s budget
 *   is too short for. */
#define MODULIX_STORE1_REFRESH_TIMEOUT_MS 180000

/** @brief Timeout for gs_modulix_store1_check_update(): none. That call runs
 *   a full `nix flake update` daemon-side, refetching every input, which is
 *   legitimately minutes — the same reasoning as the write side's
 *   MODULIX_DAEMON1_NO_TIMEOUT. */
#define MODULIX_STORE1_CHECK_TIMEOUT_MS G_MAXINT

/**
 * @brief `SearchPackages(s query, u max) -> aa{sv}`.
 * @param query Free-text search terms; treated as `""` if NULL.
 * @param max Upper bound on the number of rows.
 * @return (transfer full) (nullable): the `aa{sv}` array, or NULL on failure.
 */
GVariant *gs_modulix_store1_search_packages(const gchar *query, guint max);

/**
 * @brief `SearchModules(s query, u max) -> aa{sv}`.
 * @param query Free-text search terms; treated as `""` if NULL.
 * @param max Upper bound on the number of rows.
 * @return (transfer full) (nullable): the `aa{sv}` array, or NULL on failure.
 */
GVariant *gs_modulix_store1_search_modules(const gchar *query, guint max);

/**
 * @brief `ListInstalledPackages() -> aa{sv}`.
 * @return (transfer full) (nullable): the `aa{sv}` array, or NULL on failure.
 */
GVariant *gs_modulix_store1_list_installed_packages(void);

/**
 * @brief `ListInstalledModules() -> aa{sv}`.
 * @return (transfer full) (nullable): the `aa{sv}` array, or NULL on failure.
 */
GVariant *gs_modulix_store1_list_installed_modules(void);

/**
 * @brief `ListModulePlugins(s module) -> aa{sv}`.
 * @param module Module key; treated as `""` if NULL.
 * @return (transfer full) (nullable): the `aa{sv}` array, or NULL on failure.
 */
GVariant *gs_modulix_store1_list_module_plugins(const gchar *module);

/**
 * @brief `ListInstalledPlugins(s module) -> aa{sv}`.
 * @param module Module key, or `""` to cover every enabled module; treated
 *   as `""` if NULL.
 * @return (transfer full) (nullable): the `aa{sv}` array, or NULL on failure.
 */
GVariant *gs_modulix_store1_list_installed_plugins(const gchar *module);

/**
 * @brief `PackagesForAppId(s app_id) -> aa{sv}`.
 * @param app_id AppStream component id; treated as `""` if NULL.
 * @return (transfer full) (nullable): the `aa{sv}` array, or NULL on failure.
 */
GVariant *gs_modulix_store1_packages_for_app_id(const gchar *app_id);

/**
 * @brief `GetAppEnrichment(as app_ids) -> a{sa{sv}}`.
 *
 * Covers both the single-id and batched shapes the old shim exposed as two
 * symbols (`mx_store_get_app_enrichment`/`_many`): there is only ever the
 * one D-Bus method, called with a one- or many-element array.
 *
 * @param app_ids Array of AppStream component ids. NULL only if @p n is 0.
 * @param n Number of entries in @p app_ids.
 * @return (transfer full) (nullable): the `a{sa{sv}}` dict (one entry per id
 *   the daemon had enrichment for — an id with none is simply absent, not
 *   mapped to an empty dict), or NULL on failure.
 */
GVariant *gs_modulix_store1_get_app_enrichment(const gchar *const *app_ids,
                                               guint n);

/**
 * @brief `GetPackageLicenses(as attrs) -> a{ss}`.
 * @param attrs Array of nixpkgs attribute paths. NULL only if @p n is 0.
 * @param n Number of entries in @p attrs.
 * @return (transfer full) (nullable): the `a{ss}` dict (attribute → SPDX
 *   expression; an attribute with none is simply absent), or NULL on
 *   failure.
 */
GVariant *gs_modulix_store1_get_package_licenses(const gchar *const *attrs,
                                                 guint n);

/**
 * @brief `ListOutdatedInputs(b force_refresh) -> aa{sv}`.
 *
 * Each row carries `"input"` (`s`), `"current_rev"` (`s`), `"new_rev"` (`s`)
 * and `"last_modified"` (`t`, Unix seconds) — see `AppEntry`-sibling shape
 * documented in `modulix-daemon/src/store/entry.rs`. An empty array means
 * the system is up to date; this call never returns a D-Bus error for a
 * legitimately empty result.
 *
 * @param force_refresh FALSE serves the daemon's 1h cache; TRUE forces one
 *   `nix flake metadata --refresh` per tracked input daemon-side (slow —
 *   see MODULIX_STORE1_REFRESH_TIMEOUT_MS, used automatically when this is
 *   TRUE).
 * @return (transfer full) (nullable): the `aa{sv}` array, or NULL on
 *   failure (bus unreachable, D-Bus error).
 */
GVariant *gs_modulix_store1_list_outdated_inputs(gboolean force_refresh);

/**
 * @brief `CheckUpdate() -> b`: whether refreshing every flake input would
 *   change anything.
 *
 * Unlike gs_modulix_store1_list_outdated_inputs(), which only reports, this
 * call *prepares* the update: the daemon resolves the whole flake into a
 * candidate `flake.lock`, keeps it in RAM, and the next
 * gs_modulix_daemon1_update_system() writes that exact lockfile. It also
 * refills the daemon's outdated-inputs cache from a local diff of the two
 * lockfiles, so a gs_modulix_store1_list_outdated_inputs() right after is
 * free and consistent with what an update would apply.
 *
 * Blocking and **slow** — a full `nix flake update`, minutes rather than
 * seconds (see MODULIX_STORE1_CHECK_TIMEOUT_MS). Call it only from a worker
 * thread, and only on an explicit "look for updates" path; merely displaying
 * the Updates page must not.
 *
 * @return TRUE when an update is available, FALSE when the system is current
 *   or the call failed (a failure is logged, then reported as "nothing to
 *   update" — same NULL-is-empty convention as the reads above).
 */
gboolean gs_modulix_store1_check_update(void);

/**
 * @brief `RebootRequired() -> (bas)`: whether the running system still needs a
 *   reboot.
 *
 * A `Daemon.UpdateSystem("switch")` replaces the system closure under the live
 * session, but not the kernel, the kernel modules or the initrd the machine is
 * *running*. This call is how the plugin tells "updated, nothing left to do"
 * from "updated, restart to pick up the new kernel" once a switch has
 * succeeded. The daemon answers exactly, by comparing `/run/booted-system`
 * with `/run/current-system`, not by guessing from package names.
 *
 * It answers for the running system, not for an update: a machine switched and
 * never rebooted keeps reporting TRUE however long ago that was.
 *
 * Blocking, but cheap: the daemon memoises the answer per activation. The
 * first call after a switch spawns `nix store diff-closures`, so call it from
 * the worker thread that ran the update, not from the main loop.
 *
 * The reply's second member, the `as` describing what moved since boot, is
 * **not** returned here: gs_modulix_bus_call() unwraps a reply to its first
 * child, so plumbing it through would mean a second unwrap convention for one
 * value the plugin has nowhere to show. It stays on the bus for `mx` and
 * d-spy.
 *
 * @return TRUE when a reboot is needed, FALSE when it is not or the call
 *   failed (a failure is logged, then reported as "no reboot needed" — same
 *   NULL-is-empty convention as the reads above).
 */
gboolean gs_modulix_store1_reboot_required(void);

G_END_DECLS
