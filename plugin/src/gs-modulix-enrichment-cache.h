/**
 * @file gs-modulix-enrichment-cache.h
 * @brief Public interface of the per-app-id Flathub enrichment JSON cache.
 *
 * Cache contract:
 *  - Key: AppStream component id (`app_id`), as a `gchar *` string.
 *  - Value: either a heap-allocated JSON string (the object the daemon
 *    returned for that id, re-serialized by the daemon/shim — see
 *    `GetAppEnrichment` / `mx_store_get_app_enrichment[_many]`), or the
 *    #GS_MODULIX_ENRICHMENT_EMPTY sentinel recording that the daemon has no
 *    enrichment for that id at all.
 *  - Negative results ARE cached: an id the daemon returned nothing for is
 *    stored as #GS_MODULIX_ENRICHMENT_EMPTY so a repeated lookup for that id
 *    is a cache hit (fast path, no re-fetch), not a re-fetch.
 *  - TTL / eviction: none. There is no expiry and no size bound — every
 *    distinct `app_id` ever looked up (hit, miss, or prefetched) stays in
 *    the cache for the lifetime of the process, so memory grows
 *    monotonically with the number of distinct ids seen. The only way
 *    entries are removed is a full-cache wipe via
 *    gs_modulix_enrichment_cache_clear(), which this plugin calls once, at
 *    `GsPluginModulix` finalize (`gs-plugin-modulix.c`) — not periodically
 *    and not per-entry.
 *  - Thread-safety: a single process-wide #GRWLock guards the hash table.
 *    Lookups (the fast path of gs_modulix_enrichment_cache_get_or_fetch()
 *    and the "what's missing" scan of
 *    gs_modulix_enrichment_cache_prefetch_many()) take the reader lock only
 *    long enough to read/copy a value out of the table. Population (first
 *    encounter of an id) takes the writer lock only long enough to insert;
 *    the blocking daemon call itself always happens outside both locks, so
 *    concurrent lookups for other ids are never blocked behind a fetch in
 *    flight.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * GS_MODULIX_ENRICHMENT_EMPTY:
 *
 * Sentinel cache value recording that @app_id is known to have no
 * enrichment data (the daemon's response had no entry for it). Stored as a
 * `gchar *` value equal to `(gchar *)1` — never dereferenced, only ever
 * compared by pointer identity — so it round-trips through the cache's
 * `GHashTable` (whose value type is `gchar *`) without needing a separate
 * "found but empty" flag alongside each entry.
 */
#define GS_MODULIX_ENRICHMENT_EMPTY ((gchar *)1)

/**
 * gs_modulix_enrichment_cache_get_or_fetch:
 * @app_id: the AppStream component id to look up; must be a valid
 *   NUL-terminated string (not NULL — it is looked up in the hash table and
 *   passed on to mx_store_get_app_enrichment() on a miss).
 *
 * Returns the cached enrichment JSON for @app_id, populating the cache from
 * the daemon (via `mx_store_get_app_enrichment()`) on first call for that
 * id. A cache hit — whether pre-existing or just populated by this same
 * call — never re-fetches.
 *
 * @pre None. Safe to call from any thread; internally serialized by the
 *   cache's #GRWLock, and safe to call before the cache's first use (it
 *   lazily creates the backing #GHashTable on first insert).
 * @post On a miss, @app_id (and the daemon's answer, or
 *   #GS_MODULIX_ENRICHMENT_EMPTY if the daemon had nothing) is inserted
 *   into the cache, so a subsequent call for the same @app_id is a hit.
 *
 * Returns: (transfer full) (nullable): a newly heap-allocated JSON string
 *   the caller must free with g_free() (or g_autofree), or NULL when
 *   @app_id has no enrichment — either because the daemon returned nothing
 *   for it (cached as #GS_MODULIX_ENRICHMENT_EMPTY) or because the daemon
 *   call itself failed. Both cases are indistinguishable to the caller and
 *   both are cached identically (no retry on a later call).
 */
gchar *gs_modulix_enrichment_cache_get_or_fetch(const gchar *app_id);

/**
 * gs_modulix_enrichment_cache_prefetch_many:
 * @app_ids: (array length=n): app-ids to warm the cache for. Individual
 *   entries may be NULL or the empty string — those are skipped. May be
 *   NULL only if @n is also 0.
 * @n: number of entries in @app_ids.
 *
 * Splits @app_ids into hits (ids already present in the cache, whatever
 * their value — left untouched) and misses (ids not yet present), then
 * fetches enrichment for every miss in a single batched daemon call
 * (`mx_store_get_app_enrichment_many()`) and populates the cache with the
 * results: an id present in the daemon's reply object gets its JSON entry
 * re-serialized and cached; an id absent from the reply (daemon has no
 * enrichment for it, per the `GetAppEnrichment`/shim contract) or reached
 * when the batched call itself failed gets #GS_MODULIX_ENRICHMENT_EMPTY —
 * this is a negative cache entry, not a transient failure marker, so it is
 * never retried by a later call.
 *
 * @pre None. Safe to call from any thread; internally serialized by the
 *   cache's #GRWLock. No-op if @app_ids is NULL, @n is 0, or every id in
 *   @app_ids is already cached.
 * @post Every id in @app_ids that had no cache entry before the call has
 *   one after it (real JSON or #GS_MODULIX_ENRICHMENT_EMPTY). Subsequent
 *   gs_modulix_enrichment_cache_get_or_fetch() calls for these ids become
 *   cache hits and do not re-fetch.
 *
 * Returns: nothing; results are only observable through
 *   gs_modulix_enrichment_cache_get_or_fetch().
 */
void gs_modulix_enrichment_cache_prefetch_many(const gchar *const *app_ids,
                                               guint n);

/**
 * gs_modulix_enrichment_cache_clear:
 *
 * Discards every cached entry and destroys the backing #GHashTable and the
 * cache's #GRWLock. Called once, by `GsPluginModulix`'s finalize
 * (`gs-plugin-modulix.c`), as the cache's only teardown/eviction path —
 * there is no periodic or per-entry expiry (see the cache contract at the
 * top of this file).
 *
 * @pre Intended to be called at most once, at plugin shutdown: it calls
 *   g_rw_lock_clear() on #enrichment_lock, after which further use of that
 *   lock is undefined behaviour. No enrichment-cache call (this one
 *   included) is safe to make afterwards.
 * @post The cache is empty and unusable until the process re-initializes
 *   the underlying static state (which currently does not happen — this is
 *   a one-way shutdown call, not a "reset").
 */
void gs_modulix_enrichment_cache_clear(void);

G_END_DECLS
