#pragma once

/* ─── Feature flags ────────────────────────────────────────────────────────
 * Set to FALSE to compile out the corresponding source entirely (listing,
 * searching and install/uninstall alike). Useful during development or on
 * hardware where one path is not available. */
#define MODULIX_ENABLE_PACKAGES TRUE
#define MODULIX_ENABLE_MODULES TRUE

/* Fallback cap when the query carries no max-results of its own. */
#define MODULIX_SEARCH_LIMIT 50
