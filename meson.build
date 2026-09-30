project(
  'gs-plugin-modulix',
  'c',
  version : '0.1.0',
  meson_version : '>= 1.0',
  default_options : [
    'warning_level=2',
    'c_std=gnu17',
  ],
)

fs    = import('fs')
i18n  = import('i18n')
gnome = import('gnome')

gettext_package = 'gs-plugin-modulix'
localedir       = get_option('prefix') / get_option('localedir')

# ─── Dependencies ─────────────────────────────────────────────────────────────

gnome_software_dep = dependency('gnome-software', version : '>= 49')
glib_dep           = dependency('glib-2.0',  version : '>= 2.76')
gobject_dep        = dependency('gobject-2.0')
gio_dep            = dependency('gio-2.0')
gio_unix_dep       = dependency('gio-unix-2.0')
gtk4_dep           = dependency('gtk4')
gdk_pixbuf_dep     = dependency('gdk-pixbuf-2.0')
appstream_dep      = dependency('appstream', version : '>= 1.0')
libsoup_dep        = dependency('libsoup-3.0')
xmlb_dep           = dependency('xmlb')  # gnome-software.h → gs-silo-wrapper.h → <xmlb.h>

# Plugin API version (e.g. plugins-23 for GS 49, plugins-24 for GS 50) is read
# from gnome-software itself so we never hardcode it. Override with -Dplugindir
# for a local dev install under ~/.local.
gs_plugindir   = gnome_software_dep.get_variable(pkgconfig : 'plugindir')
plugins_subdir = fs.name(gs_plugindir)

plugindir = get_option('plugindir')
if plugindir == ''
  plugindir = get_option('libdir') / 'gnome-software' / plugins_subdir
endif

# ─── Plugin ───────────────────────────────────────────────────────────────────

plugin_sources = files(
  'plugin/src/gs-plugin-modulix.c',
  'plugin/src/gs-modulix-app.c',
  'plugin/src/gs-modulix-icon-theme.c',
  'plugin/src/gs-modulix-lifecycle.c',
  'plugin/src/gs-modulix-list.c',
  'plugin/src/gs-modulix-refine.c',
  'plugin/src/gs-modulix-update.c',
  'plugin/src/gs-modulix-upgrade.c',
  'plugin/src/gs-modulix-enrichment-cache.c',
  'plugin/src/gs-modulix-icon-cache.c',
  'plugin/src/gs-modulix-icon-resolver.c',
  'plugin/src/gs-modulix-plugins-cache.c',
  'plugin/src/gs-modulix-markup.c',
  'plugin/src/dbus/gs-modulix-bus.c',
  'plugin/src/dbus/gs-modulix-store1.c',
  'plugin/src/dbus/gs-modulix-daemon1.c',
)

modulix_resources = gnome.compile_resources(
  'modulix-resources',
  'data/modulix.gresource.xml',
  source_dir : 'data',
  c_name     : 'modulix',
)

shared_module(
  'gs_plugin_modulix',
  sources : [plugin_sources, modulix_resources],
  include_directories : include_directories('plugin/src'),
  dependencies : [
    gnome_software_dep,
    glib_dep,
    gobject_dep,
    gio_dep,
    gio_unix_dep,
    gtk4_dep,
    gdk_pixbuf_dep,
    appstream_dep,
    libsoup_dep,
    xmlb_dep,
  ],
  install     : true,
  install_dir : plugindir,
  name_prefix : 'lib',
  c_args : [
    '-DG_LOG_DOMAIN="GsPluginModulix"',
    '-DI_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE',
    '-DGETTEXT_PACKAGE="' + gettext_package + '"',
    '-DMODULIX_LOCALEDIR="' + localedir + '"',
  ],
)

# ─── Translations ─────────────────────────────────────────────────────────────

subdir('po')
