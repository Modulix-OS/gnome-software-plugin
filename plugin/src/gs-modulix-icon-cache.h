#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * @file gs-modulix-icon-cache.h
 * @brief Process-wide, in-memory app-id -> icon URL cache.
 *
 * Pure in-memory store (a GHashTable behind a GRWLock): nothing here ever
 * touches disk or network. The key is the AppStream app-id; the value is the
 * icon URL last recorded for it. Insert-only semantics — the first URL seen
 * for a given app-id wins and later puts for the same app-id are no-ops — so
 * the cache never shrinks or evicts individual entries; it only grows, one
 * entry per distinct app-id encountered, for as long as the plugin process
 * runs. The only way entries leave is a full gs_modulix_icon_cache_clear(),
 * called from gs_plugin_modulix_finalize() (gs-plugin-modulix.c) when the
 * plugin instance is torn down (GNOME Software can unload/reload the plugin
 * within the same process). Concurrent readers never block each other;
 * writers (rare — only on the first encounter of each app-id) are mutually
 * exclusive with both readers and other writers.
 */

/**
 * @brief Record the icon URL discovered for an app-id, if not already cached.
 *
 * Inserts (@p app_id, @p url) into the process-wide in-memory cache the
 * first time @p app_id is seen; a later call for the same @p app_id is a
 * silent no-op (insert-only, first URL wins). Nothing is written to disk.
 * Safe to call from any thread — the call takes the cache's GRWLock as a
 * writer for its duration.
 *
 * @param app_id AppStream app-id used as the cache key. Private copies
 *   (g_strdup()) of both this and @p url are made and owned by the cache, so
 *   the caller retains ownership of the arguments. Must not be NULL.
 * @param url Icon URL to associate with @p app_id. Must not be NULL.
 *
 * @pre None; the underlying hash table is created lazily on first use.
 * @post @p app_id maps to @p url in the cache if it was not already present;
 *   otherwise the cache is unchanged.
 * @return None (void).
 */
void gs_modulix_icon_cache_put(const gchar *app_id, const gchar *url);

/**
 * @brief Look up the icon URL cached for an app-id.
 *
 * Reads the process-wide in-memory cache only — no disk or network I/O.
 * Safe to call from any thread; the call takes the cache's GRWLock as a
 * reader for its duration, so concurrent lookups do not block each other.
 *
 * @param app_id AppStream app-id to look up. Must not be NULL.
 *
 * @pre None. Valid to call before any matching gs_modulix_icon_cache_put()
 *   (returns NULL) or after gs_modulix_icon_cache_clear() (also NULL).
 * @post The cache is left unmodified.
 * @return A newly-allocated copy of the cached URL (transfer full — caller
 *   must free with g_free()), or NULL if @p app_id has never been cached, or
 *   was cached but the cache has since been cleared.
 */
gchar *gs_modulix_icon_cache_get(const gchar *app_id);

/**
 * @brief Drop every entry from the process-wide icon-URL cache.
 *
 * Destroys the underlying GHashTable (freeing every cached app-id/URL pair)
 * and resets the cache to its just-initialized, empty state; the next
 * gs_modulix_icon_cache_put() call lazily recreates it. This is the only way
 * entries ever leave the cache — there is no per-entry expiry or eviction,
 * so absent a call to this function the cache only grows, one entry per
 * distinct app-id listed. Called from gs_plugin_modulix_finalize()
 * (gs-plugin-modulix.c) when the plugin instance is torn down.
 *
 * @pre None.
 * @post The cache holds no entries. The static GRWLock guarding it is left
 *   intact — never g_rw_lock_clear()'d — so a later put/get after the
 *   plugin is unloaded and reloaded within the same process does not lock a
 *   destroyed lock.
 * @return None (void).
 */
void gs_modulix_icon_cache_clear(void);

G_END_DECLS
