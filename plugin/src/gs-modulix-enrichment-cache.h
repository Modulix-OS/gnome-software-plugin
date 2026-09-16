#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Sentinel: app_id is known to have no enrichment data. */
#define GS_MODULIX_ENRICHMENT_EMPTY ((gchar *)1)

/**
 * gs_modulix_enrichment_cache_get_or_fetch:
 * @app_id: the AppStream component id
 *
 * Returns the cached enrichment JSON for @app_id, fetching it from the
 * daemon (via mx_store_get_app_enrichment) on first call.
 *
 * Returns GS_MODULIX_ENRICHMENT_EMPTY when the daemon returned nothing.
 * Returns a heap-allocated JSON string otherwise — caller must g_free() it.
 */
gchar *gs_modulix_enrichment_cache_get_or_fetch(const gchar *app_id);

/**
 * gs_modulix_enrichment_cache_prefetch_many:
 * @app_ids: (array length=n): app-ids to warm the cache for
 * @n: number of entries in @app_ids
 *
 * Fetches enrichment for every id in @app_ids not already cached, in a
 * single batched daemon call, and populates the cache with the results (or
 * with #GS_MODULIX_ENRICHMENT_EMPTY for ids the daemon found nothing for).
 * Subsequent gs_modulix_enrichment_cache_get_or_fetch() calls for these ids
 * become cache hits.
 */
void gs_modulix_enrichment_cache_prefetch_many(const gchar *const *app_ids,
                                               guint n);

void gs_modulix_enrichment_cache_clear(void);

G_END_DECLS
