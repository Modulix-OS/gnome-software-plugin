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
appstream_dep      = dependency('appstream', version : '>= 1.0')
json_glib_dep      = dependency('json-glib-1.0')
libsoup_dep        = dependency('libsoup-3.0')
xmlb_dep           = dependency('xmlb')  # gnome-software.h → gs-silo-wrapper.h → <xmlb.h>

cargo    = find_program('cargo')
cbindgen = find_program('cbindgen')

# Plugin API version (e.g. plugins-23 for GS 49, plugins-24 for GS 50) is read
# from gnome-software itself so we never hardcode it. Override with -Dplugindir
# for a local dev install under ~/.local.
gs_plugindir   = gnome_software_dep.get_variable(pkgconfig : 'plugindir')
plugins_subdir = fs.name(gs_plugindir)

plugindir = get_option('plugindir')
if plugindir == ''
  plugindir = get_option('libdir') / 'gnome-software' / plugins_subdir
endif

# ─── Rust backend: static lib + cbindgen header (one target, two outputs) ─────

backend_src      = meson.current_source_dir() / 'backend'
cargo_target_dir = meson.current_build_dir() / 'cargo-target'

backend = custom_target(
  'backend',
  output             : ['libbackend.a', 'backend.h'],
  console            : true,
  build_by_default   : true,
  build_always_stale : true,
  command : [
    'sh', '-c',
    'set -e; "@0@" build --release --manifest-path "@1@/Cargo.toml" --target-dir "@2@"; cp "@2@/release/libbackend.a" "@OUTPUT0@"; "@3@" --quiet --config "@1@/cbindgen.toml" --crate backend --output "@OUTPUT1@" "@1@"'.format(
      cargo.full_path(), backend_src, cargo_target_dir, cbindgen.full_path()),
  ],
)
backend_header = backend[1]

backend_dep = declare_dependency(
  link_args : [
    '-L' + meson.current_build_dir(),
    '-lbackend',
    '-lpthread', '-ldl', '-lm',
  ],
)

# ─── Plugin ───────────────────────────────────────────────────────────────────

plugin_sources = files(
  'plugin/src/gs-plugin-modulix.c',
  'plugin/src/gs-modulix-app.c',
  'plugin/src/gs-modulix-enrichment-cache.c',
  'plugin/src/gs-modulix-icon-cache.c',
  'plugin/src/gs-modulix-json-utils.c',
  'plugin/src/gs-modulix-markup.c',
)

modulix_resources = gnome.compile_resources(
  'modulix-resources',
  'data/modulix.gresource.xml',
  source_dir : 'data',
  c_name     : 'modulix',
)

shared_module(
  'gs_plugin_modulix',
  sources : [plugin_sources, backend_header, modulix_resources],
  dependencies : [
    gnome_software_dep,
    glib_dep,
    gobject_dep,
    gio_dep,
    gio_unix_dep,
    gtk4_dep,
    appstream_dep,
    json_glib_dep,
    libsoup_dep,
    xmlb_dep,
    backend_dep,
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
