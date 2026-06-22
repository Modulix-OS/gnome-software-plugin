/*
 * gs-modulix-icon-cache.c — App-id → icon URL cache
 *
 * GRWLock allows concurrent readers without blocking each other; writers
 * (gs_modulix_icon_cache_put) are rare — only on the first encounter of
 * each app-id.  The first icon URL wins (insert-only).
 */

#include "gs-modulix-icon-cache.h"

static GHashTable *icon_cache;
static GRWLock icon_cache_lock;

void gs_modulix_icon_cache_put(const gchar *app_id, const gchar *url) {
  g_rw_lock_writer_lock(&icon_cache_lock);
  if (icon_cache == NULL)
    icon_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  if (!g_hash_table_contains(icon_cache, app_id))
    g_hash_table_insert(icon_cache, g_strdup(app_id), g_strdup(url));
  g_rw_lock_writer_unlock(&icon_cache_lock);
}

/* Returns a newly-allocated copy (caller frees) or NULL. */
gchar *gs_modulix_icon_cache_get(const gchar *app_id) {
  gchar *url = NULL;
  g_rw_lock_reader_lock(&icon_cache_lock);
  if (icon_cache != NULL)
    url = g_strdup(g_hash_table_lookup(icon_cache, app_id));
  g_rw_lock_reader_unlock(&icon_cache_lock);
  return url;
}

void gs_modulix_icon_cache_clear(void) {
  g_rw_lock_writer_lock(&icon_cache_lock);
  g_clear_pointer(&icon_cache, g_hash_table_unref);
  g_rw_lock_writer_unlock(&icon_cache_lock);
  g_rw_lock_clear(&icon_cache_lock);
}
