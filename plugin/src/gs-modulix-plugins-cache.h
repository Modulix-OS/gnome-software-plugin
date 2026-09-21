/**
 * @file gs-modulix-plugins-cache.h
 * @brief Public interface of the per-module plugins-list JSON cache.
 *
 * Cache contract:
 *  - Key: Modulix module name (`module_name`), as a `gchar *` string.
 *  - Value: either a heap-allocated JSON string (the plugins-array payload
 *    `mx_store_list_module_plugins()` returned for that module, re-strdup'd
 *    out of the shim's own buffer), or the #GS_MODULIX_PLUGINS_CACHE_EMPTY
 *    sentinel recording that the daemon has nothing for that module (no
 *    plugins namespace, or the daemon call itself failed — the two are not
 *    distinguished).
 *  - Negative results ARE cached: a module the daemon returned nothing for
 *    is stored as #GS_MODULIX_PLUGINS_CACHE_EMPTY, so a repeated lookup for
 *    that module is a cache hit (fast path, no re-fetch), not a re-fetch.
 *  - TTL / eviction: none. There is no expiry and no size bound — every
 *    distinct module name ever looked up stays cached for the lifetime of
 *    the process. Entries are removed only by:
 *      (1) gs_modulix_plugins_cache_invalidate(), called from
 *          `gs-modulix-lifecycle.c`'s `call_for_plugins()` after a *single
 *          plugin's* install/uninstall daemon call reports success, for
 *          that plugin's parent module only — a module-level or
 *          package-level install/uninstall does NOT invalidate this cache;
 *      (2) gs_modulix_plugins_cache_clear(), a full-table wipe called once,
 *          at `GsPluginModulix` finalize (`gs-plugin-modulix.c`) — not
 *          periodically and not per-entry.
 *    Because of (1), a details page reopened for a module while its plugin
 *    install/uninstall daemon call is still in flight observes the
 *    *pre-change* JSON (and therefore a stale per-plugin `installed` bit)
 *    until that call returns successfully and invalidation runs.
 *  - Thread-safety: a single process-wide #GRWLock guards the hash table.
 *    The lookup fast path of gs_modulix_plugins_cache_get_or_fetch() takes
 *    the reader lock only long enough to read/copy a value out of the
 *    table. Population (first encounter of a module) takes the writer lock
 *    only long enough to insert; the blocking daemon call itself always
 *    happens outside both locks, so concurrent lookups for other modules
 *    are never blocked behind a fetch in flight.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * @def GS_MODULIX_PLUGINS_CACHE_EMPTY
 * @brief Sentinel cache value recording that a module is known to have no
 * plugins.
 *
 * Stored as the value for a `module_name` key whose fetch produced nothing
 * (no plugins namespace for that module, or the daemon call failed). A
 * `gchar *` equal to `(gchar *) 1` — never dereferenced, only ever compared
 * by pointer identity — so it round-trips through the cache's `GHashTable`
 * (whose value type is `gchar *`) without needing a separate "found but
 * empty" flag alongside each entry. Must never be passed to g_free(); see
 * `plugins_value_free()` in gs-modulix-plugins-cache.c, which special-cases
 * it.
 */
#define GS_MODULIX_PLUGINS_CACHE_EMPTY ((gchar *)1)

/**
 * @brief Returns a module's plugins-list JSON, from cache or by fetching it
 * from the daemon on first call for that module.
 *
 * The details page re-requests a module's plugins on every ADDONS refine;
 * without this cache each of those re-requests would repeat
 * `mx_store_list_module_plugins()`'s underlying full-namespace `nix eval`
 * daemon-side. A cache hit — whether pre-existing or just populated by this
 * same call — never re-fetches.
 *
 * @param module_name The Modulix module name to look up, as passed on to
 *   `mx_store_list_module_plugins()` on a miss.
 *
 * @return (transfer full) (nullable): a newly heap-allocated JSON string the
 *   caller must free with g_free() (or `g_autofree`) — every call, hit or
 *   miss, returns a fresh copy, never a pointer the cache still owns. NULL
 *   when @p module_name has no plugins — either because the daemon reported
 *   none (cached as #GS_MODULIX_PLUGINS_CACHE_EMPTY) or because the daemon
 *   call itself failed; the two cases are indistinguishable to the caller
 *   and both are cached identically (no retry on a later call for the same
 *   module).
 *
 * @pre @p module_name is non-NULL. Safe to call from any thread, before or
 *   after the cache's first use (the backing #GHashTable is created lazily
 *   on first insert).
 * @post @p module_name has a cache entry after this call (real JSON or
 *   #GS_MODULIX_PLUGINS_CACHE_EMPTY), so a subsequent call for the same
 *   module is a hit and does not re-fetch — until
 *   gs_modulix_plugins_cache_invalidate() or
 *   gs_modulix_plugins_cache_clear() removes it. Thread-safe: internally
 *   serialized by the cache's #GRWLock.
 */
gchar *gs_modulix_plugins_cache_get_or_fetch (const gchar *module_name);

/**
 * @brief Drops the cached plugins-list JSON for one module, forcing the next
 * gs_modulix_plugins_cache_get_or_fetch() call for it to re-fetch from the
 * daemon.
 *
 * This is the cache's only state-change-triggered invalidation path. It is
 * called from `gs-modulix-lifecycle.c`'s `call_for_plugins()`, and only
 * after a single plugin's install/uninstall daemon call reports success,
 * for that plugin's parent module. A module-level or package-level
 * install/uninstall does not call this function and so does not invalidate
 * this cache. Without this call, a plugin install/uninstall would leave the
 * module's cached JSON — and specifically its per-plugin `installed` bit —
 * serving stale data for the rest of the process's life, since this cache
 * has no TTL of its own.
 *
 * Observable staleness window: between the moment a plugin install/
 * uninstall is initiated and the moment its daemon call returns
 * successfully (immediately followed by this function), any details-page
 * refine for the same module still reads the pre-change cached JSON. If the
 * daemon call fails, this function is not called and the cache correctly
 * keeps serving the unchanged prior state.
 *
 * @param module_name The Modulix module name whose cache entry to drop.
 *
 * @return void.
 * @pre @p module_name is non-NULL.
 * @post The entry for @p module_name, if any, is removed; a subsequent
 *   gs_modulix_plugins_cache_get_or_fetch() for the same module re-fetches
 *   from the daemon. A no-op if the cache table has not been created yet,
 *   or holds no entry for @p module_name. Thread-safe: internally
 *   serialized by the cache's #GRWLock.
 */
void gs_modulix_plugins_cache_invalidate (const gchar *module_name);

/**
 * @brief Discards every cached entry and destroys the backing #GHashTable
 * and the cache's #GRWLock.
 *
 * Called once, by `GsPluginModulix`'s finalize (`gs-plugin-modulix.c`), as
 * the cache's only full-teardown path — there is no periodic or per-entry
 * expiry (see the cache contract at the top of this file; per-module
 * eviction is gs_modulix_plugins_cache_invalidate()).
 *
 * @return void.
 * @pre Intended to be called at most once, at plugin shutdown: it calls
 *   g_rw_lock_clear() on the cache's lock, after which further use of that
 *   lock is undefined behaviour. No call into this cache (this one
 *   included) is safe to make afterwards.
 * @post The cache table is unreffed and its pointer reset to NULL; the
 *   #GRWLock is cleared. The cache is unusable until the process
 *   re-initializes the underlying static state, which currently does not
 *   happen — this is a one-way shutdown call, not a "reset".
 */
void gs_modulix_plugins_cache_clear (void);

G_END_DECLS
