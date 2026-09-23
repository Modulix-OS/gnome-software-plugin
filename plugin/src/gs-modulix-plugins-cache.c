/**
 * @file gs-modulix-plugins-cache.c
 * @brief Per-module plugins-list `GVariant` cache; see the header for the
 *        public contract.
 *
 * `ListModulePlugins` forces `meta.description` across a whole nixpkgs
 * namespace (a full-namespace `nix eval`); the details page re-requests the
 * same module's plugins on every ADDONS refine. Mirrors
 * gs-modulix-enrichment-cache.c's locking strategy:
 *   reader lock  → lookup (fast path)
 *   writer lock  → populate on first encounter (double-check inside)
 */

#include "gs-modulix-plugins-cache.h"
#include "dbus/gs-modulix-store1.h"

static GHashTable *plugins_cache;
static GRWLock plugins_lock;

/**
 * @brief The shared "no plugins" marker: an empty `aa{sv}` singleton,
 *   refcounted like any other cached value.
 *
 * Compared by pointer identity to tell "we explicitly cached nothing" apart
 * from a table miss; unlike the old `(gchar *)1` sentinel, this is a real
 * `GVariant` and needs no special-casing in the value destructor — every
 * insertion takes its own ref, and `g_variant_unref()` on removal is always
 * correct.
 *
 * @return The shared singleton (transfer none: do not unref the return
 *   value itself, only refs taken from it).
 */
static GVariant *plugins_cache_empty(void) {
  static GVariant *empty;
  if (empty == NULL)
    empty = g_variant_ref_sink(g_variant_new("aa{sv}", NULL));
  return empty;
}

GVariant *gs_modulix_plugins_cache_get_or_fetch(const gchar *module_name) {
  g_rw_lock_reader_lock(&plugins_lock);
  gboolean in_cache = (plugins_cache != NULL &&
                       g_hash_table_contains(plugins_cache, module_name));
  GVariant *cached = NULL;
  if (in_cache) {
    GVariant *v = g_hash_table_lookup(plugins_cache, module_name);
    cached = (v != plugins_cache_empty()) ? g_variant_ref(v) : NULL;
  }
  g_rw_lock_reader_unlock(&plugins_lock);

  if (in_cache)
    return cached;

  g_autoptr(GVariant) fetched =
      gs_modulix_store1_list_module_plugins(module_name);

  g_rw_lock_writer_lock(&plugins_lock);
  if (plugins_cache == NULL)
    plugins_cache = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);

  if (!g_hash_table_contains(plugins_cache, module_name)) {
    g_hash_table_insert(plugins_cache, g_strdup(module_name),
                        fetched != NULL ? g_variant_ref(fetched)
                                        : g_variant_ref(plugins_cache_empty()));
    cached = fetched != NULL ? g_variant_ref(fetched) : NULL;
  } else {
    GVariant *winner = g_hash_table_lookup(plugins_cache, module_name);
    cached = (winner != plugins_cache_empty()) ? g_variant_ref(winner) : NULL;
  }
  g_rw_lock_writer_unlock(&plugins_lock);

  return cached;
}

void gs_modulix_plugins_cache_invalidate(const gchar *module_name) {
  g_rw_lock_writer_lock(&plugins_lock);
  if (plugins_cache != NULL)
    g_hash_table_remove(plugins_cache, module_name);
  g_rw_lock_writer_unlock(&plugins_lock);
}

void gs_modulix_plugins_cache_clear(void) {
  g_rw_lock_writer_lock(&plugins_lock);
  g_clear_pointer(&plugins_cache, g_hash_table_unref);
  g_rw_lock_writer_unlock(&plugins_lock);
  g_rw_lock_clear(&plugins_lock);
}
