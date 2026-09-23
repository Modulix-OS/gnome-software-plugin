/**
 * @file gs-modulix-enrichment-cache.c
 * @brief Per-app-id enrichment `GVariant` cache.
 *
 * `GetAppEnrichment` is a D-Bus round-trip that can block; multiple GsApp
 * instances sharing the same app_id (nix variants of the same app) must not
 * each trigger a redundant fetch.
 *
 * Locking strategy:
 *   reader lock  → lookup (fast path)
 *   writer lock  → populate on first encounter (double-check inside)
 *
 * See gs-modulix-enrichment-cache.h for the full cache contract (key/value
 * shape, negative caching, absence of any TTL/eviction, thread-safety).
 */

#include "gs-modulix-enrichment-cache.h"
#include "dbus/gs-modulix-store1.h"

static GHashTable *enrichment_cache;
static GRWLock enrichment_lock;

/**
 * @brief The shared "no enrichment" marker: an empty `a{sv}` singleton,
 *   refcounted like any other cached value. See
 *   `plugins_cache_empty()` (gs-modulix-plugins-cache.c) for the same
 *   pattern and rationale.
 *
 * @return The shared singleton (transfer none).
 */
static GVariant *enrichment_cache_empty(void) {
  static GVariant *empty;
  if (empty == NULL)
    empty = g_variant_ref_sink(g_variant_new("a{sv}", NULL));
  return empty;
}

/**
 * gs_modulix_enrichment_cache_get_or_fetch:
 * @app_id: the AppStream component id to look up; must be a valid
 *   NUL-terminated string, not NULL.
 *
 * See the full contract in gs-modulix-enrichment-cache.h. Implementation
 * notes: the reader-lock fast path returns immediately on a hit (including
 * a cached empty singleton, returned as NULL). On a miss, the blocking
 * `GetAppEnrichment` D-Bus call happens with no lock held, so concurrent
 * lookups of other ids are never blocked behind this fetch; the writer lock
 * is then taken only to insert, with a double-check
 * (`g_hash_table_contains`) in case another thread populated the same
 * @app_id first — the loser discards its own fetch and returns a fresh ref
 * to the winner's value instead.
 *
 * @pre None; callable from any thread, before or after the cache's first
 *   use.
 * @post @app_id has a cache entry after this call (real enrichment or the
 *   empty singleton).
 *
 * Returns: (transfer full) (nullable): a new reference to the cached
 *   `a{sv}` GVariant the caller must g_variant_unref(), or NULL if @app_id
 *   has no enrichment (daemon returned nothing, or the fetch failed) — see
 *   header for details.
 */
GVariant *gs_modulix_enrichment_cache_get_or_fetch(const gchar *app_id) {
  g_rw_lock_reader_lock(&enrichment_lock);
  gboolean in_cache = (enrichment_cache != NULL &&
                       g_hash_table_contains(enrichment_cache, app_id));
  GVariant *cached = NULL;
  if (in_cache) {
    GVariant *v = g_hash_table_lookup(enrichment_cache, app_id);
    cached = (v != enrichment_cache_empty()) ? g_variant_ref(v) : NULL;
  }
  g_rw_lock_reader_unlock(&enrichment_lock);

  if (in_cache)
    return cached;

  const gchar *ids[1] = {app_id};
  g_autoptr(GVariant) many = gs_modulix_store1_get_app_enrichment(ids, 1);
  g_autoptr(GVariant) fetched = NULL;
  if (many != NULL)
    g_variant_lookup(many, app_id, "@a{sv}", &fetched);

  g_rw_lock_writer_lock(&enrichment_lock);
  if (enrichment_cache == NULL)
    enrichment_cache = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);

  if (!g_hash_table_contains(enrichment_cache, app_id)) {
    g_hash_table_insert(
        enrichment_cache, g_strdup(app_id),
        fetched != NULL ? g_variant_ref(fetched)
                        : g_variant_ref(enrichment_cache_empty()));
    cached = fetched != NULL ? g_variant_ref(fetched) : NULL;
  } else {
    GVariant *winner = g_hash_table_lookup(enrichment_cache, app_id);
    cached = (winner != enrichment_cache_empty()) ? g_variant_ref(winner) : NULL;
  }
  g_rw_lock_writer_unlock(&enrichment_lock);

  return cached;
}

/**
 * gs_modulix_enrichment_cache_prefetch_many:
 * @app_ids: (array length=n): app-ids to warm the cache for. NULL or empty
 *   entries are skipped. May be NULL only if @n is 0.
 * @n: number of entries in @app_ids.
 *
 * See the full contract in gs-modulix-enrichment-cache.h. Implementation
 * notes: the "which ids are missing" scan (reader lock) and the population
 * of results (writer lock) are two separate critical sections; the batched
 * `GetAppEnrichment` D-Bus call happens between them with no lock held.
 * Each still-missing id is re-checked under the writer lock (another thread
 * may have populated it via this function or
 * gs_modulix_enrichment_cache_get_or_fetch() in the meantime) and left
 * untouched if so. An id present in the reply dict is re-referenced and
 * cached as its own `a{sv}`; an id the reply has no member for — including
 * every id when the D-Bus call itself failed — is cached as the empty
 * singleton.
 *
 * @pre None; callable from any thread. No-op when @app_ids is NULL, @n is
 *   0, or every id in @app_ids is already cached.
 * @post Every id in @app_ids not already cached now has an entry (real
 *   enrichment or the empty singleton).
 *
 * Returns: nothing.
 */
void gs_modulix_enrichment_cache_prefetch_many(const gchar *const *app_ids,
                                               guint n) {
  if (app_ids == NULL || n == 0)
    return;

  g_autoptr(GPtrArray) missing = g_ptr_array_new();
  g_rw_lock_reader_lock(&enrichment_lock);
  for (guint i = 0; i < n; i++) {
    const gchar *id = app_ids[i];
    if (id == NULL || *id == '\0')
      continue;
    if (enrichment_cache == NULL || !g_hash_table_contains(enrichment_cache, id))
      g_ptr_array_add(missing, (gpointer)id);
  }
  g_rw_lock_reader_unlock(&enrichment_lock);

  if (missing->len == 0)
    return;

  g_autoptr(GVariant) many = gs_modulix_store1_get_app_enrichment(
      (const gchar *const *)missing->pdata, missing->len);

  g_rw_lock_writer_lock(&enrichment_lock);
  if (enrichment_cache == NULL)
    enrichment_cache = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);

  for (guint i = 0; i < missing->len; i++) {
    const gchar *id = g_ptr_array_index(missing, i);
    if (g_hash_table_contains(enrichment_cache, id))
      continue;

    GVariant *entry = NULL;
    if (many != NULL)
      g_variant_lookup(many, id, "@a{sv}", &entry);
    g_hash_table_insert(enrichment_cache, g_strdup(id),
                        entry != NULL ? entry
                                      : g_variant_ref(enrichment_cache_empty()));
  }
  g_rw_lock_writer_unlock(&enrichment_lock);
}

/**
 * gs_modulix_enrichment_cache_clear:
 *
 * See the full contract in gs-modulix-enrichment-cache.h. Takes the writer
 * lock to drop and unref #enrichment_cache (freeing every key and value via
 * `g_free`/`g_variant_unref`), then destroys #enrichment_lock itself with
 * g_rw_lock_clear().
 *
 * @pre Intended to be called at most once, at plugin shutdown (see
 *   `gs_plugin_modulix_finalize()` in gs-plugin-modulix.c). Because it
 *   destroys #enrichment_lock, no other enrichment-cache call may run
 *   concurrently with or after this one.
 * @post #enrichment_cache is NULL and #enrichment_lock is invalid.
 *
 * Returns: nothing.
 */
void gs_modulix_enrichment_cache_clear(void) {
  g_rw_lock_writer_lock(&enrichment_lock);
  g_clear_pointer(&enrichment_cache, g_hash_table_unref);
  g_rw_lock_writer_unlock(&enrichment_lock);
  g_rw_lock_clear(&enrichment_lock);
}
