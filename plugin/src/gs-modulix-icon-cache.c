/**
 * @file gs-modulix-icon-cache.c
 * @brief App-id -> icon URL cache; see gs-modulix-icon-cache.h for the
 *   public contract.
 *
 * Storage is a plain GHashTable in static (per-process) storage — purely
 * in-memory, nothing here ever touches disk or network. A GRWLock allows
 * concurrent readers without blocking each other; writers
 * (gs_modulix_icon_cache_put()) are rare — only on the first encounter of
 * each app-id. The first icon URL wins (insert-only): entries are never
 * overwritten or individually evicted, so the table only grows until
 * gs_modulix_icon_cache_clear() drops it wholesale.
 */

#include "gs-modulix-icon-cache.h"

static GHashTable *icon_cache;
static GRWLock icon_cache_lock;

/**
 * @brief Insert-only write of the icon URL for an app-id.
 *
 * Takes @p icon_cache_lock as a writer, lazily creates the hash table on
 * first use, and inserts (@p app_id, @p url) only if @p app_id is not
 * already present. No disk I/O.
 *
 * @param app_id Cache key (AppStream app-id). Copied via g_strdup(); caller
 *   keeps ownership of the argument. Must not be NULL.
 * @param url Icon URL to store. Copied via g_strdup(); caller keeps
 *   ownership of the argument. Must not be NULL.
 *
 * @pre None.
 * @post @p app_id maps to @p url if it was not already present in the
 *   cache; otherwise no change.
 * @return None (void).
 */
void gs_modulix_icon_cache_put(const gchar *app_id, const gchar *url) {
  g_rw_lock_writer_lock(&icon_cache_lock);
  if (icon_cache == NULL)
    icon_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  if (!g_hash_table_contains(icon_cache, app_id))
    g_hash_table_insert(icon_cache, g_strdup(app_id), g_strdup(url));
  g_rw_lock_writer_unlock(&icon_cache_lock);
}

/**
 * @brief Read-only lookup of the icon URL cached for an app-id.
 *
 * Takes @p icon_cache_lock as a reader for the duration of the lookup. No
 * disk I/O. Returns a newly-allocated copy (caller frees) or NULL.
 *
 * @param app_id Cache key to look up. Must not be NULL.
 *
 * @pre None; safe to call whether or not the table has been created yet
 *   (an uncreated table behaves like an empty one).
 * @post Cache is unmodified.
 * @return A newly-allocated copy of the cached URL (transfer full — caller
 *   must g_free() it), or NULL if @p app_id is not cached.
 */
gchar *gs_modulix_icon_cache_get(const gchar *app_id) {
  gchar *url = NULL;
  g_rw_lock_reader_lock(&icon_cache_lock);
  if (icon_cache != NULL)
    url = g_strdup(g_hash_table_lookup(icon_cache, app_id));
  g_rw_lock_reader_unlock(&icon_cache_lock);
  return url;
}

/**
 * @brief Drop every entry from the cache, freeing the underlying table.
 *
 * The only way entries ever leave the cache (no per-entry eviction or
 * expiry exists). A subsequent gs_modulix_icon_cache_put() lazily recreates
 * the table.
 *
 * Unlike other Modulix caches, this function never calls g_rw_lock_clear()
 * on @p icon_cache_lock: GNOME Software can unload and reload this plugin
 * within the same process, and a later gs_modulix_icon_cache_put()/
 * gs_modulix_icon_cache_get() would otherwise lock an already-destroyed
 * GRWLock.
 *
 * @pre None.
 * @post The cache holds no entries.
 * @return None (void).
 */
void gs_modulix_icon_cache_clear(void) {
  g_rw_lock_writer_lock(&icon_cache_lock);
  g_clear_pointer(&icon_cache, g_hash_table_unref);
  g_rw_lock_writer_unlock(&icon_cache_lock);
}
