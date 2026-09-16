#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Sentinel: module is known to have no plugins (no plugins namespace, or the
 * daemon call failed). */
#define GS_MODULIX_PLUGINS_CACHE_EMPTY ((gchar *)1)

/**
 * gs_modulix_plugins_cache_get_or_fetch:
 * @module_name: the Modulix module name
 *
 * Returns the cached plugins-list JSON for @module_name, fetching it from the
 * daemon (via mx_store_list_module_plugins) on first call. The details page
 * re-requests a module's plugins on every ADDONS refine, so without this
 * cache the same full-namespace `nix eval` reruns every time.
 *
 * Returns NULL when the daemon has nothing (no plugins namespace, or an
 * error). Returns a heap-allocated JSON string otherwise — caller must
 * g_free() it.
 */
gchar *gs_modulix_plugins_cache_get_or_fetch (const gchar *module_name);

void gs_modulix_plugins_cache_clear (void);

G_END_DECLS
