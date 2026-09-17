#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Main-thread only, once at setup: registers the plugin's own resource icon
 * path and every system/user icon directory a NixOS install can hold onto the
 * default display's GtkIconTheme. No-op when there is no display.
 *
 * Must run *before* gs_modulix_icon_resolver_init(), which snapshots the
 * theme's icon-name set. */
void gs_modulix_icon_theme_setup(void);

G_END_DECLS
