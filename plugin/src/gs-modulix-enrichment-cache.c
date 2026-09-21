/**
 * @file gs-modulix-enrichment-cache.c
 * @brief Per-app-id enrichment JSON cache.
 *
 * mx_store_get_app_enrichment() is an FFI call that can block; multiple
 * GsApp instances sharing the same app_id (nix variants of the same app)
 * must not each trigger a redundant fetch.
 *
 * Locking strategy:
 *   reader lock  → lookup (fast path)
 *   writer lock  → populate on first encounter (double-check inside)
 *
 * See gs-modulix-enrichment-cache.h for the full cache contract (key/value
 * shape, negative caching, absence of any TTL/eviction, thread-safety).
 */

#include "gs-modulix-enrichment-cache.h"
#include "modulix-store-client.h"
#include <json-glib/json-glib.h>

static GHashTable *enrichment_cache;
static GRWLock enrichment_lock;

/**
 * enrichment_value_free:
 * @v: (transfer full) (nullable): a cache value, i.e. either a
 *   `gchar *` previously stored by gs_modulix_enrichment_cache_get_or_fetch()
 *   or gs_modulix_enrichment_cache_prefetch_many(), or
 *   #GS_MODULIX_ENRICHMENT_EMPTY.
 *
 * `GDestroyNotify` used as the value-destructor of #enrichment_cache. Frees
 * @v with g_free() unless it is the #GS_MODULIX_ENRICHMENT_EMPTY sentinel,
 * which is not a heap pointer and must never be passed to g_free().
 *
 * Cache values are always GLib-allocated (g_strdup'd out of whatever the
 * daemon returned), never the daemon's own CString pointer — this lets
 * every insertion path (single-fetch and batched prefetch) share this one
 * destructor, instead of having to track which allocator produced which
 * value.
 *
 * @pre @v is either NULL, #GS_MODULIX_ENRICHMENT_EMPTY, or a pointer
 *   previously returned by g_strdup()/g_malloc()-family allocation.
 * @post @v is freed (unless it was the sentinel); it must not be used
 *   afterwards.
 *
 * Returns: nothing.
 */
static void enrichment_value_free(gpointer v) {
  if (v != GS_MODULIX_ENRICHMENT_EMPTY)
    g_free(v);
}

/**
 * gs_modulix_enrichment_cache_get_or_fetch:
 * @app_id: the AppStream component id to look up; must be a valid
 *   NUL-terminated string, not NULL.
 *
 * See the full contract in gs-modulix-enrichment-cache.h. Implementation
 * notes: the reader-lock fast path returns immediately on a hit (including
 * a cached #GS_MODULIX_ENRICHMENT_EMPTY, returned as NULL). On a miss, the
 * blocking `mx_store_get_app_enrichment()` FFI call and its `g_strdup()`
 * copy both happen with no lock held, so concurrent lookups of other ids
 * are never blocked behind this fetch; the writer lock is then taken only
 * to insert, with a double-check (`g_hash_table_contains`) in case another
 * thread populated the same @app_id first — the loser discards its own
 * fetch and returns a fresh copy of the winner's value instead.
 *
 * @pre None; callable from any thread, before or after the cache's first
 *   use.
 * @post @app_id has a cache entry after this call (real JSON or
 *   #GS_MODULIX_ENRICHMENT_EMPTY).
 *
 * Returns: (transfer full) (nullable): a newly heap-allocated JSON string
 *   the caller must g_free(), or NULL if @app_id has no enrichment (daemon
 *   returned nothing, or the fetch failed) — see header for details.
 */
gchar *gs_modulix_enrichment_cache_get_or_fetch(const gchar *app_id) {
  g_rw_lock_reader_lock(&enrichment_lock);
  gboolean in_cache = (enrichment_cache != NULL &&
                       g_hash_table_contains(enrichment_cache, app_id));
  gchar *cached = NULL;
  if (in_cache) {
    gchar *v = g_hash_table_lookup(enrichment_cache, app_id);
    cached = (v && v != GS_MODULIX_ENRICHMENT_EMPTY) ? g_strdup(v) : NULL;
  }
  g_rw_lock_reader_unlock(&enrichment_lock);

  if (in_cache)
    return cached;

  gchar *fetched = mx_store_get_app_enrichment(app_id);
  gchar *owned = fetched ? g_strdup(fetched) : NULL;
  if (fetched)
    mx_store_free_string(fetched);

  g_rw_lock_writer_lock(&enrichment_lock);
  if (enrichment_cache == NULL)
    enrichment_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             enrichment_value_free);

  if (!g_hash_table_contains(enrichment_cache, app_id)) {
    g_hash_table_insert(enrichment_cache, g_strdup(app_id),
                        owned ? owned : GS_MODULIX_ENRICHMENT_EMPTY);
    cached = owned ? g_strdup(owned) : NULL;
  } else {
    g_free(owned);
    gchar *winner = g_hash_table_lookup(enrichment_cache, app_id);
    cached = (winner && winner != GS_MODULIX_ENRICHMENT_EMPTY)
                 ? g_strdup(winner)
                 : NULL;
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
 * `mx_store_get_app_enrichment_many()` FFI call and its JSON parse happen
 * between them with no lock held. Each still-missing id is re-checked under
 * the writer lock (another thread may have populated it via this function
 * or gs_modulix_enrichment_cache_get_or_fetch() in the meantime) and left
 * untouched if so. An id present in the parsed response object is
 * re-serialized (via `JsonGenerator`) and cached as its own JSON string; an
 * id the response object has no member for — including every id when the
 * FFI call itself returned NULL or produced unparsable JSON — is cached as
 * #GS_MODULIX_ENRICHMENT_EMPTY.
 *
 * @pre None; callable from any thread. No-op when @app_ids is NULL, @n is
 *   0, or every id in @app_ids is already cached.
 * @post Every id in @app_ids not already cached now has an entry (real
 *   JSON or #GS_MODULIX_ENRICHMENT_EMPTY).
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

  gchar *json = mx_store_get_app_enrichment_many(
      (const gchar *const *)missing->pdata, missing->len);

  g_autoptr(JsonParser) parser = json_parser_new();
  JsonObject *root = NULL;
  if (json != NULL) {
    g_autoptr(GError) err = NULL;
    if (json_parser_load_from_data(parser, json, -1, &err)) {
      JsonNode *node = json_parser_get_root(parser);
      if (node != NULL && JSON_NODE_HOLDS_OBJECT(node))
        root = json_node_get_object(node);
    } else {
      g_warning("[modulix] parse enrichment-many JSON: %s", err->message);
    }
  }

  g_rw_lock_writer_lock(&enrichment_lock);
  if (enrichment_cache == NULL)
    enrichment_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             enrichment_value_free);

  for (guint i = 0; i < missing->len; i++) {
    const gchar *id = g_ptr_array_index(missing, i);
    if (g_hash_table_contains(enrichment_cache, id))
      continue;

    gchar *value = GS_MODULIX_ENRICHMENT_EMPTY;
    if (root != NULL && json_object_has_member(root, id)) {
      JsonNode *entry_node = json_object_get_member(root, id);
      g_autoptr(JsonGenerator) gen = json_generator_new();
      json_generator_set_root(gen, entry_node);
      value = json_generator_to_data(gen, NULL);
    }
    g_hash_table_insert(enrichment_cache, g_strdup(id), value);
  }
  g_rw_lock_writer_unlock(&enrichment_lock);

  if (json)
    mx_store_free_string(json);
}

/**
 * gs_modulix_enrichment_cache_clear:
 *
 * See the full contract in gs-modulix-enrichment-cache.h. Takes the writer
 * lock to drop and unref #enrichment_cache (freeing every key and value via
 * `g_free`/enrichment_value_free()), then destroys #enrichment_lock itself
 * with g_rw_lock_clear().
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
