/**
 * @file gs-modulix-config.h
 * @brief Compile-time knobs of the plugin: which sources it exposes, and the
 *        default result cap.
 */

#pragma once

/**
 * @brief Whether nixpkgs packages are exposed as a source.
 *
 * FALSE compiles out every package path — listing, searching and
 * install/uninstall alike — so the plugin then only ever offers modules.
 * Useful during development or on hardware where this source is not
 * available. Consumed by gs-plugin-modulix.c and gs-modulix-lifecycle.c.
 */
#define MODULIX_ENABLE_PACKAGES TRUE

/**
 * @brief Whether Modulix modules are exposed as a source.
 *
 * FALSE compiles out every module path, plugins (addons) included. Useful
 * during development or on hardware where this source is not available.
 * Consumed by gs-plugin-modulix.c and gs-modulix-lifecycle.c.
 */
#define MODULIX_ENABLE_MODULES TRUE

/**
 * @brief Maximum number of results requested from the daemon, in rows, when the
 *        query carries no limit of its own.
 *
 * Passed as the `max` argument of the search calls, so it bounds work on the
 * daemon side, not just what the UI shows.
 */
#define MODULIX_SEARCH_LIMIT 50
