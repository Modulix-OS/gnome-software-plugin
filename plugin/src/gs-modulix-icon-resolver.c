/**
 * @file gs-modulix-icon-resolver.c
 * @brief Theme-first icon resolution for Modulix `GsApp`s.
 *
 * Resolution order (see CLAUDE.md "Icons"), tried until one wins:
 *   1. themed(icon_name) → themed(app_id) → themed(name) → themed(base_name),
 *      whichever the current icon theme (Papirus/Modulix-OS/…) actually has.
 *      Wins iff at least one candidate name is present in the theme-name
 *      index; on a win, two unsized `GThemedIcon`s are added to the app (the
 *      matched candidate(s), plus a separate guaranteed
 *      "application-x-executable" fallback) — see gs_modulix_icon_resolve().
 *   2. local flatpak-appstream icon cache (already on disk, no download),
 *      keyed by app-id, 128×128 preferred over 64×64.
 *   3. local gnome-software remote-icon cache, keyed by app-id — catches the
 *      icon even when the flatpak plugin fetched it from a different URL.
 *   4. the remote Flathub icon URL (the only level that touches the
 *      network; the download itself is scheduled asynchronously by
 *      gs_modulix_remote_icon_new()/`GsRemoteIcon`, not performed inline).
 *   5. a generic "application-x-executable" themed icon, so an app is never
 *      left without any icon at all.
 *
 * gs_app_get_icon_for_size() only ever returns the *first* matching icon in
 * an app's icon list (a sized file/remote icon in its first pass, an unsized
 * themed one in its second) — handing it several would let the wrong level
 * win. This resolver therefore always adds exactly one icon "unit" per call
 * (one GIcon on levels 2-5, two on level 1 as described above).
 *
 * The theme-name index is built on the main thread (GtkIconTheme is not
 * thread-safe) by gs_modulix_icon_resolver_init(), then read from worker
 * threads (list_apps_thread / refine_thread) through a GRWLock. The flatpak
 * and gnome-software cache indexes are plain directory scans, run once on a
 * background thread spawned by gs_modulix_icon_resolver_init() and
 * republished (also under the GRWLock) when that scan completes; no
 * network I/O occurs anywhere in this file except indirectly, by handing a
 * URL to gs_modulix_remote_icon_new() at level 4.
 */

#ifndef I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#endif

#include "gs-modulix-icon-resolver.h"

#include "gs-modulix-app.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gnome-software.h>
#include <gtk/gtk.h>
#include <string.h>

/**
 * @brief One entry of the flatpak-appstream or gnome-software file-cache index.
 *
 * @var IconFileEntry::path Absolute path to the on-disk icon file (owned by
 *   this struct; freed by icon_file_entry_free()).
 * @var IconFileEntry::width Pixel width (== height, icons are square) of the
 *   file at @c path: the scanned directory's size bucket for a
 *   flatpak-appstream entry (128 or 64), or the value read from the image's
 *   own header (falling back to 128 on a failed probe) for a
 *   gnome-software-cache entry.
 */
typedef struct {
  gchar *path;
  guint width;
} IconFileEntry;

/**
 * @brief Frees one #IconFileEntry, including its owned path string.
 *
 * @param entry Entry to free. Must be non-NULL. Ownership of @p entry (and
 *   of its @c path field) is taken and released by this call; the caller
 *   must not use @p entry afterwards.
 * @pre @p entry is non-NULL.
 * @post @p entry and its @c path are freed.
 * @return void.
 */
static void icon_file_entry_free(IconFileEntry *entry) {
  g_free(entry->path);
  g_free(entry);
}

static GRWLock resolver_lock;

static GHashTable *theme_names;
static GHashTable *flatpak_icons;
static GHashTable *gs_cache_icons;

static GtkIconTheme *theme_instance;
static gulong theme_changed_id;

/**
 * @brief Snapshots @p theme's full icon-name set into a fresh set-semantics table.
 *
 * Calls `gtk_icon_theme_get_icon_names()` to list every icon name the theme
 * (including all its inherited parent themes) currently exposes, builds a
 * new owned `GHashTable` from it, then publishes it as the process-wide
 * `theme_names` index under the writer side of `resolver_lock`. The
 * previous table (if any) is unreffed only after the lock is released, so
 * a concurrent reader under the reader lock always sees a complete table.
 *
 * @param theme The `GtkIconTheme` to snapshot. Must be non-NULL. Not
 *   retained beyond this call — ownership is not taken.
 * @pre Must run on the main thread (`GtkIconTheme` is not thread-safe).
 * @post The global `theme_names` table is replaced with a new table
 *   containing exactly @p theme's current icon names (owned `gchar*` keys,
 *   NULL values). The old table, if any, is unreffed.
 * @return void.
 */
static void rebuild_theme_index(GtkIconTheme *theme) {
  GHashTable *names =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_auto(GStrv) icon_names = gtk_icon_theme_get_icon_names(theme);
  for (GStrv p = icon_names; p && *p; p++)
    g_hash_table_add(names, g_strdup(*p));

  g_rw_lock_writer_lock(&resolver_lock);
  GHashTable *old = theme_names;
  theme_names = names;
  g_rw_lock_writer_unlock(&resolver_lock);
  g_clear_pointer(&old, g_hash_table_unref);
}

/**
 * @brief `GtkIconTheme::changed` handler: rebuilds the theme-name index.
 *
 * @param theme The `GtkIconTheme` that changed (the theme in use, a new
 *   theme, or its search path). Not retained; ownership stays with the
 *   caller (GTK).
 * @param user_data Unused (connected with a NULL user_data).
 * @pre Runs on the main thread, as a signal callback on the theme owned by
 *   the default `GdkDisplay`.
 * @post The global `theme_names` index reflects @p theme's new icon-name
 *   set (see rebuild_theme_index()).
 * @return void.
 */
static void on_theme_changed(GtkIconTheme *theme,
                             gpointer user_data G_GNUC_UNUSED) {
  rebuild_theme_index(theme);
}

/**
 * @brief Strips a known icon file extension from @p filename, if present.
 *
 * Tries `.png`, `.svg`, `.svgz`, `.jpg`, `.jpeg` (in that order) as
 * suffixes of @p filename.
 *
 * @param filename Filename to strip, e.g. `"org.foo.App.png"`. Must be
 *   non-NULL. Not modified; not retained.
 * @pre @p filename is non-NULL.
 * @post None (pure function).
 * @return A newly-allocated string with the matched extension removed
 *   (transfer full — caller must `g_free()` it), or NULL when @p filename
 *   ends in none of the known extensions (i.e. it is not recognized as an
 *   icon file).
 */
static gchar *strip_icon_extension(const gchar *filename) {
  static const gchar *const exts[] = {".png", ".svg", ".svgz", ".jpg",
                                      ".jpeg"};
  for (guint i = 0; i < G_N_ELEMENTS(exts); i++) {
    if (g_str_has_suffix(filename, exts[i]))
      return g_strndup(filename, strlen(filename) - strlen(exts[i]));
  }
  return NULL;
}

/**
 * @brief Scans both system and per-user flatpak-appstream icon trees into an index.
 *
 * Walks `{/var/lib/flatpak, $XDG_DATA_HOME/flatpak}/appstream/<remote>/
 * <arch>/active/icons/{128x128,64x64}/<app-id>.<ext>` for every remote and
 * arch subdirectory found, preferring 128×128 over 64×64 (128×128 is
 * scanned first per app-id and the first entry found for a given app-id is
 * kept — subsequent hits, including a same-size or smaller one from a
 * later remote/arch, are skipped). Directories that don't exist are
 * silently skipped (missing flatpak install, no icons for a given
 * remote/arch/size).
 *
 * @pre None.
 * @post None beyond the returned table (no global/shared state is touched
 *   — this is a pure filesystem read). Performs synchronous directory and
 *   filename I/O only (no file contents are read).
 * @return A newly-allocated `GHashTable` (transfer full — caller must
 *   `g_hash_table_unref()` it) mapping owned `gchar*` app-id to owned
 *   #IconFileEntry* (freed via icon_file_entry_free()). Empty (not NULL)
 *   when no flatpak-appstream icons are found anywhere.
 */
static GHashTable *scan_flatpak_appstream_icons(void) {
  GHashTable *index = g_hash_table_new_full(
      g_str_hash, g_str_equal, g_free, (GDestroyNotify)icon_file_entry_free);

  g_autofree gchar *user_base =
      g_build_filename(g_get_user_data_dir(), "flatpak", NULL);
  const gchar *bases[] = {"/var/lib/flatpak", user_base};
  const guint sizes[] = {128, 64};

  for (guint b = 0; b < G_N_ELEMENTS(bases); b++) {
    g_autofree gchar *appstream_dir =
        g_build_filename(bases[b], "appstream", NULL);
    g_autoptr(GDir) remotes = g_dir_open(appstream_dir, 0, NULL);
    if (remotes == NULL)
      continue;
    const gchar *remote;
    while ((remote = g_dir_read_name(remotes)) != NULL) {
      g_autofree gchar *remote_dir =
          g_build_filename(appstream_dir, remote, NULL);
      g_autoptr(GDir) arches = g_dir_open(remote_dir, 0, NULL);
      if (arches == NULL)
        continue;
      const gchar *arch;
      while ((arch = g_dir_read_name(arches)) != NULL) {
        for (guint s = 0; s < G_N_ELEMENTS(sizes); s++) {
          g_autofree gchar *size_name = g_strdup_printf("%ux%u", sizes[s], sizes[s]);
          g_autofree gchar *icons_dir = g_build_filename(
              remote_dir, arch, "active", "icons", size_name, NULL);
          g_autoptr(GDir) icons = g_dir_open(icons_dir, 0, NULL);
          if (icons == NULL)
            continue;
          const gchar *fname;
          while ((fname = g_dir_read_name(icons)) != NULL) {
            g_autofree gchar *app_id = strip_icon_extension(fname);
            if (app_id == NULL || *app_id == '\0')
              continue;
            if (g_hash_table_contains(index, app_id))
              continue;
            IconFileEntry *entry = g_new0(IconFileEntry, 1);
            entry->path = g_build_filename(icons_dir, fname, NULL);
            entry->width = sizes[s];
            g_hash_table_insert(index, g_steal_pointer(&app_id), entry);
          }
        }
      }
    }
  }
  return index;
}

/**
 * @brief Scans the local gnome-software remote-icon cache directory into an index.
 *
 * Walks `~/.cache/gnome-software/icons/<sha1(uri)>-<basename>`, where the
 * sha1 prefix is always exactly 40 hex characters followed by `-` (see
 * `gs_remote_icon_get_cache_filename()` in gnome-software's
 * `lib/gs-remote-icon.c`) and, for a Flathub icon, `<basename>` is always
 * `<app-id>.<ext>`. For each filename, the app-id is recovered by
 * stripping the sha1 prefix and the extension (via strip_icon_extension());
 * the first entry found for a given app-id is kept. For each kept entry,
 * `gdk_pixbuf_get_file_info()` performs a header-only read of the image
 * file to recover its real pixel width (falling back to 128 when the probe
 * fails or reports a non-positive width) — cheap even scanned across
 * roughly a thousand cached files, since only the header is read, not the
 * pixel data.
 *
 * @pre None.
 * @post None beyond the returned table (no global/shared state is touched).
 *   Performs synchronous directory, filename and image-header I/O (no full
 *   file/pixel-data reads).
 * @return A newly-allocated `GHashTable` (transfer full — caller must
 *   `g_hash_table_unref()` it) mapping owned `gchar*` app-id to owned
 *   #IconFileEntry* (freed via icon_file_entry_free()). Empty (not NULL)
 *   when the cache directory doesn't exist or is empty.
 */
static GHashTable *scan_gs_cache_icons(void) {
  GHashTable *index = g_hash_table_new_full(
      g_str_hash, g_str_equal, g_free, (GDestroyNotify)icon_file_entry_free);

  g_autofree gchar *cache_dir =
      g_build_filename(g_get_user_cache_dir(), "gnome-software", "icons", NULL);
  g_autoptr(GDir) dir = g_dir_open(cache_dir, 0, NULL);
  if (dir == NULL)
    return index;

  const gchar *fname;
  while ((fname = g_dir_read_name(dir)) != NULL) {
    if (strlen(fname) <= 41 || fname[40] != '-')
      continue;
    g_autofree gchar *app_id = strip_icon_extension(fname + 41);
    if (app_id == NULL || *app_id == '\0')
      continue;
    if (g_hash_table_contains(index, app_id))
      continue;

    g_autofree gchar *full_path = g_build_filename(cache_dir, fname, NULL);
    gint w = 0, h = 0;
    GdkPixbufFormat *fmt = gdk_pixbuf_get_file_info(full_path, &w, &h);
    IconFileEntry *entry = g_new0(IconFileEntry, 1);
    entry->path = g_steal_pointer(&full_path);
    entry->width = (fmt != NULL && w > 0) ? (guint)w : 128;
    g_hash_table_insert(index, g_steal_pointer(&app_id), entry);
  }
  return index;
}

/**
 * @brief Checks whether @p name is present in the current theme-name index.
 *
 * @param name Icon name to look up, nullable.
 * @pre Caller must hold `resolver_lock` (reader or writer) around this
 *   call, so the read of the shared `theme_names` table is safe.
 * @post None (pure lookup, no state mutated).
 * @return TRUE iff @p name is non-NULL, non-empty, and present in the
 *   global `theme_names` index (which may itself be NULL, e.g. before the
 *   first gs_modulix_icon_resolver_init() call or when there is no
 *   display — treated as "not found"). FALSE otherwise.
 */
static gboolean theme_has_name_locked(const gchar *name) {
  if (name == NULL || *name == '\0')
    return FALSE;
  return theme_names != NULL && g_hash_table_contains(theme_names, name);
}

/**
 * @brief Looks up @p key in a file-cache index and copies out its entry.
 *
 * @param index The file-cache index to search (`flatpak_icons` or
 *   `gs_cache_icons`), nullable (treated as "not found").
 * @param key App-id to look up, nullable.
 * @param out_path Output parameter receiving a newly-allocated copy of the
 *   matched entry's path (transfer full — caller must `g_free()` it) when
 *   the function returns TRUE. Must be non-NULL. Left untouched when the
 *   function returns FALSE.
 * @param out_width Output parameter receiving the matched entry's width
 *   when the function returns TRUE. Must be non-NULL. Left untouched when
 *   the function returns FALSE.
 * @pre Caller must hold `resolver_lock` (reader or writer) around this
 *   call, so the read of @p index is safe.
 * @post On a hit, `*out_path` and `*out_width` are set from the matched
 *   #IconFileEntry. On a miss, neither output parameter is touched.
 * @return TRUE iff @p key is non-NULL/non-empty, @p index is non-NULL, and
 *   an entry for @p key exists in @p index. FALSE otherwise.
 */
static gboolean file_index_lookup_locked(GHashTable *index, const gchar *key,
                                         gchar **out_path, guint *out_width) {
  if (key == NULL || *key == '\0' || index == NULL)
    return FALSE;
  IconFileEntry *entry = g_hash_table_lookup(index, key);
  if (entry == NULL)
    return FALSE;
  *out_path = g_strdup(entry->path);
  *out_width = entry->width;
  return TRUE;
}

/**
 * @brief Background-thread body: (re-)scans both file-cache indexes and publishes them.
 *
 * Runs scan_flatpak_appstream_icons() and scan_gs_cache_icons() off the
 * main thread, then publishes both resulting tables as the global
 * `flatpak_icons`/`gs_cache_icons` indexes under the writer side of
 * `resolver_lock`, unreffing the previous tables only after the lock is
 * released. Runs off the main thread because gs_modulix_icon_resolver_init()
 * spawns it synchronously from `setup_async` at startup, and walking
 * `gdk_pixbuf_get_file_info()` over every file in the gnome-software icon
 * cache (potentially ~1000 files) must not delay that startup path.
 * Fire-and-forget: results are published through `resolver_lock` the same
 * way rebuild_theme_index() publishes the theme-name index, so a lookup
 * racing an in-progress scan just sees the previous (possibly empty) index
 * and falls through to a later icon level — never a crash.
 *
 * @param user_data Unused (thread function signature requirement).
 * @pre Spawned by gs_modulix_icon_resolver_init() via `g_thread_new()`;
 *   not intended to be called directly.
 * @post `flatpak_icons` and `gs_cache_icons` reflect the on-disk state at
 *   scan time. A gs_modulix_icon_resolve() lookup racing an in-progress
 *   scan simply sees the previous (possibly still-empty) indexes and falls
 *   through to a later resolution level — never a crash or torn read.
 * @return NULL (unused `GThreadFunc` return value).
 */
static gpointer scan_file_caches_thread(gpointer user_data G_GNUC_UNUSED) {
  GHashTable *new_flatpak = scan_flatpak_appstream_icons();
  GHashTable *new_gs_cache = scan_gs_cache_icons();

  g_rw_lock_writer_lock(&resolver_lock);
  GHashTable *old_flatpak = flatpak_icons;
  GHashTable *old_gs_cache = gs_cache_icons;
  flatpak_icons = new_flatpak;
  gs_cache_icons = new_gs_cache;
  g_rw_lock_writer_unlock(&resolver_lock);
  g_clear_pointer(&old_flatpak, g_hash_table_unref);
  g_clear_pointer(&old_gs_cache, g_hash_table_unref);
  return NULL;
}

/**
 * @brief Definition of gs_modulix_icon_resolver_init(); full contract documented in
 *   gs-modulix-icon-resolver.h.
 *
 * Spawns the background file-cache scan (scan_file_caches_thread()) first,
 * then — synchronously, on the calling thread — snapshots the default
 * display's `GtkIconTheme` via rebuild_theme_index() and subscribes to its
 * `"changed"` signal (only once: a second call with `theme_instance`
 * already set does not reconnect).
 *
 * @pre Main thread only. Should run after the plugin's own
 *   `gtk_icon_theme_set_search_path()` (gs_modulix_icon_theme_setup()).
 * @post See header. When there is no default `GdkDisplay` or no
 *   `GtkIconTheme` for it (service mode), returns early after spawning the
 *   file-cache scan thread — the theme-name index stays empty/unset and
 *   resolution starts at level 2.
 * @return void.
 */
void gs_modulix_icon_resolver_init(void) {
  g_thread_unref(g_thread_new("modulix-icon-scan", scan_file_caches_thread, NULL));

  GdkDisplay *display = gdk_display_get_default();
  if (display == NULL)
    return;
  GtkIconTheme *theme = gtk_icon_theme_get_for_display(display);
  if (theme == NULL)
    return;

  rebuild_theme_index(theme);
  if (theme_instance == NULL) {
    theme_instance = theme;
    theme_changed_id =
        g_signal_connect(theme, "changed", G_CALLBACK(on_theme_changed), NULL);
  }
}

/**
 * @brief Definition of gs_modulix_icon_resolver_shutdown(); full contract documented
 *   in gs-modulix-icon-resolver.h.
 *
 * `resolver_lock` itself is never destroyed (no g_rw_lock_clear()) by this
 * function: GNOME Software can unload and reload this plugin within the same
 * process, and a later gs_modulix_icon_resolver_init()/gs_modulix_icon_resolve()
 * call would otherwise lock an already-destroyed GRWLock.
 *
 * @pre None; idempotent.
 * @post See header: theme-change subscription disconnected, all three
 *   indexes dropped and set to NULL under the writer lock.
 * @return void.
 */
void gs_modulix_icon_resolver_shutdown(void) {
  if (theme_instance != NULL && theme_changed_id != 0)
    g_signal_handler_disconnect(theme_instance, theme_changed_id);
  theme_instance = NULL;
  theme_changed_id = 0;

  g_rw_lock_writer_lock(&resolver_lock);
  GHashTable *old_theme = theme_names;
  GHashTable *old_flatpak = flatpak_icons;
  GHashTable *old_gs_cache = gs_cache_icons;
  theme_names = NULL;
  flatpak_icons = NULL;
  gs_cache_icons = NULL;
  g_rw_lock_writer_unlock(&resolver_lock);
  g_clear_pointer(&old_theme, g_hash_table_unref);
  g_clear_pointer(&old_flatpak, g_hash_table_unref);
  g_clear_pointer(&old_gs_cache, g_hash_table_unref);
}

/**
 * @brief Definition of gs_modulix_icon_resolve(); full contract (all 5 levels,
 *   parameters, ownership, return semantics) documented in
 *   gs-modulix-icon-resolver.h — see that header before reading this body.
 *
 * Level 1 only "wins" when at least one of `icon_name`/`app_id`/`name`/
 * `base_name` is itself present in the theme-name index: candidates are
 * never checked against `"application-x-executable"` here, since that name
 * is virtually always present and would make level 1 always match, starving
 * levels 2-4 for every app.
 *
 * @param app See header.
 * @param app_id See header.
 * @param icon_name See header.
 * @param name See header.
 * @param base_name See header.
 * @param url See header.
 * @pre See header.
 * @post See header.
 * @return See header.
 */
gboolean gs_modulix_icon_resolve(GsApp *app, const gchar *app_id,
                                 const gchar *icon_name, const gchar *name,
                                 const gchar *base_name, const gchar *url) {
  gboolean skip_base = (g_strcmp0(name, base_name) == 0);
  const gchar *candidates[] = {icon_name, app_id, name,
                               skip_base ? NULL : base_name};

  gboolean any_themed = FALSE;
  g_autofree gchar *flatpak_path = NULL;
  guint flatpak_width = 0;
  gboolean has_flatpak = FALSE;
  g_autofree gchar *gs_cache_path = NULL;
  guint gs_cache_width = 0;
  gboolean has_gs_cache = FALSE;

  g_rw_lock_reader_lock(&resolver_lock);
  for (guint i = 0; i < G_N_ELEMENTS(candidates); i++) {
    if (theme_has_name_locked(candidates[i])) {
      any_themed = TRUE;
      break;
    }
  }
  if (!any_themed) {
    has_flatpak = file_index_lookup_locked(flatpak_icons, app_id,
                                           &flatpak_path, &flatpak_width);
    if (!has_flatpak)
      has_gs_cache = file_index_lookup_locked(gs_cache_icons, app_id,
                                              &gs_cache_path, &gs_cache_width);
  }
  g_rw_lock_reader_unlock(&resolver_lock);

  if (any_themed) {
    GIcon *icon = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(candidates); i++) {
      if (candidates[i] == NULL || *candidates[i] == '\0')
        continue;
      if (icon == NULL)
        icon = g_themed_icon_new(candidates[i]);
      else
        g_themed_icon_append_name(G_THEMED_ICON(icon), candidates[i]);
    }
    gs_app_add_icon(app, icon);
    g_object_unref(icon);
    GIcon *fallback = g_themed_icon_new("application-x-executable");
    gs_app_add_icon(app, fallback);
    g_object_unref(fallback);
    return TRUE;
  }

  if (has_flatpak) {
    g_autoptr(GFile) file = g_file_new_for_path(flatpak_path);
    GIcon *icon = g_file_icon_new(file);
    gs_icon_set_width(icon, flatpak_width);
    gs_icon_set_height(icon, flatpak_width);
    gs_app_add_icon(app, icon);
    g_object_unref(icon);
    return FALSE;
  }

  if (has_gs_cache) {
    g_autoptr(GFile) file = g_file_new_for_path(gs_cache_path);
    GIcon *icon = g_file_icon_new(file);
    gs_icon_set_width(icon, gs_cache_width);
    gs_icon_set_height(icon, gs_cache_width);
    gs_app_add_icon(app, icon);
    g_object_unref(icon);
    return FALSE;
  }

  if (url != NULL && *url != '\0') {
    GIcon *icon = gs_modulix_remote_icon_new(url);
    gs_app_add_icon(app, icon);
    g_object_unref(icon);
    return FALSE;
  }

  GIcon *icon = g_themed_icon_new("application-x-executable");
  gs_app_add_icon(app, icon);
  g_object_unref(icon);
  return FALSE;
}
