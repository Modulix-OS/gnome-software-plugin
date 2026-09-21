/**
 * @file gs-modulix-icon-theme.h
 * @brief One-shot GtkIconTheme search-path setup for a NixOS install.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * @brief Teaches the display's GtkIconTheme where NixOS keeps its icons.
 *
 * Registers the plugin's own resource icon path plus every system and user icon
 * directory a NixOS install can hold, which is what makes themed icon lookup
 * work outside a full GNOME session.
 *
 * @pre Main thread only, and exactly once during plugin setup. Must run
 *   *before* gs_modulix_icon_resolver_init(), which snapshots the theme's
 *   icon-name set — running it after would leave that snapshot empty.
 * @post The default display's icon theme has the plugin resource path and every
 *   existing candidate directory in its search path; directories that do not
 *   exist are left out. A no-op when there is no default display or no icon
 *   theme for it. The search path is replaced in one call rather than extended
 *   one entry at a time, so already-loaded icons are re-resolved only once.
 * @return None.
 */
void gs_modulix_icon_theme_setup(void);

G_END_DECLS
