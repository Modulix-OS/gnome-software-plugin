/*
 * gs-modulix-enrichment-cache.c — Per-app-id enrichment JSON cache
 *
 * backend_get_app_enrichment() is an FFI call that can block; multiple
 * GsApp instances sharing the same app_id (nix variants of the same app)
 * must not each trigger a redundant fetch.
 *
 * Locking strategy:
 *   reader lock  → lookup (fast path)
 *   writer lock  → populate on first encounter (double-check inside)
 */

#include "gs-modulix-enrichment-cache.h"
#include "backend.h"

static GHashTable *enrichment_cache; /* app_id (owned) → json (owned) | EMPTY */
static GRWLock enrichment_lock;

static void enrichment_value_free(gpointer v) {
  if (v != GS_MODULIX_ENRICHMENT_EMPTY)
    backend_free_string((gchar *)v);
}

gchar *gs_modulix_enrichment_cache_get_or_fetch(const gchar *app_id) {
  /* ── fast path ── */
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
    return cached; /* may be NULL if EMPTY */

  /* ── slow path: fetch outside the lock ── */
  gchar *fetched = backend_get_app_enrichment(app_id);

  g_rw_lock_writer_lock(&enrichment_lock);
  if (enrichment_cache == NULL)
    enrichment_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             enrichment_value_free);

  if (!g_hash_table_contains(enrichment_cache, app_id)) {
    g_hash_table_insert(enrichment_cache, g_strdup(app_id),
                        fetched ? fetched : GS_MODULIX_ENRICHMENT_EMPTY);
    cached = fetched ? g_strdup(fetched) : NULL;
  } else {
    /* Lost the race — discard our fetch, borrow the winner's value. */
    if (fetched)
      backend_free_string(fetched);
    gchar *winner = g_hash_table_lookup(enrichment_cache, app_id);
    cached = (winner && winner != GS_MODULIX_ENRICHMENT_EMPTY)
                 ? g_strdup(winner)
                 : NULL;
  }
  g_rw_lock_writer_unlock(&enrichment_lock);

  return cached;
}

void gs_modulix_enrichment_cache_clear(void) {
  g_rw_lock_writer_lock(&enrichment_lock);
  g_clear_pointer(&enrichment_cache, g_hash_table_unref);
  g_rw_lock_writer_unlock(&enrichment_lock);
  g_rw_lock_clear(&enrichment_lock);
}
