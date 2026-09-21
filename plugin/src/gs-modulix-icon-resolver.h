/**
 * @file gs-modulix-icon-resolver.h
 * @brief Public API of the theme-first icon resolver.
 *
 * Declares the lifecycle pair (`gs_modulix_icon_resolver_init()` /
 * `gs_modulix_icon_resolver_shutdown()`) that builds and tears down the
 * three lookup indexes, and `gs_modulix_icon_resolve()`, the single entry
 * point that turns an app's icon-related fields into exactly one `GIcon`
 * attached to a `GsApp`. See gs-modulix-icon-resolver.c for the full
 * resolution order and rationale (themed name → local flatpak-appstream
 * cache → local gnome-software remote-icon cache → remote Flathub URL →
 * generic fallback).
 */

#pragma once

#include <glib.h>

#ifndef I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#endif
#include <gnome-software.h>

G_BEGIN_DECLS

/**
 * @brief Builds the resolver's three lookup indexes for the first time.
 *
 * Snapshots the current `GtkIconTheme`'s icon-name set synchronously on the
 * calling thread (must be the main thread — `GtkIconTheme` is not
 * thread-safe) and subscribes to its `"changed"` signal so the theme-name
 * index is rebuilt whenever the active theme changes. The two on-disk icon
 * caches (local flatpak-appstream icons, local gnome-software remote-icon
 * cache) are scanned once on a background worker thread spawned internally,
 * so this call does not block on directory/file I/O.
 *
 * @pre Must be called on the main thread.
 * @pre Should be called after the plugin's own
 *   `gtk_icon_theme_set_search_path()` (gs_modulix_icon_theme_setup()), so
 *   the theme-name snapshot includes the plugin's bundled search paths.
 * @post The theme-name index reflects the display's current `GtkIconTheme`
 *   (or stays empty when there is no default `GdkDisplay`, e.g. service
 *   mode — resolution then starts at level 2). The two file-cache indexes
 *   are empty until the background scan thread finishes and publishes them;
 *   until then, gs_modulix_icon_resolve() falls through to a later level.
 * @return void.
 */
void gs_modulix_icon_resolver_init (void);
/**
 * @brief Tears down the resolver's indexes and theme-change subscription.
 *
 * Disconnects the `"changed"` handler installed by
 * gs_modulix_icon_resolver_init() (if any) and drops all three lookup
 * indexes (theme names, flatpak-appstream cache, gnome-software cache),
 * releasing their `GHashTable`s.
 *
 * @pre None; safe to call even if gs_modulix_icon_resolver_init() was never
 *   called or only partially initialized (no display).
 * @post All three indexes are NULL and the writer lock is left unlocked.
 *   The resolver can be re-initialized afterwards via a fresh call to
 *   gs_modulix_icon_resolver_init() (the plugin may be unloaded and
 *   `setup_async()` re-run within the same process).
 * @return void.
 */
void gs_modulix_icon_resolver_shutdown (void);

/**
 * @brief Resolves the best available icon for one app and attaches it to @p app.
 *
 * Tries, in order, until one source wins:
 *   1. Themed icon: `icon_name` → `app_id` → `name` → `base_name` (skipped
 *      when equal to `name`), whichever the current `GtkIconTheme` actually
 *      has (checked against the index built by
 *      gs_modulix_icon_resolver_init()). On a win, two unsized `GThemedIcon`s
 *      are added to @p app: one built from every matching candidate name (via
 *      `g_themed_icon_append_name()`), plus a separate
 *      `"application-x-executable"` `GThemedIcon` appended after it as a
 *      guaranteed fallback for `gs_app_get_icon_for_size()`'s unsized pass.
 *      The fallback is kept in its own icon rather than folded into the
 *      first one because `gtk_icon_theme_lookup_by_gicon()` resolves a
 *      multi-name `GThemedIcon` theme-layer-major (all names in the top
 *      theme, then all names in the first inherited theme, …), not
 *      name-major — so a generic name the top theme happens to ship itself
 *      (e.g. a branded "Modulix-OS" icon set's own
 *      `application-x-executable.svg`) would otherwise win over the real
 *      candidate name that only exists in an *inherited* theme (e.g.
 *      Papirus). Two separate icons sidestep this: `gs_app_get_icon_for_size()`'s
 *      pass 2 tries the real name alone first and only advances to the
 *      fallback icon if that lookup genuinely fails.
 *   2. Local flatpak-appstream icon cache, looked up by `app_id` (128×128
 *      scanned before 64×64, so 128×128 wins when both exist). Adds one
 *      sized `GFileIcon` (width = height = the source file's on-disk size,
 *      128 or 64).
 *   3. Local gnome-software remote-icon cache
 *      (`~/.cache/gnome-software/icons/`), looked up by `app_id`. Adds one
 *      sized `GFileIcon` (width = height = the cached file's real pixel
 *      width read via `gdk_pixbuf_get_file_info()`, or 128 if that probe
 *      fails).
 *   4. Remote Flathub icon URL (`url`), via `gs_modulix_remote_icon_new()`
 *      (128×128 `GsRemoteIcon`). This is the only level that performs
 *      network I/O — and even then only schedules a download; it does not
 *      block the caller. The other levels only touch local state (an
 *      in-memory index) or, for levels 2/3, local disk (the index itself is
 *      pre-scanned; only the small `gdk_pixbuf_get_file_info()` header reads
 *      happened earlier, during gs_modulix_icon_resolver_init()'s
 *      background scan, not on this call).
 *   5. Generic `"application-x-executable"` themed `GIcon`, always available,
 *      so an app is never left without any icon at all.
 *
 * Levels 1-3 are decided under a single `GRWLock` reader-lock acquisition
 * covering the whole lookup phase (up to 4 theme-name checks plus the 2
 * file-index lookups), rather than one acquisition per candidate.
 *
 * @param app The `GsApp` to attach the resolved icon(s) to. Must be
 *   non-NULL. Ownership of `app` is not transferred; the function only adds
 *   icon(s) to it via `gs_app_add_icon()` (which takes its own reference).
 * @param app_id Canonical AppStream id, nullable. Used as a level-1 themed
 *   candidate and as the lookup key for levels 2 and 3.
 * @param icon_name `meta.mainProgram`-derived themed-icon candidate (e.g.
 *   `vscode` → `code`), nullable. Tried first at level 1.
 * @param name Raw nix attribute name, nullable. Tried at level 1 after
 *   `icon_name` and `app_id`.
 * @param base_name `name` with any known variant/edition suffix stripped
 *   (see `icon_base_name()` in modulix-daemon's `src/store/entry.rs`),
 *   nullable. Tried last at level 1, and skipped entirely when it equals
 *   `name` (nothing was stripped) to avoid stacking the same candidate
 *   twice.
 * @param url Remote Flathub/Flatpak icon URL, nullable. Only used at level
 *   4, and only when levels 1-3 all miss.
 *
 * @pre gs_modulix_icon_resolver_init() must have completed on the main
 *   thread before this is called (the theme-name index may otherwise be
 *   empty and the two file-cache indexes may still be mid-scan — neither is
 *   a correctness problem, both just widen the miss that falls through to a
 *   later level).
 * @pre Safe to call from any thread (list/refine worker threads included):
 *   all shared state is read under `resolver_lock`.
 * @post Exactly one `GIcon` is added to @p app on levels 2-5, or exactly two
 *   (the real themed icon plus the guaranteed fallback) on level 1 —
 *   `gs_app_get_icon_for_size()` only ever consults the first matching icon
 *   in its list, so adding more than the documented count per level would
 *   let the wrong source win.
 * @return TRUE only when the added icon(s) resolved against the icon theme
 *   (level 1) — as opposed to a local file, a remote URL, or the generic
 *   fallback. FALSE for every other level, including level 5 (no icon
 *   resolved at all): the caller uses this to avoid letting a
 *   later-arriving remote icon override an already-correct themed one,
 *   while still allowing it to replace a generic placeholder.
 */
gboolean gs_modulix_icon_resolve (GsApp *app, const gchar *app_id,
                                  const gchar *icon_name, const gchar *name,
                                  const gchar *base_name, const gchar *url);

G_END_DECLS
