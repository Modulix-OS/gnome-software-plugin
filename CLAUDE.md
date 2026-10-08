# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A **GNOME Software plugin** (gnome-software 49/50, GObject `GsPlugin` subclass) that
lets Modulix-OS users browse and install **nix packages**, **Modulix modules**
(meta-packages) and **module plugins** from the Software app.

Everything builds inside the Nix dev shell — the host has no gnome-software
headers, meson or ninja. Always `nix develop` first. Pure C: no Rust
toolchain involved anywhere in this repo.

## Commands

```bash
nix develop                 # enter dev shell (gnome-software, gtk4, meson, d-spy…)

just build                  # meson + ninja
just install-dev            # install the .so under ~/.local/lib/gnome-software/plugins-<api>/
just run-gs                 # launch gnome-software (bubblewrap) with the local plugin
just run-gs-log             # same, filtered logs
just check-plugin           # assert gs_plugin_query_type is exported
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
every cache. This repo no longer links `modulix-core-utils` at all, and no
longer links any Rust code either: it talks directly, in plain C, to two
D-Bus interfaces served by `mx-daemon` (`org.modulix.Daemon`, system bus,
both at `/org/modulix/Daemon`), via `plugin/src/dbus/`:

```
GNOME Software (user process)
  libgs_plugin_modulix.so   (GsPlugin subclass: gs-plugin-modulix.c = GObject shell,
                             gs-modulix-{list,refine,lifecycle}.c = async vfunc bodies)
   └── plugin/src/dbus/ (GDBus/GVariant, no JSON, no Rust)
        ├── gs-modulix-bus.c     one system-bus GDBusConnection, memoized
        ├── gs-modulix-store1.c  READ  → org.modulix.Store1  → mx-daemon (unprivileged)
        └── gs-modulix-daemon1.c WRITE → org.modulix.Daemon  → mx-daemon (polkit-gated)
```

- **READ** (search / list installed / module plugins / enrichment / alternate
  sources) calls `org.modulix.Store1`, typed `a{sv}`/`aa{sv}`/`a{sa{sv}}`/
  `a{ss}` on the wire — `gs-modulix-store1.c` hands back the reply's
  `GVariant` payload unwrapped from its tuple, still typed, never
  re-serialized to text. Consumers (`gs-modulix-app.c` and friends) read
  fields with `g_variant_lookup(dict, key, "&s"/"b"/"i"/"u", &out)`, which
  leaves `out` at its default when the key is absent or the wrong type — the
  same total-accessor behaviour `gs-modulix-json-utils.c` used to provide,
  now built into `GVariant` itself. A failed read is logged
  (`g_warning`) inside `gs-modulix-store1.c` and returned as `NULL`; callers
  treat `NULL` exactly like an empty result.
- **WRITE** (install / uninstall) calls `org.modulix.Daemon` directly through
  `gs-modulix-daemon1.c` — no shim, no hand-rolled `GDBusConnection` call
  duplicated per call site. polkit authorization happens daemon-side; unlike
  the old shim (which collapsed every failure to an undifferentiated `NULL`),
  a failure now carries a real `GError` (D-Bus error name + message), logged
  by `gs-modulix-lifecycle.c`'s `call_names()`/`call_plugin()`.
- `gs_modulix_bus_init()` (`setup_async`) opens one system-bus
  `GDBusConnection`, reused by every `gs_modulix_bus_call()` for the
  plugin's lifetime; `GDBusConnection` is thread-safe for method calls, so
  the list/refine/lifecycle worker threads all share it directly, no Tokio
  runtime or blocking-bridge needed in the GNOME Software process.

### Components

| Path | Role |
|---|---|
| `plugin/src/gs-plugin-modulix.c` | GObject shell only: type definition, `setup_async` (locale, icon theme, resolver, `gs_modulix_bus_init`), vfunc plumbing, `gs_plugin_query_type`. |
| `plugin/src/gs-modulix-list.c` | `list_apps` body: the four query shapes (search / installed / `alternate_of` / `is_for_update`), one `collect_into()` call per daemon read (the `is_for_update` shape instead delegates to `gs-modulix-update.c`). |
| `plugin/src/gs-modulix-refine.c` | `refine` body: batched enrichment + license prefetches, then the per-app steps (addons, icon, description seed, license, Flathub enrichment). |
| `plugin/src/gs-modulix-lifecycle.c` | `install_apps`/`uninstall_apps`: the coalescing queue (`GsModulixLifecycle`), its single drain worker, and the `org.modulix.Daemon` write calls (`gs-modulix-daemon1.c`). |
| `plugin/src/gs-modulix-update.c` | `refresh_metadata`/`update_apps` body: the synthetic "Modulix OS" `GsApp` (`GS_APP_SPECIAL_KIND_OS_UPDATE`), checked via `Store1.CheckUpdate`, described from `Store1.ListOutdatedInputs`, applied via `Daemon.UpdateSystem`, and after an interactive `"switch"` finished off with `Store1.RebootRequired`. See "System updates" below. |
| `plugin/src/gs-modulix-icon-theme.c` | One-shot `GtkIconTheme` search-path setup (NixOS profile dirs + the plugin's resource icons), run before the resolver indexes the theme. |
| `plugin/src/gs-modulix-config.h` | `MODULIX_ENABLE_PACKAGES`/`MODULIX_ENABLE_MODULES` feature flags + `MODULIX_SEARCH_LIMIT`, shared by the list and lifecycle sides. |
| `plugin/src/dbus/gs-modulix-bus.{c,h}` | The single memoized system-bus `GDBusConnection` (`gs_modulix_bus_init`/`_shutdown`) and the generic synchronous call helper (`gs_modulix_bus_call`) every wrapper below is built on. |
| `plugin/src/dbus/gs-modulix-store1.{c,h}` | One wrapper per `org.modulix.Store1` read method, each returning the reply's `GVariant` payload (`aa{sv}`/`a{sa{sv}}`/`a{ss}`) unwrapped from its tuple, or `NULL` (logged) on failure. 30s timeout. |
| `plugin/src/dbus/gs-modulix-daemon1.{c,h}` | `org.modulix.Daemon` write wrappers (`gs_modulix_daemon1_names_call`/`_plugin_call`), returning the daemon's status string or `NULL` with a `GError`. No timeout (`G_MAXINT`) — a rebuild can take minutes. |
| `flake.nix` | `packages.plugin`, `packages.default` (= gnome-software + plugin), dev shell + `gnome-software-dev` bubblewrap runner. No Rust toolchain, no vendored sibling checkout — the plugin is pure C against `gnome-software`/`glib`/`gtk4`/`appstream`/`libsoup`/`xmlb`. |

### GsApp mapping & dedup (the non-obvious part)

The plugin must make a nix/module entry **deduplicate** with the Flatpak of the
same app (one "Firefox" row) while still offering every source.

- Each GsApp's **id is the canonical AppStream id** (`gs_app_new(app_id)`), so
  the loader's id-keyed `gs_app_list_filter_duplicates` merges it with the
  Flatpak. The nix attribute is kept separately in `g_object_set_data
  "modulix::name"` (used for install). `bundle_kind = AS_BUNDLE_KIND_PACKAGE` +
  an icon are **mandatory** or the loader drops the app.
- The display name is plain `app_name` → `pname` → `name`
  (`gs_modulix_make_app_from_json`, `gs-modulix-app.c`). The former
  `"<app_name> (<pname>)"` variant label (and its `label_variant` argument) is
  gone: a GsApp is now shared across queries through the plugin cache, so the
  label could no longer depend on which path listed it, and the Sources
  popover distinguishes variants by its own origin/`SortKey` ordering anyway.
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
- **Installed state** comes from the daemon, per entry: every `a{sv}` row
  carries an `installed` bool (`AppEntry` in `modulix-daemon/src/store/entry.rs`),
  stamped *after* every `FlightCache` read so a 60s-old `SearchPackages` or a
  5min-old `PackagesForAppId` never serves a pre-install answer, and
  invalidated outright by `store::invalidate_installed()` when a write goes
  through `org.modulix.Daemon`. `gs_modulix_make_app_from_json` reads it; the
  `is_installed` argument is only the fallback for a daemon predating the
  field. Before this, only the installed *listing* said `TRUE`, so search
  results and Sources-popover rows were born `AVAILABLE` whatever the system
  had. The flag also elects each group's representative in `dedup_by_group`
  (an installed `firefox-bin` outranks an available `firefox`), which is why
  the daemon stamps it *before* deduplicating.
- **One `GsApp` per (kind, name), via `gs_plugin_cache_{lookup,add}`**
  (`gs-modulix-app.c`) — the pattern flatpak/packagekit/epiphany use. The key
  is `"<kind>\x1f<name>"`, **not** the GsApp id, which the `alternate_of` path
  deliberately shares across rows. Reuse is what makes state survive a query:
  `gs_details_page_get_alternates_cb` (`src/gs-details-page.c`) calls
  `_set_app()` with the matching row of the `alternate-of` reply, re-issued on
  every page reload — a fresh instance would land `AVAILABLE` and flip the
  button back to "Install" the instant an install finished. Consequences for
  anything written onto a `GsApp`: it must be idempotent or guarded, since the
  same object is re-listed and re-refined. Icons (`gs_app_has_icons`),
  screenshots (`gs_app_get_screenshots`) and module addons
  (`modulix::addons-added` object data) are guarded; `gs_app_set_state` skips
  the transient states owned by `enqueue_lifecycle` (`gs-modulix-lifecycle.c`) through
  its `state_is_transient` guard (`gs-modulix-app.c`);
  `gs_app_set_metadata` is write-once (`lib/gs-app.c` warns and returns on a
  second write of the same key), and `gs_app_set_license` only accepts a
  *strictly higher* quality, so the first caller wins in both. `gs_app_set_name`
  is **not** in that group — it gates on `quality < name_quality`, so an
  equal-quality rewrite does go through.
- **Module beats nix package when both are ours** via emission order:
  `list_apps_thread` (`gs-modulix-list.c`) appends modules before packages
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

### Licenses

The details page's License row comes from `gs_app_set_license()`, which only
accepts a write of **strictly higher** `GsAppQuality` than the one already
stored (`lib/gs-app.c`) — that arbitration is what orders our two sources:

- **nixpkgs `meta.license`** (`GS_APP_QUALITY_HIGHEST`) — the license of the
  derivation the user actually gets. Costs one `nix eval` per attribute
  daemon-side (`Store1.GetPackageLicenses` → `package_info::license_for_package`),
  so the plugin only asks for it when `require_flags` carries **ADDONS**:
  that bit is the details page's own marker (`GS_DETAILS_PAGE_REFINE_REQUIRE_FLAGS`
  in `src/gs-details-page.c`), and the search page — which also requests
  `LICENSE`, over its whole result list — never sets it. `prefetch_licenses()`
  batches the whole list into one call whose result lives in `TaskData.licenses`
  for that job only (the daemon owns the real cache).
- **Flathub `project_license`** (`GS_APP_QUALITY_NORMAL`) — rides along in the
  enrichment payload already fetched for DESCRIPTION/SCREENSHOTS, so it is
  free on every path, including search. Used for modules (no nix attribute to
  evaluate) and whenever the `nix eval` yielded nothing.

Because search runs first and details later on the *same* `GsApp`, the
HIGHEST/NORMAL split is what lets the nix license replace the Flathub one when
the user opens the details page, and never the other way round.

`modulix-core-utils::license` (`src/core/license.rs`) normalizes both into the
SPDX expression AppStream understands: a list of licenses joins with `AND`, a
single unfree term collapses the whole expression to `LicenseRef-proprietary`,
and a term with no `spdxId` degrades it to `LicenseRef-free` rather than
emitting a partial `AND` chain.

### System updates

There is no per-app "update" for a nix package or module — installing again
via the lifecycle queue is how a newer version is picked up. The one thing
that *is* an update is the NixOS system itself, backed by
`Store1.CheckUpdate`, `Store1.ListOutdatedInputs` and
`Store1.RebootRequired` (reads) and `Daemon.UpdateSystem` (write; the daemon
serves `"build"`/`"boot"`/`"switch"`/`"apply"`, this plugin asks for the first
three — see the flags table below), implemented entirely in
`plugin/src/gs-modulix-update.c`.

**Modulix is continuous: this row is the only update path there is.** There is
no major release, no distro upgrade, and deliberately no `GsUpgradeBanner` —
`VERSION_ID`/`VERSION_CODENAME` (from `mxpkgs/release.json` through
`mx.branding`) are labels on `/etc/os-release`, not a mechanism. Revisions,
release-numbered or not, all ride the same tracked branch, so arriving at one
is moving the flake inputs: exactly what this row does, automatically. A
previous iteration added a release banner (`gs-modulix-upgrade.c`,
`Store1.GetRemoteRelease`, `modulix-core-utils::release`); all of it was
removed rather than kept in sync, and the plugin now implements none of
`list_distro_upgrades`/`download_upgrade`/`trigger_upgrade` — hence a harmless
`failed to get upgrades: no plugin could handle listing distro upgrades` in
GNOME Software's log. Do not reintroduce it.

**GNOME Software applies an update in two successive jobs, and both reach this
plugin.** `gs-update-monitor.c`'s `get_updates_finished_cb` issues
`update_apps(NO_APPLY)` ("download"), and its completion callback
`download_finished_cb` issues `update_apps(…)` a second time to apply — but only
for apps `_should_auto_update()` still finds in `GS_APP_STATE_UPDATABLE_LIVE`.
Applying from the *first* job therefore ends the sequence: the app leaves that
state and the apply job is never issued. That was this plugin's original bug
(`NO_APPLY → "boot"`): the automatic update rebuilt during the "download" step
and GNOME Software then only posted a notification. The Updates page's manual
Download/Update buttons are the same two-step pair
(`src/gs-updates-section.c:454`, `:496`).

**The two reads are not interchangeable.** `CheckUpdate` is the *search*: the
daemon runs a full `nix flake update --output-lock-file` into a scratch file
(minutes, refetches every input), keeps the resulting `flake.lock` in RAM
(`PENDING_LOCK`, no TTL), and refills its own outdated-inputs cache from a
**local** diff of the old and new lockfiles. `UpdateSystem` then *writes that
exact lockfile* instead of re-resolving — so the revisions installed are the
ones the user was shown, and the network cost is paid once.
`ListOutdatedInputs` is the *display*: cached rows describing that candidate,
free once a check ran. Only `gs_modulix_update_check()` spends the first;
`gs_modulix_update_list()` (every visit to the Updates page) reads the second.

`"build"` mode is the one write that does **not** consume that memoized
lockfile: it reads it with `store::peek_pending_lock()` (daemon side) precisely
so the `"boot"` that follows applies the very revisions it just built. It is
also the one mode that writes nothing at all — no `flake.lock` in
the config repo, no commit, no activation: `update::build_with_lock()`
(`modulix-core-utils`) stages a throwaway copy of the config dir carrying the
candidate lock and runs `nixos-rebuild build` there. Writing the candidate into
the real repo would move the configuration ahead of the running system, and the
next `CheckUpdate` would then report "up to date" while the system is still
behind. `Daemon::run` correspondingly skips its outdated-cache /
pending-lock / package-index invalidations for `"build"`.

That scratch directory lives under `/tmp`, and `mx-daemon` runs as root, so
`modulix-daemon.service` now sets `PrivateTmp = true` (in
`mxpkgs/modulixos/modulix-daemon/default.nix`) — no symlink anyone plants in the
shared `/tmp` is reachable. `PrivateTmp` alone would have broken more than it
fixed: `/tmp/mx-build-queue` and `/tmp/mx-skip-rebuild.lock`
(`modulix-core-utils/src/core/transaction/`) are **cross-process** rendezvous
between the daemon, the `mx` CLI and `mx-init`, and a private `/tmp` would have
hidden them from each other, letting two rebuilds run at once. Both are
therefore re-exposed with `BindPaths`, and pre-created by `systemd.tmpfiles`
rules so the bind sources always exist (hence the added
`after = systemd-tmpfiles-setup.service`). `build_with_lock` still creates its
directory with `create_dir` rather than `create_dir_all`, so the library is safe
to call from a process without `PrivateTmp` too.

- **One synthetic `GsApp`** represents the whole system, built/reused via
  `gs_modulix_update_get_app()` (same plugin-cache "one GsApp per key" rule
  as `gs-modulix-app.c`, key `"update\x1fmodulix-os"`, id
  `"org.modulix.ModulixOS"`). `gs_app_set_special_kind(app,
  GS_APP_SPECIAL_KIND_OS_UPDATE)` is what makes the Updates page render it as
  the system-update row rather than a regular app; `bundle_kind =
  AS_BUNDLE_KIND_PACKAGE` is still required or the loader drops it, same as
  every other Modulix `GsApp`. `special_kind` is set **before**
  `gs_app_set_kind(AS_COMPONENT_KIND_OPERATING_SYSTEM)`, not after: the special
  setter forces the kind to `GENERIC` internally (`lib/gs-app.c`), and
  `OPERATING_SYSTEM → GENERIC` is a rejected transition, which used to log
  `Kind change on org.modulix.ModulixOS … is not OK` once per session. The
  final kind has to be `OPERATING_SYSTEM`, because `gs_app_is_updatable()`
  returns TRUE unconditionally for it — that is what keeps the row through
  `filter_updatable_apps` (`lib/gs-plugin-job-list-apps.c`) while it sits in
  `PENDING_INSTALL`. Object data `modulix::kind = "update"` keeps
  it out of `gs-modulix-lifecycle.c`'s kind-filtered install/uninstall calls
  (which only ever match `"package"`/`"module"`/`"plugin"`).
- **`gs_modulix_update_sync()`** does one `ListOutdatedInputs` read and
  rewrites the app's dynamic fields every call (state — `UPDATABLE_LIVE` vs
  `INSTALLED`, guarded by `gs_modulix_state_is_transient()` like every other
  Modulix app — update-details-text, one line per outdated input, and
  update-version, the ISO date of the most recently modified input). Used by
  `gs_modulix_update_list()` (the `is_for_update` branch of
  `gs-modulix-list.c`'s `list_apps`, `force_refresh = FALSE`).
- **`PENDING_INSTALL` + nothing outdated is left untouched entirely.** That
  state is not transient, yet a finished `"boot"` update owns it *and* the
  details it left behind, while the daemon's outdated cache is empty by then —
  so an unconditional rewrite would both drop the state and blank the row that
  has to offer the restart. A *newly discovered* update does take it over, and
  has to: staying `PENDING_INSTALL` keeps the app out of
  `_should_auto_update()`, stranding the new update behind a reboot that only
  applies the older one.
- **`GS_APP_QUIRK_NEEDS_REBOOT` tracks `PENDING_INSTALL` exactly**, which is why
  every state write in `gs-modulix-update.c` goes through `update_set_state()`
  instead of `gs_app_set_state()`. A stale quirk is not cosmetic:
  `_get_app_section()` (`src/gs-updates-page.c`) routes any
  `AS_COMPONENT_KIND_OPERATING_SYSTEM` app carrying it into the *offline*
  section, and `_button_update_all_clicked_cb()` (`src/gs-updates-section.c`)
  sets `do_reboot` for that whole section — so a quirk left behind after the
  pending generation is superseded turns the next interactive "Update" into an
  unprompted `gs_utils_invoke_reboot_async()`.
- **The `CheckUpdate` memo expires after 10 minutes**
  (`MODULIX_CHECK_MEMO_TTL_US`). It only has to bridge one GNOME Software pass —
  `refresh_metadata` and the `is-for-update` listing `refresh_cache_finished_cb`
  issues right after, seconds apart. Without an expiry, an update applied
  *outside* GNOME Software (`nixos-rebuild switch` in a terminal, the normal
  NixOS workflow) never clears it: every Updates-page visit would force a
  detail-less row back on, and the automatic monitor would spend a real
  multi-minute `UpdateSystem` on an update that no longer exists.
- **The memoized `CheckUpdate` boolean is what `force_outdated` now carries.**
  `gs_modulix_update_check()` stores its answer in a `G_LOCK`-guarded
  `last_check_outdated` (cleared once an update is applied), and
  `gs_modulix_update_sync()` folds it into `update_app_apply()` while it is
  fresh. So an update whose lockfile diff moved no *direct* input — `diff_locks()`
  in `modulix-core-utils` walks only the lockfile root's direct inputs, so a
  candidate that moved a transitive one yields zero rows for a real update — keeps
  the app `UPDATABLE_LIVE` instead of being declared current on the next page
  refresh. `gs_modulix_update_list()` appends the app when that returned TRUE,
  *or* when the app sits in `PENDING_INSTALL` (daemon cache empty by
  construction, row still has to offer the restart). Returning an empty list
  here stops the whole automatic chain dead: `get_updates_finished_cb` treats
  zero apps as "no updates".
- **`gs_modulix_update_check()`** is the live path, wired to
  `refresh_metadata_async` — GNOME Software's designated "go and check" hook
  (Updates-page Refresh button, `gs-update-monitor.c`'s daily check).
  `CheckUpdate` first, then the same `ListOutdatedInputs` read for the detail
  rows. **The boolean wins over the rows** when setting the state: a candidate
  lockfile whose diff moved no *direct* input yields an empty row list but is
  still a real update, which is why `update_app_apply()` takes a
  `force_outdated` argument. `cache_age_secs` is now ignored — the old
  `cache_age_secs == 0 → force_refresh` mapping was dead code, no in-tree
  GNOME Software caller passes `0`.
- **Flags → `UpdateSystem` mode** (`update_mode_for_flags()`):

  | Flags | Mode | Cores | Pre-call state | Final state |
  |---|---|---|---|---|
  | `NO_APPLY` (± `INTERACTIVE`) | `"build"` | half | `DOWNLOADING` | `UPDATABLE_LIVE` |
  | `INTERACTIVE`, no `NO_APPLY` (a click) | `"switch"` | half | `INSTALLING` | `INSTALLED` (+ `NEEDS_REBOOT` **iff** `Store1.RebootRequired`) |
  | anything else (the monitor) | `"boot"` | half | `INSTALLING` | `PENDING_INSTALL` (+ `NEEDS_REBOOT`) |
  | `NO_DOWNLOAD` **and** `NO_APPLY` | — (no-op) | — | untouched | untouched |

  **`INTERACTIVE` is the manual/automatic discriminant, and it is read.** Every
  click-driven path sets it (`src/gs-updates-section.c`, `src/gs-page.c`),
  `gs-update-monitor.c` never does. A user who clicked "Update Now" therefore
  gets `UpdateSystem("switch")` — the update applied to the running system —
  while the monitor's unattended apply stays `UpdateSystem("boot")`, which only
  pre-builds; `mx-apply-update.service` promotes it at shutdown. That split is
  the daemon's own design: `modulix-daemon/src/command/update.rs` documents
  `"switch"` as the manual path and the only mode that touches the running
  system.

  A click's *download* job carries `INTERACTIVE` **and** `NO_APPLY`, so it maps
  to `"build"` like the monitor's — only the apply job that follows switches.

  Two consequences to keep in mind:

  - **After a `"switch"` the row is `INSTALLED`, not `PENDING_INSTALL`**: the
    update *is* applied. `GS_APP_QUIRK_NEEDS_REBOOT` (read by
    `src/gs-updates-page.c`'s `_get_app_section`, `src/gs-updates-section.c`
    and `src/gs-common.c`) is then added only when `Store1.RebootRequired`
    reports that the running kernel, kernel modules or initrd no longer match
    the activated system — an exact comparison of `/run/booted-system` with
    `/run/current-system`, not a guess from package names. A switch that moved
    only userspace prompts for nothing.
  - **The daemon does not restart itself across a `"switch"`**
    (`restartIfChanged = false`), so an update carrying a new daemon keeps
    being served by the old binary until the next boot or an explicit
    `systemctl restart`. Never assume a fresh version's methods exist right
    after one.

  The row itself **stays** on the Updates page. Hiding it (it is listable only
  for the monitor: `src/gs-updates-page.c:670` passes
  `GS_PLUGIN_LIST_APPS_FLAGS_INTERACTIVE` where `src/gs-update-monitor.c:849`
  passes `FLAGS_NONE`, and `lib/gs-plugin-job-list-apps.c:304` forwards the flag
  to the vfunc) was considered and rejected: with GNOME Software's own automatic
  updates switched off (GSettings `download-updates`, `should_download_updates()`
  in `src/gs-update-monitor.c`) that row is the only remaining way to update at
  all. Note there is no middle ground — an `AS_COMPONENT_KIND_OPERATING_SYSTEM`
  app cannot be shown without a clickable action: `_get_app_section()` always
  lands it in `ONLINE` or `OFFLINE`, and `_update_buttons()`
  (`src/gs-updates-section.c`) renders those sections' button unconditionally,
  never looking at quirks.

  Restoring `UPDATABLE_LIVE` after `"build"` is load-bearing, not cosmetic — it
  is the single thing that lets the apply job be issued at all (see the two-job
  note above). The `NO_DOWNLOAD | NO_APPLY` no-op branch is unreachable in
  practice: `lib/gs-plugin-job-update-apps.c` `g_assert`s against that
  combination. `NO_DOWNLOAD` alone still cannot be honoured as "download only" —
  it takes the apply path.
- **No progress reporting**: the daemon emits no progress signal, so
  `update_apps_async` calls `progress_callback` once with
  `GS_APP_PROGRESS_UNKNOWN` right before the blocking call starts —
  indeterminate bar, not a bar stuck at 0%.
- **At most one system update runs at a time**, process-wide: a
  `G_LOCK`-guarded static flag in `gs-modulix-update.c` (not the per-app
  coalescing queue `gs-modulix-lifecycle.c` uses for install/uninstall — a
  system update is a single app, single call, nothing to batch). A second
  trigger while one is in flight fails immediately with
  `GS_PLUGIN_ERROR_FAILED` rather than queuing.

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
| `Store1` | `ListInstalledPlugins` | `s` (module, empty = all) | `aa{sv}` |
| `Store1` | `GetAppEnrichment` | `as` (app_ids) | `a{sa{sv}}` |
| `Store1` | `PackagesForAppId` | `s` (app_id) | `aa{sv}` |
| `Store1` | `GetPackageLicenses` | `as` (nix attrs) | `a{ss}` |
| `Store1` | `ListOutdatedInputs` | `b` (force_refresh) | `aa{sv}` (1h cache, refilled by `CheckUpdate`; empty = up to date) |
| `Store1` | `CheckUpdate` | — | `b` (full `nix flake update`, minutes; memoizes the candidate `flake.lock`) |
| `Store1` | `RebootRequired` | — | `(bas)` (reboot needed + what moved since boot; memoized per activation) |
| `Store1` | property `IndexReady` | — | `b` |
| `Daemon` (write, polkit-gated) | `InstallPackage` / `UninstallPackage` | `as` | `s` (status text) |
| `Daemon` | `InstallModule` / `UninstallModule` | `as` | `s` |
| `Daemon` | `InstallPlugin` / `UninstallPlugin` | `ss` (module, plugin) | `s` |
| `Daemon` | `UpdateSystem` | `s` (mode: `"switch"`/`"boot"`/`"build"`) | `s` |

Success = a D-Bus reply with no error. Packages/modules are batched into one
`as` call each; plugins are called individually. `a{sv}` entry field names
(`name`, `base_name`, `pname`, `app_name?`, `summary`, `version`, `app_id?`,
`group_id?`, `icon?`, `icon_name?`, `kind`, `flatpak_preferred`,
`variant_rank`, `score?`, `installed`) are documented in
`modulix-daemon/src/store/entry.rs`
and re-serialized by the shim to the same-named JSON keys this repo's C code
parses — see `plugin/src/gs-modulix-app.c`'s `gs_modulix_make_app_from_json`
for exactly which keys it reads. The enrichment entry
(`GetAppEnrichment`) carries `description?`, `screenshots?`, `icon?`,
`icon_name?` and `license?` (SPDX expression, or an AppStream
`LicenseRef-proprietary`/`LicenseRef-free` when only the free/unfree bit is
known — see `modulix-core-utils::license`). Each `ListOutdatedInputs` row
carries `input`/`current_rev`/`new_rev` (`s`) and `last_modified` (`t`, Unix
seconds) — see "System updates" above.

### `plugin/src/dbus/` GVariant wrappers

`gs_modulix_bus_init`/`_shutdown`/`_call` (`gs-modulix-bus.{c,h}`); reads
`gs_modulix_store1_{search_packages,search_modules,
list_installed_packages,list_installed_modules,list_module_plugins,
list_installed_plugins,get_app_enrichment,packages_for_app_id,
get_package_licenses,list_outdated_inputs,check_update}`
(`gs-modulix-store1.{c,h}`;
`get_app_enrichment` covers both the single-id and batched shapes the old
shim exposed as two symbols — there is only ever the one D-Bus method;
`list_outdated_inputs` is the one call with a variable timeout —
`MODULIX_STORE1_REFRESH_TIMEOUT_MS` instead of the usual 30s when
`force_refresh` is TRUE; `check_update` is the one read with **no** timeout
(`MODULIX_STORE1_CHECK_TIMEOUT_MS = G_MAXINT`, same reasoning as the write
side) and the one returning a plain `gboolean` rather than a `GVariant`);
writes `gs_modulix_daemon1_names_call`/
`_plugin_call`/`_update_system` (`gs-modulix-daemon1.{c,h}`).
A read returns `NULL` on failure (connection error, D-Bus error) after
logging its own `g_warning`. A write returns `NULL` **with a `GError`**
(connection error, D-Bus error, denied polkit authorization) — unlike the
old shim, this repo's code *can* see why a write failed
(`lifecycle_execute` in `gs-modulix-lifecycle.c` still only surfaces a
generic status to GNOME Software, but the detail is logged).

## Gotchas / current limitations

- **`just build` and `nix build .#plugin` do not see the same files.**
  `nix build`/`nix flake check` resolve `src = ./.` through git's own
  filtered source, which only sees files **tracked or staged** — a brand-new
  file that was never `git add`-ed (even though it's on disk and `just
  build` compiles it fine) is silently absent from the Nix build, which then
  fails with a meson "File … does not exist" error, or worse, a stale
  binary with a missing symbol. Hit this exact trap adding `plugin/src/dbus/`
  (Partie C, JSON→GVariant migration): `just build` succeeded immediately,
  `nix build .#plugin` failed until the new files were staged with
  `git add`. Stage new files (no need to commit) before trusting a Nix
  build result. Verify what actually shipped rather than assuming:
  `strings …/plugins-23/libgs_plugin_modulix.so | grep -cx 'modulix::kind'`.
- **The Installed page silently drops any app with a NULL description.**
  `gs_installed_page_is_actual_app()` (`src/gs-installed-page.c`) is the only
  gate — an app can pass `gs_plugin_loader_app_is_valid()` and
  `filter_app_kinds_cb` and still never be rendered. Our description only ever
  comes from Flathub enrichment, so every nix package with no Flatpak
  counterpart (no `app_id` → `refine_one` returns before fetching) used to be
  invisible there. `refine_one` now seeds the summary as a
  `GS_APP_QUALITY_LOWEST` description; Flathub's own, written at `NORMAL`,
  still replaces it. `meta.longDescription` is not an alternative — it is
  empty for essentially every attribute we list.
- **`just run-gs` tests nothing while GNOME Software is already running.**
  `mx` starts it as a user service
  (`systemd.user.services.gnome-software.wantedBy = graphical-session.target`),
  so it owns `org.gnome.Software` on the session bus and GApplication makes
  the dev launcher hand its arguments to that instance and exit — the local
  `~/.local` plugin is never loaded, and the log stops right after the
  `[dev] overlaying N plugins` banner. Run `gnome-software --quit` (or
  `systemctl --user stop gnome-software`) first. `dbus-run-session` is not a
  workaround: gnome-software segfaults under it inside the bubblewrap runner.
- Sort-key suffix ranking and `FLATPAK_PREFERRED_APP_IDS` are curated (on the
  daemon side, `modulix-core-utils::package_info`) — tune against the real
  flatpak/packagekit app priorities with d-spy/bustle.
- `Store1.PackagesForAppId` no longer runs `nix eval` (it used to, to expand
  multi-output packages — that expansion was removed along with the now-dead
  `expand_outputs`/`fetch_outputs`/`expand_with_outputs`). It is table
  lookups + one bounded (500ms) module-index lookup + an optional `nix
  search` for pname-only groups, cached per app-id for 5 minutes daemon-side.
- Per-plugin installed state (`mx.<module>.plugins`) is wired end to end:
  stamped by the daemon on both `ListModulePlugins` (per-module catalogue)
  and `ListInstalledPlugins` (cross-module "Installed" page listing), and
  refreshed client-side by `gs-modulix-refine.c`'s `refine_plugin()` even
  when a plugin's addon `GsApp` is reached outside its parent module's
  details page.
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
- `Store1.GetPackageLicenses` runs one `nix eval` per uncached attribute and
  caps a single call at `MAX_LICENSE_EVALS` (16, `modulix-daemon/src/store/mod.rs`);
  attributes past the cap are served from cache or omitted. The plugin-side
  guard (ADDONS-only, see "Licenses") is the one that actually keeps the
  search page off this path — if a future GNOME Software page requests
  `LICENSE` together with `ADDONS` over a long list, that guard needs
  revisiting.
- `mx_store_*` write calls collapse every failure mode (bus down, D-Bus
  error, denied polkit prompt, daemon-side transaction failure) to `NULL`;
  `lifecycle_execute` reports a single generic `GS_PLUGIN_ERROR_FAILED` for
  the whole batch rather than a per-app reason.
