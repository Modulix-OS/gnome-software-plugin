/*
 * gs-modulix-enrichment-cache.c — Per-app-id enrichment JSON cache
 *
 * mx_store_get_app_enrichment() is an FFI call that can block; multiple
 * GsApp instances sharing the same app_id (nix variants of the same app)
 * must not each trigger a redundant fetch.
 *
 * Locking strategy:
 *   reader lock  → lookup (fast path)
 *   writer lock  → populate on first encounter (double-check inside)
 */

#include "gs-modulix-enrichment-cache.h"
#include "modulix-store-client.h"
#include <json-glib/json-glib.h>

/* Cache values are always GLib-allocated (g_strdup'd out of whatever the
 * daemon returned), never the daemon's own CString pointer — this lets
 * every insertion path (single-fetch and batched prefetch) share one
 * allocator for enrichment_value_free, instead of having to track which
 * allocator produced which value. */
static GHashTable *enrichment_cache; /* app_id (owned) → json (owned) | EMPTY */
static GRWLock enrichment_lock;

static void enrichment_value_free(gpointer v) {
  if (v != GS_MODULIX_ENRICHMENT_EMPTY)
    g_free(v);
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
    /* Lost the race — discard our fetch, borrow the winner's value. */
    g_free(owned);
    gchar *winner = g_hash_table_lookup(enrichment_cache, app_id);
    cached = (winner && winner != GS_MODULIX_ENRICHMENT_EMPTY)
                 ? g_strdup(winner)
                 : NULL;
  }
  g_rw_lock_writer_unlock(&enrichment_lock);

  return cached;
}

void gs_modulix_enrichment_cache_prefetch_many(const gchar *const *app_ids,
                                               guint n) {
  if (app_ids == NULL || n == 0)
    return;

  /* ── find what's missing (fast path) ── */
  g_autoptr(GPtrArray) missing = g_ptr_array_new(); /* borrowed const gchar* */
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

  /* ── slow path: one batched fetch outside the lock ── */
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
      continue; /* another thread won the race for this id */

    gchar *value = GS_MODULIX_ENRICHMENT_EMPTY;
    if (root != NULL && json_object_has_member(root, id)) {
      JsonNode *entry_node = json_object_get_member(root, id);
      g_autoptr(JsonGenerator) gen = json_generator_new();
      json_generator_set_root(gen, entry_node);
      value = json_generator_to_data(gen, NULL); /* g_malloc'd */
    }
    g_hash_table_insert(enrichment_cache, g_strdup(id), value);
  }
  g_rw_lock_writer_unlock(&enrichment_lock);

  if (json)
    mx_store_free_string(json);
}

void gs_modulix_enrichment_cache_clear(void) {
  g_rw_lock_writer_lock(&enrichment_lock);
  g_clear_pointer(&enrichment_cache, g_hash_table_unref);
  g_rw_lock_writer_unlock(&enrichment_lock);
  g_rw_lock_clear(&enrichment_lock);
}
