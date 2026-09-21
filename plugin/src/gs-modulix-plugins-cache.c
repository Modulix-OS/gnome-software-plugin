/**
 * @file gs-modulix-plugins-cache.c
 * @brief Per-module plugins-list JSON cache; see the header for the public
 *        contract.
 *
 * mx_store_list_module_plugins() forces `meta.description` across a whole
 * nixpkgs namespace (a full-namespace `nix eval`); the details page
 * re-requests the same module's plugins on every ADDONS refine. Mirrors
 * gs-modulix-enrichment-cache.c's locking strategy:
 *   reader lock  → lookup (fast path)
 *   writer lock  → populate on first encounter (double-check inside)
 */

#include "gs-modulix-plugins-cache.h"
#include "modulix-store-client.h"

static GHashTable *plugins_cache;
static GRWLock plugins_lock;

/**
 * @brief Frees a cache value, leaving the "no plugins" sentinel alone.
 *
 * @param v Stored value: an owned JSON string, or
 *   GS_MODULIX_PLUGINS_CACHE_EMPTY, which is a static sentinel and must not be
 *   freed.
 * @pre @p v comes from this cache.
 * @post The string is freed unless it was the sentinel.
 * @return None.
 */
static void plugins_value_free(gpointer v) {
  if (v != GS_MODULIX_PLUGINS_CACHE_EMPTY)
    g_free(v);
}

/**
 * @brief Returns a module's plugin-list JSON, fetching it from the daemon on a
 *        miss.
 *
 * @param module_name Module key, used as the cache key and passed straight to
 *   the daemon. Not NULL.
 * @pre None: the table is created on first write.
 * @post On a miss the entry is memoised for the lifetime of the process, a
 *   module without plugins included (stored as a sentinel, so the expensive
 *   full-namespace `nix eval` is not repeated for it). There is no TTL, so a
 *   plugin enabled or disabled meanwhile stays invisible until
 *   gs_modulix_plugins_cache_invalidate() is called. The daemon call runs
 *   outside the lock, so concurrent callers may fetch the same module twice;
 *   the loser discards its result and returns the winner's.
 * @return A newly allocated copy of the JSON (transfer-full: free with
 *   g_free()), or NULL when the daemon call failed or reported no plugin —
 *   these two outcomes are indistinguishable to the caller.
 */
gchar *gs_modulix_plugins_cache_get_or_fetch(const gchar *module_name) {
  g_rw_lock_reader_lock(&plugins_lock);
  gboolean in_cache = (plugins_cache != NULL &&
                       g_hash_table_contains(plugins_cache, module_name));
  gchar *cached = NULL;
  if (in_cache) {
    gchar *v = g_hash_table_lookup(plugins_cache, module_name);
    cached = (v && v != GS_MODULIX_PLUGINS_CACHE_EMPTY) ? g_strdup(v) : NULL;
  }
  g_rw_lock_reader_unlock(&plugins_lock);

  if (in_cache)
    return cached;

  gchar *fetched = mx_store_list_module_plugins(module_name);
  gchar *owned = fetched ? g_strdup(fetched) : NULL;
  if (fetched)
    mx_store_free_string(fetched);

  g_rw_lock_writer_lock(&plugins_lock);
  if (plugins_cache == NULL)
    plugins_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          plugins_value_free);

  if (!g_hash_table_contains(plugins_cache, module_name)) {
    g_hash_table_insert(plugins_cache, g_strdup(module_name),
                        owned ? owned : GS_MODULIX_PLUGINS_CACHE_EMPTY);
    cached = owned ? g_strdup(owned) : NULL;
  } else {
    g_free(owned);
    gchar *winner = g_hash_table_lookup(plugins_cache, module_name);
    cached = (winner && winner != GS_MODULIX_PLUGINS_CACHE_EMPTY)
                 ? g_strdup(winner)
                 : NULL;
  }
  g_rw_lock_writer_unlock(&plugins_lock);

  return cached;
}

/**
 * @brief Drops one module's cached plugin list, so the next lookup refetches.
 *
 * @param module_name Module key to forget. Not NULL.
 * @pre None: forgetting a module that was never cached is a no-op.
 * @post The next gs_modulix_plugins_cache_get_or_fetch() for that module hits
 *   the daemon again. Call this after enabling or disabling one of its plugins,
 *   otherwise the UI keeps showing the previous state.
 * @return None.
 */
void gs_modulix_plugins_cache_invalidate(const gchar *module_name) {
  g_rw_lock_writer_lock(&plugins_lock);
  if (plugins_cache != NULL)
    g_hash_table_remove(plugins_cache, module_name);
  g_rw_lock_writer_unlock(&plugins_lock);
}

/**
 * @brief Tears the cache down at plugin finalize.
 *
 * @pre Must be the last call made on this cache.
 * @post The table is freed *and the lock itself is destroyed*, so any later
 *   call to another function of this cache is undefined behaviour. This is a
 *   one-way teardown, not a reusable "empty the cache" — use
 *   gs_modulix_plugins_cache_invalidate() for that.
 * @return None.
 */
void gs_modulix_plugins_cache_clear(void) {
  g_rw_lock_writer_lock(&plugins_lock);
  g_clear_pointer(&plugins_cache, g_hash_table_unref);
  g_rw_lock_writer_unlock(&plugins_lock);
  g_rw_lock_clear(&plugins_lock);
}
