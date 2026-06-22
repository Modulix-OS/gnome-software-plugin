# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A **GNOME Software plugin** (gnome-software 49/50, GObject `GsPlugin` subclass) that
lets Modulix-OS users browse and install **nix packages**, **Modulix modules**
(meta-packages) and **module plugins** from the Software app.

Everything builds inside the Nix dev shell — the host has no gnome-software
headers, meson, ninja or cbindgen. Always `nix develop` first.

## Commands

```bash
nix develop                 # enter dev shell (gnome-software, gtk4, cargo, meson, cbindgen, d-spy…)

just build                  # meson + ninja; meson drives cargo + cbindgen for the Rust backend
just install-dev            # install the .so under ~/.local/lib/gnome-software/plugins-<api>/
just run-gs                 # launch gnome-software (bubblewrap) with the local plugin
just run-gs-log             # same, filtered logs
just check-plugin           # assert gs_plugin_query_type is exported
just test | lint | fmt      # backend crate: cargo test / clippy -D warnings / fmt
nix build .#default         # gnome-software bundled WITH the plugin (single package)
nix build .#plugin          # just the plugin .so derivation
```

The plugin API version (`plugins-23` for GS 49, `plugins-24` for GS 50, …) is
never hardcoded: meson reads it from `pkg-config --variable=plugindir
gnome-software` (`fs.name`), and the Justfile/flake derive the install dir the
same way.

## Architecture — two channels

```
GNOME Software (user process)
  libgs_plugin_modulix.so   (plugin/src/gs-plugin-modulix.c, GsPlugin subclass, async vfuncs)
   ├── READ  → FFI (cbindgen) → backend/ (Rust staticlib) → modulix-core-utils
   └── WRITE → GDBus (system bus) → org.modulix.Daemon (root daemon)
```

- **READ** (search / list installed / metadata / module plugins) is in-process
  FFI. Data crosses as **JSON strings** owned by the caller; free with
  `backend_free_string`. The Rust side runs a Tokio runtime because
  modulix-core-utils is async.
- **WRITE** (install / uninstall) is **never** done in Rust here — the plugin
  calls the daemon over D-Bus. See contracts below.

### Components

| Path | Role |
|---|---|
| `plugin/src/gs-plugin-modulix.c` | The C plugin: vfuncs `setup`/`list_apps`/`refine`/`install_apps`/`uninstall_apps`, `gs_plugin_query_type`. |
| `backend/` | Rust staticlib (`crate-type = ["staticlib","rlib"]`) wrapping `modulix-core-utils` behind `extern "C"`. Depends on it by **path** (`../../modulix-core-utils`). |
| `backend/cbindgen.toml` | Generates `backend.h` (the FFI header) at build time. |
| `meson.build` | One `custom_target` runs cargo (→ `libbackend.a`) **and** cbindgen (→ `backend.h`); the plugin links `-lbackend`. |
| `flake.nix` | `packages.plugin`, `packages.default` (= gnome-software + plugin), dev shell + `gnome-software-dev` bubblewrap runner. |

### GsApp mapping & dedup (the non-obvious part)

The plugin must make a nix/module entry **deduplicate** with the Flatpak of the
same app (one "Firefox" row) while still offering every source.

- Each GsApp's **id is the canonical AppStream id** (`gs_app_new(app_id)`), so
  the loader's id-keyed `gs_app_list_filter_duplicates` merges it with the
  Flatpak. The nix attribute is kept separately in `g_object_set_data
  "modulix::name"` (used for install). `bundle_kind = AS_BUNDLE_KIND_PACKAGE` +
  an icon are **mandatory** or the loader drops the app.
- **Priority** is intended to pick the default source and order the "Sources"
  list: module (1000) > nix (500) > flatpak; apps in
  `package_info::FLATPAK_PREFERRED_APP_IDS` drop the nix package to 100 (below
  flatpak, still below a module). The backend computes this value and the plugin
  stashes it in `g_object_set_data "modulix::priority"`. **Caveat:** GNOME
  Software 50 exposes **no public per-app priority setter** (`gs_app_set_priority`
  is loader-internal; dedup priority is stamped from plugin order). So the
  cross-source *default* is currently governed by plugin ordering, not this
  number — wire real ordering via plugin rules, or patch gnome-software, if the
  exact module>nix>flatpak default must be enforced.
- The Sources dropdown is filled by a separate **`alternate-of`** query:
  `list_apps_async` checks `gs_app_query_get_alternate_of` and returns our nix
  variants (`backend_packages_for_app_id`) with the same id.
- `modulix::kind` metadata (`package` | `module` | `plugin`) routes refine and
  install. A module's plugins are attached as **ADDON** addons
  (`gs_app_add_addon`); they carry `modulix::parent` + `modulix::plugin_name`.

`list_apps_async` modes: `alternate_of` set → variants; `is_installed == TRUE` →
installed packages + modules; keywords → search packages + modules.

## Contracts (must match the real services)

### D-Bus — `org.modulix.Daemon` (system bus, root), in `../modulix-daemon`

| Method | Sig | Returns |
|---|---|---|
| `InstallPackage` / `UninstallPackage` | `as` | `s` (status text) |
| `InstallModule` / `UninstallModule` | `as` | `s` |
| `InstallPlugin` / `UninstallPlugin` | `ss` (module, plugin) | `s` |

Success = a D-Bus reply with no error. Packages/modules are batched into one
`as` call each; plugins are called individually.

### Rust read API — `modulix-core-utils` (in `../modulix-core-utils`)

The backend uses: `NixPackage::search` / `::new` / getters incl. the added
`version()`; `install_package::list_installed_package(CONFIG_DIRECTORY)`;
`ModuleInfo::search` / `::new` / `resolve` / `list_plugins`; the added
`install_module::list_installed_modules`; and `package_info::{is_flatpak_preferred,
packages_for_app_id}` plus `AppInfoGui::{id, icon}` for the canonical app-id and
icon. `CONFIG_DIRECTORY` (= `/etc/modulix-os/` in release) is the config path,
so the installed-listing FFI takes no directory argument.

## Gotchas / current limitations

- **modulix-core-utils path dependency.** Local `cargo`/`just build` use the
  sibling checkout directly (its uncommitted changes count). The Nix package
  build pulls it via the `modulix-core-utils` flake input (`git+file://…`, also
  picking up tracked uncommitted edits); switch that input to the GitHub URL
  once the additions are pushed.
- **Daemon side (in `../modulix-daemon`) must be finished for installs to work
  end-to-end**: install logic is currently stubbed (prints only); the system
  D-Bus policy denies non-root callers (the user-context plugin will get
  `AccessDenied`); there are no progress signals (install shows an
  indeterminate state). The plugin still builds and emits the correct calls.
- Priority constants and `FLATPAK_PREFERRED_APP_IDS` are curated — tune against
  the real flatpak/packagekit app priorities with d-spy/bustle.
- Installed **module plugins** are listed as available addons; per-plugin
  installed state (reading `mx.<name>.plugins`) is not wired yet.
