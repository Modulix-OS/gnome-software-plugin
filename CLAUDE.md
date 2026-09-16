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

just build                  # meson + ninja; meson drives cargo + cbindgen for modulix-store-client
just install-dev            # install the .so under ~/.local/lib/gnome-software/plugins-<api>/
just run-gs                 # launch gnome-software (bubblewrap) with the local plugin
just run-gs-log             # same, filtered logs
just check-plugin           # assert gs_plugin_query_type is exported
just test | lint | fmt      # modulix-store-client crate (sibling repo): cargo test / clippy -D warnings / fmt
nix build .#default         # gnome-software bundled WITH the plugin (single package)
nix build .#plugin          # just the plugin .so derivation
```

The plugin API version (`plugins-23` for GS 49, `plugins-24` for GS 50, …) is
never hardcoded: meson reads it from `pkg-config --variable=plugindir
gnome-software` (`fs.name`), and the Justfile/flake derive the install dir the
same way.

## Architecture — thin D-Bus client

The daemon (`../modulix-daemon`) is now the sole source of truth for both
reads and writes — it owns `modulix-core-utils`, the nix package index, and
every cache. This repo no longer links `modulix-core-utils` at all; it only
talks to two D-Bus interfaces served by `mx-daemon` (`org.modulix.Daemon`,
system bus, both at `/org/modulix/Daemon`) through a small Rust+C shim crate,
`modulix-store-client` (sibling repo `../modulix-store-client`):

```
GNOME Software (user process)
  libgs_plugin_modulix.so   (plugin/src/gs-plugin-modulix.c, GsPlugin subclass, async vfuncs)
   └── mx_store_* (modulix-store-client.h, C shim over zbus)
        ├── READ  → org.modulix.Store1  → mx-daemon (unprivileged)
        └── WRITE → org.modulix.Daemon  → mx-daemon (polkit-gated)
```

- **READ** (search / list installed / module plugins / enrichment / alternate
  sources) calls `org.modulix.Store1`, typed `a{sv}` on the wire. The shim
  reshapes each reply into the same JSON string shape this repo's C code has
  always parsed (json-glib) — JSON is now a shim-internal detail, not the
  inter-process contract. Free every `mx_store_*` string with
  `mx_store_free_string`.
- **WRITE** (install / uninstall) calls `org.modulix.Daemon` directly through
  the shim — no more hand-rolled `GDBusConnection` call in this repo
  (`daemon_call()` is gone). polkit authorization happens daemon-side.
- The shim's blocking zbus API needs no Tokio runtime in the GNOME Software
  process; `mx_store_init()` opens one system-bus connection, reused by every
  `mx_store_*` call for the plugin's lifetime.

### Components

| Path | Role |
|---|---|
| `plugin/src/gs-plugin-modulix.c` | The C plugin: vfuncs `setup`/`list_apps`/`refine`/`install_apps`/`uninstall_apps`, `gs_plugin_query_type`. |
| `../modulix-store-client/` | Sibling repo: Rust staticlib (`crate-type = ["staticlib","rlib"]`) wrapping zbus proxies to `org.modulix.{Store1,Daemon}` behind `extern "C"` (`mx_store_*`). No dependency on `modulix-core-utils`. |
| `../modulix-store-client/cbindgen.toml` | Generates `modulix-store-client.h` (the FFI header) at build time. |
| `meson.build` | One `custom_target` runs cargo (→ `libmodulix_store_client.a`) **and** cbindgen (→ `modulix-store-client.h`) against the sibling checkout; the plugin links `-lmodulix_store_client`. |
| `flake.nix` | `packages.plugin`, `packages.default` (= gnome-software + plugin), dev shell + `gnome-software-dev` bubblewrap runner. Vendors `../modulix-store-client` via a flake input for the Nix package build, same pattern `../modulix-core-utils` used to be vendored under. |

### GsApp mapping & dedup (the non-obvious part)

The plugin must make a nix/module entry **deduplicate** with the Flatpak of the
same app (one "Firefox" row) while still offering every source.

- Each GsApp's **id is the canonical AppStream id** (`gs_app_new(app_id)`), so
  the loader's id-keyed `gs_app_list_filter_duplicates` merges it with the
  Flatpak. The nix attribute is kept separately in `g_object_set_data
  "modulix::name"` (used for install). `bundle_kind = AS_BUNDLE_KIND_PACKAGE` +
  an icon are **mandatory** or the loader drops the app.
- The `"<app_name> (<pname>)"` display-name label (`gs-modulix-app.c`,
  `gs_modulix_make_app_from_json`) only appears when the caller passes
  `label_variant = TRUE` — the installed list and the `alternate_of` (Sources
  popover) paths. The search-results path passes `FALSE`: `dedup_by_group`
  (`modulix-daemon/src/store/entry.rs`) has already merged the variants there, so the
  parenthesized pname would be redundant noise next to the Flatpak row (e.g.
  `Firefox (firefox-bin)` instead of a bare `Firefox`).
- **Which entry wins the dedup** is decided by `gs_app_get_priority()`
  (`lib/gs-app.c`), which comes from the fixed point over
  `GS_PLUGIN_RULE_BETTER_THAN` edges (`gs-plugin-loader.c`) — **not** from
  plugin registration order. `gs_plugin_modulix_init()` declares
  `BETTER_THAN flatpak` and `BETTER_THAN packagekit` so a module/nix entry
  always outranks the Flatpak of the same app-id. There is still no public
  per-app priority setter (`gs_app_set_priority` is in the unexported
  `gs-app-private.h`), so this is a single plugin-wide priority: a bare nix
  package with no associated module also outranks the Flatpak, and
  `modulix::flatpak_preferred` (computed in `modulix-daemon/src/store/entry.rs`,
  written to GObject data in `gs-modulix-app.c`) is currently **inert** — no
  per-app exception is expressible.
- **Module beats nix package when both are ours** via emission order:
  `list_apps_thread` (`gs-plugin-modulix.c`) appends modules before packages
  in both the `installed` and `search` modes, sharing a `seen_ids`
  `GHashTable` passed to `gs_modulix_append_apps_from_json()` — an id already
  seen is skipped, so the module wins intra-plugin dedup explicitly instead of
  relying on `gs_app_list_filter_duplicates`'s first-wins semantics. The
  `alternate_of` (Sources popover) path passes `seen_ids = NULL`: every entry
  there intentionally shares one GsApp id and must NOT be deduped.
- **Ordering of the "Sources" popover** is enforced via a **patched
  gnome-software** (`nix/gs-details-page-sortkey.patch`, applied in `flake.nix`
  to `gnome-software-patched`, used by `gnome-software-modulix` and the
  `gnome-software-dev` runner): `sort_by_packaging_format_preference` in
  `src/gs-details-page.c` reads a `GnomeSoftware::SortKey` metadata value
  *before* the user's `packaging-format-preference` GSetting, so module > nix
  variant ordering is fixed regardless of user settings (Flatpak has no such
  metadata and falls back to the patch's default of 1000, landing it between a
  module and any nix variant). The daemon emits neutral `kind`/`variant_rank`
  fields only (see `modulix-daemon/src/store/entry.rs`); `gs-modulix-app.c`
  turns them into the actual number (`MODULIX_MODULE_SORT_KEY = 50`; nix
  variants get `MODULIX_PACKAGE_SORT_BASE (2000) + variant_rank`, e.g. `-bin` <
  base < `-beta` < … < `-unwrapped` < unknown suffix) and writes it via
  `gs_app_set_metadata(app, "GnomeSoftware::SortKey", …)`. This governs the
  version-selector order only — it is **not** dedup priority (dedup priority
  is the `BETTER_THAN` + emission-order scheme described above).
- The Sources dropdown is filled by a separate **`alternate-of`** query:
  `list_apps_async` checks `gs_app_query_get_alternate_of` and returns
  (`mx_store_packages_for_app_id`, → `Store1.PackagesForAppId`) — a Modulix
  module targeting the app-id first (best-effort, via
  `modulix-core-utils::module_info::modules_for_app_id` on the daemon side,
  bounded by a 500ms timeout), then the curated/live nix variants with the
  same id. No `nix eval` runs on this path any more (see Gotchas).
- `modulix::kind` metadata (`package` | `module` | `plugin`) routes refine and
  install. A module's plugins are attached as **ADDON** addons
  (`gs_app_add_addon`); they carry `modulix::parent` + `modulix::plugin_name`.
- **Ranking within the search-results page** is a third, separate lever:
  `GsApp::match-value` (`gs_app_set_match_value()`), read by
  `gs-search-page.c`'s sort key (`kind : state : match_value : rating :
  kudos`). Distinct from `SortKey` (Sources popover only) and from
  `BETTER_THAN` (dedup only). `NixPackage::search_scored` /
  `ModuleInfo::search_scored` (in `modulix-core-utils`, daemon-side) return
  the raw relevance `score()` alongside each item -- `ModuleInfo::search_scored`
  additionally filters `score == 0` so unrelated modules stop appearing on
  every query. The daemon emits that raw `score` as-is (see
  `modulix-daemon/src/store/entry.rs`); `gs-modulix-app.c`'s
  `modulix_match_value(score, is_module)` rescales it client-side: nix
  packages land in `1..=0x7F`, the same range `gs_appstream_add_search_hit`
  uses for AppStream hits, so nix and Flatpak results interleave by real
  relevance; modules get `+0x80` on top so a relevant module always precedes
  its own Flatpak. `score` is absent from the `a{sv}` entry on the
  installed-listing and `alternate_of` paths, where relevance sort doesn't
  apply -- `gs-modulix-app.c` only calls `gs_app_set_match_value()` when the
  `score` key is present.

`list_apps_async` modes: `alternate_of` set → variants; `is_installed == TRUE` →
installed packages + modules; keywords → search packages + modules.

### Icons

`gs_app_get_icon_for_size()` (`lib/gs-app.c`) resolves in **two passes**: a
first pass over sized icons (`GFileIcon`/`GsRemoteIcon`) picking the smallest
one ≥ the requested size, and — only if that finds nothing — a second pass
over *unsized* `GThemedIcon`s, returning the first one `gtk_icon_theme_has_gicon()`
knows. A themed icon therefore always **loses** to a sized file icon whose
file exists, even a wrong or not-yet-downloaded one. To let the installed
icon theme (Papirus, `Modulix-OS`) win, an app must carry **only** a themed
icon — never both.

`plugin/src/gs-modulix-icon-resolver.c` is the single place that decides
this, in priority order: (1) `themed(icon_name) → themed(app_id) →
themed(name) → themed(base_name)`, whichever names the current theme actually
has — bundled into one `GThemedIcon` via `g_themed_icon_append_name()` so
GTK's own fallback search does the priority ordering, with
`application-x-executable` always appended last so the pass-2 lookup can never
come back empty. `name` is the raw nix attribute; `base_name` is that
attribute with any known variant/edition suffix stripped
(`ICON_BASE_SUFFIXES` in `modulix-daemon/src/store/entry.rs`, e.g. `davinci-resolve-studio`
→ `davinci-resolve`) — a **themed-icon-only** fallback, never used for
identity (`app_id`/`app_name`/`group_id` are untouched, so two editions of a
program still show as distinct rows); skipped when it equals `name` (nothing
was stripped) to avoid stacking the same candidate twice; (2) the
local flatpak-appstream icon cache
(`{/var/lib/flatpak,$XDG_DATA_HOME/flatpak}/appstream/*/*/active/icons/{128x128,64x64}`);
(3) the local gnome-software remote-icon cache
(`~/.cache/gnome-software/icons/<sha1(uri)>-<basename>`), indexed by app-id so
an icon the **flatpak** plugin already downloaded is reused even when its URL
differs from ours; (4) the remote Flathub URL (`gs_remote_icon_new`, 128×128
— the asset's real size, and the icon downloader's own cap); (5) a generic
`application-x-executable` fallback, so an app is never left iconless. The
resolver always returns exactly **one** `GIcon` — handing back several would
let the wrong level win the two-pass rule above. Three indexes back it,
built once by `gs_modulix_icon_resolver_init()` (called from `setup_async`,
main thread only — `GtkIconTheme` isn't thread-safe — after the plugin's own
`gtk_icon_theme_set_search_path()`) and read through a `GRWLock` from the
list/refine worker threads: the theme's icon-name set (re-snapshotted on
`GtkIconTheme::changed`), and the two file-cache directory scans above.

`icon_name` (from `meta.mainProgram`, e.g. `vscode` → `code`) is a field
alongside `icon` on every JSON app entry the daemon emits
(`AppEntry`/`EnrichEntry` in `modulix-daemon/src/store/entry.rs`), sourced from
`AppInfoGui::icon_name()` (see Rust read API below). The plugin stores it as
`g_object_data` (`modulix::icon_name`, alongside the pre-existing
`modulix::app_id`/`modulix::base_name`/`modulix::icon`) so a later ICON-only
refine can replay the resolver without re-fetching anything.

`NixPackage::icon()` is unconditionally `None` — `flathub_basic_info.rs` no
longer carries an icon URL at all (see Gotchas). Level 4 above is therefore
reachable only via `ModuleInfo::icon()` (module metadata) and refine's live
Flathub enrichment (`FlatpakInfo::icon()`, `fetch_enrichment_json`); a nix
package on the list/search path relies on levels 1-3 only.

`gs_plugin_modulix_init()` declares `RUN_BEFORE "icons"`: a `GsRemoteIcon`
added mid-refine (DESCRIPTION enrichment, or the resolver's own level 4) must
still be picked up by the `icons` plugin's download queue in the same job.
Once a themed icon has won (`modulix::icon-themed` object data set), refine's
Flathub-enrichment step deliberately skips adding its remote icon on top —
otherwise the download completing later would flip the first pass back on
and silently replace the themed icon.

## Contracts (must match the real services)

### D-Bus — `org.modulix.Daemon`, `org.modulix.Store1` (system bus), in `../modulix-daemon`

Both interfaces are served at the same object path (`/org/modulix/Daemon`) by
`mx-daemon`. This repo never talks to either directly — always through
`modulix-store-client`'s `mx_store_*` shim.

| Interface | Method | Sig | Returns |
|---|---|---|---|
| `Store1` (read, unprivileged) | `SearchPackages` / `SearchModules` | `su` (query, max) | `aa{sv}` |
| `Store1` | `ListInstalledPackages` / `ListInstalledModules` | — | `aa{sv}` |
| `Store1` | `ListModulePlugins` | `s` (module) | `aa{sv}` |
| `Store1` | `GetAppEnrichment` | `as` (app_ids) | `a{sa{sv}}` |
| `Store1` | `PackagesForAppId` | `s` (app_id) | `aa{sv}` |
| `Store1` | property `IndexReady` | — | `b` |
| `Daemon` (write, polkit-gated) | `InstallPackage` / `UninstallPackage` | `as` | `s` (status text) |
| `Daemon` | `InstallModule` / `UninstallModule` | `as` | `s` |
| `Daemon` | `InstallPlugin` / `UninstallPlugin` | `ss` (module, plugin) | `s` |

Success = a D-Bus reply with no error. Packages/modules are batched into one
`as` call each; plugins are called individually. `a{sv}` entry field names
(`name`, `base_name`, `pname`, `app_name?`, `summary`, `version`, `app_id?`,
`group_id?`, `icon?`, `icon_name?`, `kind`, `flatpak_preferred`,
`variant_rank`, `score?`) are documented in `modulix-daemon/src/store/entry.rs`
and re-serialized by the shim to the same-named JSON keys this repo's C code
parses — see `plugin/src/gs-modulix-app.c`'s `gs_modulix_make_app_from_json`
for exactly which keys it reads.

### `modulix-store-client` C ABI — `../modulix-store-client/cbindgen.toml`

`mx_store_init`/`mx_store_shutdown`/`mx_store_free_string`; reads
`mx_store_{search_packages,search_modules,list_installed_packages,
list_installed_modules,list_module_plugins,get_app_enrichment,
get_app_enrichment_many,packages_for_app_id}`; writes
`mx_store_{install,uninstall}_{packages,modules}` (name array + count) and
`mx_store_{install,uninstall}_plugin` (module, plugin). Every call returns
`NULL` on failure (connection error, D-Bus error, denied polkit
authorization) — this repo's code never sees *why* a write failed, only
whether it did (see `lifecycle_execute` in `gs-plugin-modulix.c`).

## Gotchas / current limitations

- **modulix-store-client sibling checkout.** Local `cargo`/`just build` use
  `../modulix-store-client` directly (its uncommitted changes count). The Nix
  package build vendors it via the `modulix-store-client` flake input
  (`git+file://…`, also picking up tracked uncommitted edits); switch that
  input to the GitHub URL once pushed.
- Sort-key suffix ranking and `FLATPAK_PREFERRED_APP_IDS` are curated (on the
  daemon side, `modulix-core-utils::package_info`) — tune against the real
  flatpak/packagekit app priorities with d-spy/bustle.
- `Store1.PackagesForAppId` no longer runs `nix eval` (it used to, to expand
  multi-output packages — that expansion was removed along with the now-dead
  `expand_outputs`/`fetch_outputs`/`expand_with_outputs`). It is table
  lookups + one bounded (500ms) module-index lookup + an optional `nix
  search` for pname-only groups, cached per app-id for 5 minutes daemon-side.
- Installed **module plugins** are listed as available addons; per-plugin
  installed state (reading `mx.<name>.plugins`) is not wired yet.
- `flathub_basic_info.rs` (the generated `NIX_INFO` table, in
  `modulix-core-utils`) must be regenerated after touching `icon_name`/
  `keywords` fields or the Flathub-matching logic: `cargo run --release
  --features flathub-info-gen --bin flathub-info-gen` from
  `modulix-core-utils` (nix eval over all of nixpkgs + ~1600 Flathub API
  calls — slow, network-bound). `NixInfo` has no `icon` field — the
  URL-based level 4 icon path for nix packages is gone (see "Icons"). The
  daemon build includes this generated file unconditionally, so a stale
  table without a field the code expects fails the whole daemon build, not
  just tests — this repo is unaffected either way since it no longer builds
  `modulix-core-utils` at all.
- `mx_store_*` write calls collapse every failure mode (bus down, D-Bus
  error, denied polkit prompt, daemon-side transaction failure) to `NULL`;
  `lifecycle_execute` reports a single generic `GS_PLUGIN_ERROR_FAILED` for
  the whole batch rather than a per-app reason.
