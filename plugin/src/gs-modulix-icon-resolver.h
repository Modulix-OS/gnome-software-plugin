#pragma once

#include <glib.h>

#ifndef I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#endif
#include <gnome-software.h>

G_BEGIN_DECLS

/* Main-thread only: builds the theme-name index (GtkIconTheme is not
 * thread-safe) and scans the flatpak-appstream / gnome-software icon caches
 * once. Call after the plugin's own gtk_icon_theme_set_search_path(). */
void gs_modulix_icon_resolver_init (void);
void gs_modulix_icon_resolver_shutdown (void);

/* Resolves an icon following theme → local flatpak cache → local GS cache →
 * remote URL → generic fallback, and adds it to @app. Safe to call from any
 * thread once gs_modulix_icon_resolver_init() has run on the main thread.
 * @app_id, @icon_name, @name, @base_name and @url are all nullable. @name is
 * the raw nix attribute (before any variant-suffix stripping); @base_name is
 * the reduced attribute used as a last-resort themed-icon candidate — see
 * `icon_base_name()` in modulix-daemon's `src/store/entry.rs`.
 *
 * A themed-icon win (level 1) adds *two* separate unsized GThemedIcons
 * instead of folding the generic fallback into the same one:
 * gtk_icon_theme_lookup_by_gicon() resolves a multi-name GThemedIcon
 * theme-layer-major (all names in the top theme, then all names in the
 * first inherited theme, …), not name-major — so a generic name that the
 * top theme happens to ship itself (e.g. a branded "Modulix-OS" icon set's
 * own application-x-executable.svg) wins over the real candidate name that
 * only exists in an *inherited* theme (e.g. Papirus). Two separate icons
 * sidesteps this: gs_app_get_icon_for_size()'s pass 2 tries the real name
 * alone first and only advances to the fallback icon if that lookup
 * genuinely fails.
 *
 * Returns TRUE only when the added icon(s) resolved against the icon theme
 * (as opposed to a local file, a remote URL, or the generic fallback) — the
 * plugin uses this to avoid letting a later-arriving remote icon override an
 * already-correct themed one. */
gboolean gs_modulix_icon_resolve (GsApp *app, const gchar *app_id,
                                  const gchar *icon_name, const gchar *name,
                                  const gchar *base_name, const gchar *url);

G_END_DECLS
