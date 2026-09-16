/*
 * gs-modulix-icon-resolver.c — theme-first icon resolution
 *
 * Order (see CLAUDE.md "Icons"):
 *   1. themed(icon_name) → themed(app_id) → themed(name) → themed(base_name),
 *      whichever the current icon theme (Papirus/Modulix-OS/…) actually has.
 *   2. local flatpak-appstream icon cache (already on disk, no download).
 *   3. local gnome-software remote-icon cache, keyed by app-id — catches the
 *      icon even when the flatpak plugin fetched it from a different URL.
 *   4. the remote Flathub icon URL (the only level that touches the network).
 *   5. a generic "application-x-executable" themed icon, so an app is never
 *      left without any icon at all.
 *
 * gs_app_get_icon_for_size() only ever returns the *first* matching icon in
 * an app's icon list (a sized file/remote icon in its first pass, an unsized
 * themed one in its second) — handing it several would let the wrong level
 * win. This resolver therefore always returns exactly one GIcon.
 *
 * The theme-name index is built on the main thread (GtkIconTheme is not
 * thread-safe) by gs_modulix_icon_resolver_init(), then read from worker
 * threads (list_apps_thread / refine_thread) through a GRWLock. The flatpak
 * and gnome-software cache indexes are plain directory scans, also snapshot
 * once at init.
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

typedef struct {
  gchar *path;
  guint width;
} IconFileEntry;

static void icon_file_entry_free(IconFileEntry *entry) {
  g_free(entry->path);
  g_free(entry);
}

/* Guards all three indexes below. Zero-initialised static storage is a valid
 * unlocked GRWLock (no g_rw_lock_init() needed); never call g_rw_lock_clear()
 * on it — the plugin can be unloaded and setup_async() re-run within the same
 * process. */
static GRWLock resolver_lock;

static GHashTable *theme_names;    /* owned gchar* -> NULL, set semantics */
static GHashTable *flatpak_icons;  /* app_id -> IconFileEntry* */
static GHashTable *gs_cache_icons; /* app_id -> IconFileEntry* */

static GtkIconTheme *theme_instance; /* weak: owned by GdkDisplay */
static gulong theme_changed_id;

/* ── theme-name index ───────────────────────────────────────────────────── */

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

static void on_theme_changed(GtkIconTheme *theme,
                             gpointer user_data G_GNUC_UNUSED) {
  rebuild_theme_index(theme);
}

/* ── flatpak-appstream / gnome-software cache indexes ───────────────────── */

/* Strips a known icon extension, or returns NULL when @filename doesn't carry
 * one (not an icon file). */
static gchar *strip_icon_extension(const gchar *filename) {
  static const gchar *const exts[] = {".png", ".svg", ".svgz", ".jpg",
                                      ".jpeg"};
  for (guint i = 0; i < G_N_ELEMENTS(exts); i++) {
    if (g_str_has_suffix(filename, exts[i]))
      return g_strndup(filename, strlen(filename) - strlen(exts[i]));
  }
  return NULL;
}

/* {/var/lib/flatpak, $XDG_DATA_HOME/flatpak}/appstream/<remote>/<arch>/active/
 * icons/{128x128,64x64}/<app-id>.png — 128×128 scanned first so it wins over
 * 64×64 when both exist for the same app-id. */
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

/* ~/.cache/gnome-software/icons/<sha1(uri)>-<basename>, indexed by app-id
 * (the basename of a Flathub icon URL is always "<app-id>.png") — this is
 * what lets us reuse an icon the flatpak plugin already downloaded even when
 * our own icon URL differs. The sha1 prefix is always exactly 40 hex chars
 * followed by '-' (see gs_remote_icon_get_cache_filename() in gnome-software,
 * lib/gs-remote-icon.c). */
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
    /* Header-only read: cheap even across ~1000 cached files. */
    GdkPixbufFormat *fmt = gdk_pixbuf_get_file_info(full_path, &w, &h);
    IconFileEntry *entry = g_new0(IconFileEntry, 1);
    entry->path = g_steal_pointer(&full_path);
    entry->width = (fmt != NULL && w > 0) ? (guint)w : 128;
    g_hash_table_insert(index, g_steal_pointer(&app_id), entry);
  }
  return index;
}

/* ── lookups ─────────────────────────────────────────────────────────────── */

/* Lock-free: callers hold `resolver_lock` for reading around every call, so
 * a whole `gs_modulix_icon_resolve()` lookup phase takes the lock exactly
 * once instead of once per candidate/level. */
static gboolean theme_has_name_locked(const gchar *name) {
  if (name == NULL || *name == '\0')
    return FALSE;
  return theme_names != NULL && g_hash_table_contains(theme_names, name);
}

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

/* ── public API ──────────────────────────────────────────────────────────── */

/* Runs the two directory scans off the main thread — `setup_async` calls
 * `gs_modulix_icon_resolver_init()` synchronously on startup, and walking
 * `gdk_pixbuf_get_file_info()` over every file in the gnome-software icon
 * cache (potentially ~1000 files) must not delay it. Fire-and-forget: results
 * are published through `resolver_lock` same as `rebuild_theme_index()`, so a
 * lookup that races an in-progress scan just sees the previous (possibly
 * empty) index and falls through to a later icon level — never a crash. */
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

void gs_modulix_icon_resolver_init(void) {
  g_thread_unref(g_thread_new("modulix-icon-scan", scan_file_caches_thread, NULL));

  /* GtkIconTheme is not thread-safe: this part stays on the caller's thread
   * (the main thread, per this function's contract). No display (service
   * mode): the theme index stays empty and resolution starts at level 2. */
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

gboolean gs_modulix_icon_resolve(GsApp *app, const gchar *app_id,
                                 const gchar *icon_name, const gchar *name,
                                 const gchar *base_name, const gchar *url) {
  /* Level 1 — themed(icon_name) → themed(app_id) → themed(name) →
   * themed(base_name). Only "wins" if at least one specific name is actually
   * in the theme: checking against "application-x-executable" here would
   * make this level always match and starve levels 2-4. `base_name` is
   * skipped when it equals `name` (no variant-suffix was stripped) to avoid
   * stacking the same candidate twice. */
  gboolean skip_base = (g_strcmp0(name, base_name) == 0);
  const gchar *candidates[] = {icon_name, app_id, name,
                               skip_base ? NULL : base_name};

  /* Single reader-lock acquisition covering every lookup this resolution
   * needs (up to 4 theme-name checks + 2 file-index lookups), instead of one
   * acquisition per call as `theme_has_name`/`file_index_lookup` used to do. */
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
    /* Guaranteed fallback as a *separate* unsized icon (not folded into the
     * one above — see the "theme-layer-major, not name-major" note in the
     * header), so gs_app_get_icon_for_size()'s unsized-icon pass always
     * resolves to *something* even if a theme reload races the lookup
     * (fixes B4), without that fallback ever shadowing the real name. */
    GIcon *fallback = g_themed_icon_new("application-x-executable");
    gs_app_add_icon(app, fallback);
    g_object_unref(fallback);
    return TRUE;
  }

  /* Level 2 — local flatpak-appstream icon cache. */
  if (has_flatpak) {
    g_autoptr(GFile) file = g_file_new_for_path(flatpak_path);
    GIcon *icon = g_file_icon_new(file);
    gs_icon_set_width(icon, flatpak_width);
    gs_icon_set_height(icon, flatpak_width);
    gs_app_add_icon(app, icon);
    g_object_unref(icon);
    return FALSE;
  }

  /* Level 3 — local gnome-software remote-icon cache. */
  if (has_gs_cache) {
    g_autoptr(GFile) file = g_file_new_for_path(gs_cache_path);
    GIcon *icon = g_file_icon_new(file);
    gs_icon_set_width(icon, gs_cache_width);
    gs_icon_set_height(icon, gs_cache_width);
    gs_app_add_icon(app, icon);
    g_object_unref(icon);
    return FALSE;
  }

  /* Level 4 — remote Flathub icon URL: the only level that hits the network. */
  if (url != NULL && *url != '\0') {
    GIcon *icon = gs_modulix_remote_icon_new(url);
    gs_app_add_icon(app, icon);
    g_object_unref(icon);
    return FALSE;
  }

  /* Level 5 — an app must never end up with no icon at all. Returns FALSE:
   * unlike a real level-1 win, a later-arriving remote icon (from refine
   * enrichment) should still be allowed to replace this generic
   * placeholder. */
  GIcon *icon = g_themed_icon_new("application-x-executable");
  gs_app_add_icon(app, icon);
  g_object_unref(icon);
  return FALSE;
}
