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
 * backend (via backend_get_app_enrichment) on first call.
 *
 * Returns GS_MODULIX_ENRICHMENT_EMPTY when the backend returned nothing.
 * Returns a heap-allocated JSON string otherwise — caller must g_free() it.
 */
gchar *gs_modulix_enrichment_cache_get_or_fetch(const gchar *app_id);
void gs_modulix_enrichment_cache_clear(void);

G_END_DECLS
