/*
 * gs-modulix-plugins-cache.c — Per-module plugins-list JSON cache
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

static GHashTable *plugins_cache; /* module_name (owned) → json (owned) | EMPTY */
static GRWLock plugins_lock;

static void plugins_value_free(gpointer v) {
  if (v != GS_MODULIX_PLUGINS_CACHE_EMPTY)
    g_free(v);
}

gchar *gs_modulix_plugins_cache_get_or_fetch(const gchar *module_name) {
  /* ── fast path ── */
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
    return cached; /* may be NULL if EMPTY */

  /* ── slow path: fetch outside the lock ── */
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
    /* Lost the race — discard our fetch, borrow the winner's value. */
    g_free(owned);
    gchar *winner = g_hash_table_lookup(plugins_cache, module_name);
    cached = (winner && winner != GS_MODULIX_PLUGINS_CACHE_EMPTY)
                 ? g_strdup(winner)
                 : NULL;
  }
  g_rw_lock_writer_unlock(&plugins_lock);

  return cached;
}

void gs_modulix_plugins_cache_clear(void) {
  g_rw_lock_writer_lock(&plugins_lock);
  g_clear_pointer(&plugins_cache, g_hash_table_unref);
  g_rw_lock_writer_unlock(&plugins_lock);
  g_rw_lock_clear(&plugins_lock);
}
