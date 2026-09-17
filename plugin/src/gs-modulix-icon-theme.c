/*
 * gs-modulix-icon-theme.c — GtkIconTheme search-path setup
 *
 * Split out of the plugin's setup vfunc: this is one-shot process-wide
 * configuration, not plugin logic. gs-modulix-icon-resolver.c consumes its
 * result (it indexes whatever the theme knows once this has run).
 */

#include "gs-modulix-icon-theme.h"

#include <gtk/gtk.h>

/* Directories Papirus / Modulix-OS actually live in on a NixOS system-profile
 * install, in addition to XDG's own. `~/.nix-profile` resolves elsewhere and
 * carries no `icons/` — without /etc/profiles/per-user, outside a full GNOME
 * session the theme index built by gs-modulix-icon-resolver.c stays empty and
 * every icon falls straight through to level 2+. */
static GPtrArray *candidate_icon_dirs(void) {
  GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);

  g_ptr_array_add(dirs, g_strdup("/run/current-system/sw/share/icons"));
  g_ptr_array_add(dirs, g_build_filename(g_get_home_dir(), ".nix-profile",
                                         "share", "icons", NULL));
  g_ptr_array_add(dirs, g_build_filename("/etc/profiles/per-user",
                                         g_get_user_name(), "share", "icons",
                                         NULL));
  for (const gchar *const *d = g_get_system_data_dirs(); d && *d; d++)
    g_ptr_array_add(dirs, g_build_filename(*d, "icons", NULL));
  g_ptr_array_add(dirs, g_build_filename(g_get_user_data_dir(), "icons", NULL));

  return dirs;
}

void gs_modulix_icon_theme_setup(void) {
  GdkDisplay *display = gdk_display_get_default();
  if (display == NULL)
    return;

  GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);
  if (icon_theme == NULL)
    return;

  gtk_icon_theme_add_resource_path(icon_theme, "/org/modulix/software/icons");

  g_autoptr(GPtrArray) icon_dirs = candidate_icon_dirs();

  /* A single gtk_icon_theme_set_search_path() call instead of N
   * gtk_icon_theme_add_search_path() calls: each add invalidates the theme
   * and re-resolves every already-loaded icon. */
  g_auto(GStrv) existing = gtk_icon_theme_get_search_path(icon_theme);
  g_autoptr(GPtrArray) new_paths = g_ptr_array_new_with_free_func(g_free);

  for (GStrv p = existing; p && *p; p++)
    g_ptr_array_add(new_paths, g_strdup(*p));
  for (guint i = 0; i < icon_dirs->len; i++) {
    const gchar *dir = g_ptr_array_index(icon_dirs, i);
    if (g_file_test(dir, G_FILE_TEST_IS_DIR))
      g_ptr_array_add(new_paths, g_strdup(dir));
  }
  g_ptr_array_add(new_paths, NULL);

  gtk_icon_theme_set_search_path(icon_theme,
                                 (const gchar *const *)new_paths->pdata);
}
