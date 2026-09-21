/**
 * @file gs-modulix-app.c
 * @brief GsApp construction from Modulix JSON payloads.
 *
 * Turns the JSON entries emitted by the `modulix-store-client` C ABI shim
 * (`mx_store_*`, itself a reshaping of the `a{sv}` rows
 * `org.modulix.Store1` returns — see `AppEntry`/`PluginEntry`/`EnrichEntry`
 * in `modulix-daemon/src/store/entry.rs` and `dict_to_json`/`shots_to_json`
 * in `modulix-store-client/src/convert.rs` for the exact wire contract)
 * into `GsApp` objects GNOME Software can list, refine, install and
 * uninstall.
 *
 * Covers:
 *   - gs_modulix_make_app_from_json()       : single JSON object → GsApp
 *   - gs_modulix_append_apps_from_json()    : JSON array → GsAppList
 *   - gs_modulix_add_module_plugins()       : attach ADDON plugins to a module
 *   - gs_modulix_add_app_screenshots()      : attach AsScreenshot objects
 *
 * See CLAUDE.md ("GsApp mapping & dedup") for the design rationale behind
 * the SortKey / match-value / priority scheme in this file, and for how a
 * Modulix `GsApp` deduplicates against the Flatpak of the same app.
 *
 * The daemon (`modulix-daemon/src/store/entry.rs`) only emits neutral fields
 * (`kind`, `variant_rank`, `score`) — no GNOME-Software-specific number
 * crosses the bus. This is the one place that turns them into the two GNOME
 * Software conventions: `GnomeSoftware::SortKey` metadata (orders the
 * details-page "Sources" popover) and `GsApp::match-value` (orders the
 * search-results page).
 */

#ifndef I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#endif

#include "gs-modulix-app.h"
#include "gs-modulix-icon-cache.h"
#include "gs-modulix-icon-resolver.h"
#include "gs-modulix-json-utils.h"
#include "gs-modulix-plugins-cache.h"

#include <glib/gi18n-lib.h>

/**
 * @brief Sources-popover SortKey for a Modulix module.
 *
 * Lower sorts first in the "Sources" popover. A Modulix module must sort
 * below the lowest in-tree plugin value (flatpak: 100) to come first; a nix
 * variant sorts within PACKAGE_SORT_BASE + variant_rank, comfortably above
 * all of them (a Flatpak with none of this metadata falls back to the
 * patched gnome-software's default of 1000, still below a nix variant).
 *
 * Consumed as the literal value written under `"GnomeSoftware::SortKey"`
 * for every entry whose `kind` is `"module"` (see
 * gs_modulix_make_app_from_json()).
 */
#define MODULIX_MODULE_SORT_KEY 50

/**
 * @brief Base offset added to a package entry's `variant_rank` to form its
 *        Sources-popover SortKey.
 *
 * The final SortKey for a non-module entry is
 * `MODULIX_PACKAGE_SORT_BASE + variant_rank` (see
 * gs_modulix_make_app_from_json()); `variant_rank` itself is computed
 * daemon-side (`variant_rank()` in `modulix-daemon/src/store/entry.rs`) and
 * carried verbatim in the JSON `"variant_rank"` key.
 */
#define MODULIX_PACKAGE_SORT_BASE 2000

/**
 * @brief Sets @p app's dedup priority (`gs_app_compare_priority()`,
 *        `lib/gs-app.c`), overriding the `GS_PLUGIN_RULE_BETTER_THAN`
 *        fixed-point default.
 *
 * Declared here because the symbol lives in the unexported
 * `gs-app-private.h` (not shipped with this plugin's GNOME Software
 * headers) but is exported by `libgnomesoftware.so`, which this plugin
 * already links against (`gnome-software.pc: Libs: -lgnomesoftware`) — no
 * GNOME Software patch is needed to call it. Defined in GNOME Software's
 * `lib/gs-app.c`, not in this file.
 *
 * @param app GsApp whose priority is set. Not NULL. Mutated: its priority
 *   value is overwritten.
 * @param priority New priority value; higher wins `gs_app_compare_priority()`
 *   ties among same-id apps once at least one app in the comparison has a
 *   non-zero (explicitly set) priority.
 * @pre None beyond a valid @p app.
 * @post @p app's priority is @p priority.
 * @return None.
 */
void gs_app_set_priority(GsApp *app, guint priority);

/**
 * @brief Per-app dedup priority given to a Modulix module, so it outranks a
 *        same-id Flatpak (priority 1) and a bare nix package (the 0
 *        sentinel).
 *
 * Passed to gs_app_set_priority() in gs_modulix_make_app_from_json() for
 * every entry whose `kind` is `"module"`.
 */
#define MODULIX_PRIORITY_MODULE 2

/**
 * @brief Upper bound of the GsApp::match-value range reserved for
 *        AppStream/nix-package search hits.
 *
 * Maximum match-value reachable by AppStream once the id-match bit is
 * stripped (gs_appstream_add_search_hit, lib/gs-appstream.c). Nix packages
 * are scaled onto this same range so they interleave correctly with
 * Flatpak/AppStream hits in the search page's sort key.
 *
 * Used as the scale target in modulix_match_value().
 */
#define MODULIX_MATCH_SCALE_MAX 0x7F

/**
 * @brief Maximum raw relevance `score` value a Modulix entry can carry.
 *
 * score() value of a perfect name match: 1000 (exact) + 500 (substring at
 * position 0) + 200 (levenshtein distance 0).
 *
 * Used both as the clamp ceiling and as the scaling denominator in
 * modulix_match_value().
 */
#define MODULIX_MATCH_EXACT_SCORE 1700

/**
 * @brief Match-value bonus added on top of the scaled score for a Modulix
 *        module, so a relevant module always outranks its own Flatpak in
 *        search results.
 *
 * Floor reserved for modules: strictly above any AppStream/nix match value,
 * so a relevant module always sorts ahead of its own Flatpak.
 *
 * Added in modulix_match_value() when `is_module` is TRUE.
 */
#define MODULIX_MATCH_MODULE_BONUS 0x80

/**
 * @brief Rescales a raw Modulix relevance score into the GsApp::match-value
 *        range used by the search page's sort key.
 *
 * Scales a raw relevance `score` (0..=MODULIX_MATCH_EXACT_SCORE) into the
 * GsApp::match-value range used by the search page's sort key
 * (kind : state : match_value : rating : kudos). Packages land in
 * 1..=0x7F; modules get 0x80 added on top so they always precede a
 * package/Flatpak of equal relevance.
 *
 * Only called by gs_modulix_make_app_from_json() when the JSON entry's
 * `"score"` key is present (the daemon omits it entirely outside search
 * paths, see `AppEntry::score` in `modulix-daemon/src/store/entry.rs`).
 *
 * @param score Raw relevance value from the JSON entry's `"score"` key,
 *   expected in `0..=MODULIX_MATCH_EXACT_SCORE` but not required to be:
 *   values above the ceiling are clamped, not rejected.
 * @param is_module TRUE when the entry's `"kind"` is `"module"`.
 * @pre None.
 * @post None (pure function).
 * @return A value in `1..=MODULIX_MATCH_SCALE_MAX` for a package, or
 *   `MODULIX_MATCH_MODULE_BONUS + (1..=MODULIX_MATCH_SCALE_MAX)` for a
 *   module. Never `0`, so a Modulix entry never sorts as "no relevance" in
 *   the search page.
 */
static guint modulix_match_value(gint score, gboolean is_module) {
  gint capped = MIN(score, MODULIX_MATCH_EXACT_SCORE);
  guint scaled =
      MAX(1u, (guint)((capped * MODULIX_MATCH_SCALE_MAX) / MODULIX_MATCH_EXACT_SCORE));
  return is_module ? MODULIX_MATCH_MODULE_BONUS + scaled : scaled;
}

/**
 * @brief Parses `meta`'s `"screenshots"` array and attaches AsScreenshot
 *        objects to @p app.
 *
 * Expects the shape produced by `shots_to_json()`
 * (`modulix-store-client/src/convert.rs`) from the daemon's
 * `EnrichEntry::screenshots` (`ShotTuple` array,
 * `modulix-daemon/src/store/entry.rs`):
 * `"screenshots": [{"caption": string, "default": bool,
 * "images": [{"url": string, "width": uint, "height": uint}]}]`.
 *
 * Per-element handling:
 *   - `meta` missing the `"screenshots"` key, or that key holding a
 *     non-array value: the function returns without adding anything.
 *   - A screenshot object: `"caption"` read via gs_modulix_json_str()
 *     (`""` when absent/wrong-typed — an empty/absent caption is simply
 *     not passed to as_screenshot_set_caption()); `"default"` read via
 *     gs_modulix_json_bool() (`FALSE` when absent/wrong-typed), selecting
 *     `AS_SCREENSHOT_KIND_DEFAULT` vs `AS_SCREENSHOT_KIND_EXTRA`.
 *   - A screenshot missing `"images"`, or whose `"images"` is not an
 *     array, or is an empty array: the whole screenshot is skipped (no
 *     AsScreenshot is created for it) — an AsScreenshot with zero images
 *     would not be useful to GNOME Software.
 *   - Within `"images"`: an image whose `"url"` is absent/empty
 *     (gs_modulix_json_str() returning `""`) is skipped; `"width"`/
 *     `"height"` are read via gs_modulix_json_int() (`0` when
 *     absent/wrong-typed) and stamped verbatim, so a real screenshot with
 *     unknown dimensions ends up with an AsImage advertising 0x0.
 *
 * @param app GsApp to attach screenshots to. Not NULL.
 * @param meta JsonObject read for its `"screenshots"` member (an
 *   enrichment payload — see `EnrichEntry` above). Not NULL. Read-only.
 * @pre None.
 * @post Unchanged if @p app already had at least one screenshot (see
 *   below), or if `"screenshots"` is absent/malformed. Otherwise @p app
 *   gains one AsScreenshot (transfer taken by gs_app_add_screenshot(), the
 *   local `g_autoptr(AsScreenshot)` is not leaked) per well-formed source
 *   screenshot, each carrying one AsImage
 *   (`AS_IMAGE_KIND_SOURCE`) per well-formed source image, in source order.
 * @return None.
 *
 * A GsApp now outlives the query that produced it (see the plugin cache in
 * gs_modulix_make_app_from_json), so refine can run on one that already has
 * its screenshots — appending would show each of them twice. This function
 * guards against that by returning immediately when
 * gs_app_get_screenshots() is already non-empty, so a second call on the
 * same (possibly differently-populated) `meta` is silently a no-op rather
 * than refreshing the screenshot set.
 */
void gs_modulix_add_app_screenshots(GsApp *app, JsonObject *meta) {
  GPtrArray *existing = gs_app_get_screenshots(app);
  if (existing != NULL && existing->len > 0)
    return;
  if (!json_object_has_member(meta, "screenshots"))
    return;
  JsonNode *node = json_object_get_member(meta, "screenshots");
  if (!JSON_NODE_HOLDS_ARRAY(node))
    return;

  JsonArray *shots = json_node_get_array(node);
  for (guint i = 0; i < json_array_get_length(shots); i++) {
    JsonObject *shot = json_array_get_object_element(shots, i);
    const gchar *caption = gs_modulix_json_str(shot, "caption");
    gboolean is_default = gs_modulix_json_bool(shot, "default");

    if (!json_object_has_member(shot, "images"))
      continue;
    JsonNode *imgs_node = json_object_get_member(shot, "images");
    if (!JSON_NODE_HOLDS_ARRAY(imgs_node))
      continue;
    JsonArray *imgs = json_node_get_array(imgs_node);
    if (json_array_get_length(imgs) == 0)
      continue;

    g_autoptr(AsScreenshot) ss = as_screenshot_new();
    as_screenshot_set_kind(ss, is_default ? AS_SCREENSHOT_KIND_DEFAULT
                                          : AS_SCREENSHOT_KIND_EXTRA);
    if (caption && *caption)
      as_screenshot_set_caption(ss, caption, NULL);

    for (guint j = 0; j < json_array_get_length(imgs); j++) {
      JsonObject *img = json_array_get_object_element(imgs, j);
      const gchar *url = gs_modulix_json_str(img, "url");
      if (!url || !*url)
        continue;
      g_autoptr(AsImage) image = as_image_new();
      as_image_set_kind(image, AS_IMAGE_KIND_SOURCE);
      as_image_set_url(image, url);
      as_image_set_width(image, (guint)gs_modulix_json_int(img, "width"));
      as_image_set_height(image, (guint)gs_modulix_json_int(img, "height"));
      as_screenshot_add_image(ss, image);
    }
    gs_app_add_screenshot(app, ss);
  }
}

/**
 * @brief Builds a 128x128 remote GIcon for a Flathub/Flatpak icon URL.
 *
 * Flathub artwork is 128×128, and the icon downloader caps requests at
 * 160 logical px anyway (gs-plugin-icons.c) — a bigger claimed width used
 * to win gs_app_get_icon_for_size()'s first pass unconditionally, then get
 * silently corrected once downloaded, changing the displayed icon under
 * the user's eyes. Forcing the claimed size to 128x128 up front keeps the
 * first pass honest about what will actually be downloaded.
 *
 * Called from gs_modulix_make_app_from_json() (icon-resolver level 4,
 * `gs-modulix-icon-resolver.c`) and from the Flathub-enrichment step in
 * refine (`gs-modulix-refine.c`) for the remote-icon fallback level.
 *
 * @param url Icon URL. Not NULL (the GsRemoteIcon it wraps is meaningless
 *   otherwise).
 * @pre None.
 * @post None (no state beyond the returned object).
 * @return A new GIcon (transfer-full: caller owns the reference) wrapping
 *   @p url via gs_remote_icon_new(), with gs_icon_set_width()/
 *   gs_icon_set_height() both forced to 128. Never NULL.
 */
GIcon *gs_modulix_remote_icon_new(const gchar *url) {
  GIcon *remote = gs_remote_icon_new(url);
  gs_icon_set_width(remote, 128);
  gs_icon_set_height(remote, 128);
  return remote;
}

/**
 * @brief Tells whether @p state is owned by an in-flight install/uninstall
 *        and must not be overwritten by a listing refresh.
 *
 * States owned by an install/uninstall already under way (`enqueue_lifecycle`
 * in gs-modulix-lifecycle.c sets them on the main thread, the drain worker
 * resolves them later). A listing that happens to run in between must not
 * stamp the daemon's — necessarily pre-operation — state over them.
 *
 * Used as a guard by gs_modulix_make_app_from_json() and
 * gs_modulix_add_module_plugins() before calling gs_app_set_state(): the
 * daemon-derived `installed` state is applied only when this returns FALSE
 * for the app's *current* state.
 *
 * @param state GsAppState to classify.
 * @pre None.
 * @post None (pure function).
 * @return TRUE for GS_APP_STATE_INSTALLING, GS_APP_STATE_REMOVING,
 *   GS_APP_STATE_QUEUED_FOR_INSTALL, GS_APP_STATE_PURCHASING or
 *   GS_APP_STATE_DOWNLOADING; FALSE for every other state.
 */
static gboolean state_is_transient(GsAppState state) {
  switch (state) {
  case GS_APP_STATE_INSTALLING:
  case GS_APP_STATE_REMOVING:
  case GS_APP_STATE_QUEUED_FOR_INSTALL:
  case GS_APP_STATE_PURCHASING:
  case GS_APP_STATE_DOWNLOADING:
    return TRUE;
  default:
    return FALSE;
  }
}

/**
 * @brief Builds (or refreshes, if cached) the GsApp for one Modulix JSON app
 *        entry.
 *
 * This is the single place that maps a Modulix JSON app entry — one element
 * of the daemon's `AppEntry::into_dict()` output re-encoded to JSON by
 * `dict_to_json()` — onto a `GsApp`. JSON keys read from @p obj, all via
 * gs_modulix_json_str()/gs_modulix_json_bool()/gs_modulix_json_int()
 * (each returns `""`/`FALSE`/`0` respectively for a missing or
 * wrong-JSON-type key — see `gs-modulix-json-utils.c` — so every read below
 * degrades gracefully rather than crashing on an unexpected shape):
 *
 *   - `"name"` — required; gs_modulix_json_str() default of `""` for a
 *     missing/empty value makes the `!name || !*name` check below always
 *     take the empty-string branch (never a real NULL) — @b required: the
 *     function returns NULL (drops the row) when this is empty.
 *   - `"base_name"` — falls back to `name` when empty.
 *   - `"pname"`, `"app_name"` — feed the display-name choice
 *     (`app_name` > `pname` > `name`).
 *   - `"summary"` — passed to gs_app_set_summary() verbatim (possibly
 *     empty).
 *   - `"version"` — passed to gs_app_set_version() only when non-empty;
 *     otherwise the app's version is left as whatever it already was
 *     (relevant for a cache hit).
 *   - `"app_id"` — the AppStream id, used as an icon-cache key, an
 *     `app_unique_id` fallback, and stored as `"modulix::app_id"` object
 *     data when non-empty.
 *   - `"group_id"` — preferred `app_unique_id` (falls back to `app_id`,
 *     then `name`) — this is the *GsApp id* (`gs_app_new(app_unique_id)`),
 *     the loader's own dedup key against Flatpak/AppStream.
 *   - `"icon"` — raw icon URL/path; combined with the cross-instance icon
 *     cache (`gs-modulix-icon-cache.h`) keyed by `app_id` so an
 *     `alternate_of` row carrying no icon of its own can still reuse one
 *     seen earlier for the same `app_id`.
 *   - `"icon_name"` — themed icon name candidate, forwarded to the
 *     resolver and stored as `"modulix::icon_name"` object data when
 *     non-empty.
 *   - `"kind"` — `"module"` vs anything else (treated as `"package"`);
 *     drives every module-vs-package branch below (priority, SortKey,
 *     origin, PackagingFormat/-Icon/-BaseCssColor metadata, match-value
 *     bonus) and is stored verbatim (or as literal `"package"` when
 *     empty) as `"modulix::kind"` object data.
 *   - `"flatpak_preferred"` — stored as `"modulix::flatpak_preferred"`
 *     object data only when TRUE; per CLAUDE.md ("GsApp mapping & dedup")
 *     this flag is currently inert — nothing reads it back to suppress a
 *     module's priority bump.
 *   - `"variant_rank"` — folded into the SortKey of a non-module entry
 *     (`MODULIX_PACKAGE_SORT_BASE + variant_rank`); ignored for a module
 *     (which always gets `MODULIX_MODULE_SORT_KEY`).
 *   - `"score"` — presence-checked with json_object_has_member()
 *     (`has_score`), *not* read through gs_modulix_json_int() unconditionally:
 *     the daemon omits this key entirely outside search paths (see
 *     `AppEntry::into_dict`), and an absent key must leave match-value
 *     untouched rather than force it to `0`. gs_app_set_match_value() is
 *     only called when `has_score` is TRUE, via modulix_match_value().
 *   - `"installed"` — presence-checked the same way; when present, its
 *     boolean value is authoritative and @p is_installed is ignored; when
 *     absent (a daemon predating the field), @p is_installed is used
 *     instead. Before the daemon stamped this on every entry, only the
 *     caller's query path decided the fallback, so search results and
 *     Sources-popover rows were born AVAILABLE no matter what the system
 *     actually had installed.
 *
 * GsApp identity and caching: the GsApp id is `group_id` (falling back to
 * `app_id` then `name`) — the backend's grouping key, shared by every
 * variant that should stack into one details-page row. The *plugin cache*
 * key is different and deliberate: `"<kind>\x1f<name>"` (same separator as
 * gs-modulix-lifecycle.c's `run_group()` dedup key), via
 * `gs_plugin_cache_lookup()`/`gs_plugin_cache_add()` — the same reuse
 * pattern gs-plugin-flatpak/packagekit/epiphany use. `name` (the nix
 * attribute or module name), not the GsApp id, is what is unique per entry
 * and what install/uninstall act on: the `alternate_of` path deliberately
 * emits several entries sharing one GsApp id so they stack in the Sources
 * popover (see gs-modulix-list.c), and still needs distinct, independently
 * cached GsApp instances for them. This cache reuse is what keeps state
 * alive across queries: `gs_details_page_get_alternates_cb()`
 * (`src/gs-details-page.c`) swaps in the matching row of the `alternate_of`
 * reply via `_set_app()`, and that reply is re-issued every time the page
 * reloads after an install — a freshly built instance would come back
 * AVAILABLE and flip the button back to "Install" the moment the install
 * finished. Lookup-or-insert is done under a function-local static GMutex
 * so two threads racing on the same cache key (e.g. a parallel refine +
 * alternate_of query, both fired in parallel by `gs_details_page_reload`)
 * cannot both miss and each insert their own GsApp, which would let the
 * plugin cache evict one of them out from under a caller already holding
 * it.
 *
 * GsApp properties/quirks/states set (all via public `libgnomesoftware`
 * API except gs_app_set_priority(), see above):
 *   - `management-plugin` = @p plugin (this is, together with
 *     `bundle-kind`, part of how a Modulix app is told apart from a
 *     Flatpak of the same id: a Flatpak's management plugin is the
 *     flatpak plugin, and its bundle-kind is `AS_BUNDLE_KIND_FLATPAK`, not
 *     `AS_BUNDLE_KIND_PACKAGE`).
 *   - `priority` = MODULIX_PRIORITY_MODULE, module entries only (see
 *     gs_app_set_priority() above).
 *   - `name` at GS_APP_QUALITY_NORMAL, `summary` at GS_APP_QUALITY_NORMAL.
 *   - `kind` = AS_COMPONENT_KIND_DESKTOP_APP.
 *   - `scope` = AS_COMPONENT_SCOPE_SYSTEM (nix/module installs are
 *     system-wide, done by the daemon as root).
 *   - `state` = GS_APP_STATE_INSTALLED/`_AVAILABLE` per the resolved
 *     `installed` value, unless state_is_transient() says the app's
 *     current state belongs to an in-flight install/uninstall, in which
 *     case the state is left untouched.
 *   - `bundle-kind` = AS_BUNDLE_KIND_PACKAGE — mandatory (together with an
 *     icon) or the loader drops the app entirely (see CLAUDE.md).
 *   - metadata `"GnomeSoftware::SortKey"` — the popover order (see the
 *     macros above). @b Quirk: gs_app_set_metadata() is write-once per key
 *     (a second write of the same key logs a warning and is a no-op, see
 *     `lib/gs-app.c`); because this function unconditionally re-sets this
 *     key (and the three `PackagingFormat`/`PackagingIcon`/
 *     `PackagingBaseCssColor` keys below) on *every* call, a cache hit
 *     (the same GsApp refined or re-listed a second time) always attempts
 *     to rewrite the same key/value pair and always loses that race,
 *     logging a spurious warning each time — harmless (the value cannot
 *     have changed for a fixed kind/name) but noisy.
 *   - `match-value`, only when `has_score` (see above).
 *   - `version`, only when non-empty (see above).
 *   - icon: resolved through gs_modulix_icon_resolve()
 *     (`gs-modulix-icon-resolver.c`) — theme, then local flatpak/GS icon
 *     caches, then the remote URL, then a generic fallback — but @b only
 *     when `!gs_app_has_icons(app)`. Combined with GsApp reuse via the
 *     plugin cache, this means icon resolution runs at most **once** per
 *     (kind, name): a later call that would have resolved a different
 *     icon (e.g. `icon`/`icon_name` changed in a newer JSON payload) has
 *     no effect, by design (the resolver's contract is exactly one GIcon
 *     per GsApp, see CLAUDE.md "Icons"). A themed win additionally sets
 *     `"modulix::icon-themed"` object data (read by refine to avoid
 *     letting a later remote icon override it).
 *   - `origin` = `"modulix"` for a module, else `name`; `origin-hostname`
 *     = `"nixos.org"`; `origin-ui` = the translated `"Modulix OS"` for a
 *     module, else the untranslated `name`.
 *   - metadata `"GnomeSoftware::PackagingFormat"` (translated `"Module"`
 *     or `"Nix Package"`), `"GnomeSoftware::PackagingIcon"`
 *     (`"modulix-logo"` or `"nix-snowflake"`),
 *     `"GnomeSoftware::PackagingBaseCssColor"` (`"accent_color"` or
 *     `"error_color"`) — same write-once quirk as SortKey above.
 *
 * Object data set on @p app (all `g_object_set_data_full()` with
 * `g_free()`, except the two flag-style ones noted): `"modulix::kind"`
 * (always, `kind` or literal `"package"`), `"modulix::name"` (always,
 * `name`), `"modulix::app_id"` (only when `app_id` non-empty),
 * `"modulix::icon"` (only when `icon` non-empty), `"modulix::icon_name"`
 * (only when `icon_name` non-empty), `"modulix::base_name"` (always),
 * `"modulix::flatpak_preferred"` (`g_object_set_data()`, a bare
 * `GINT_TO_POINTER(1)` flag with no destroy, only when `fp_pref` is TRUE).
 * @b Quirk: the three conditional ones are only ever *added or overwritten*,
 * never cleared — on a cache hit, a JSON entry that now omits a
 * field it previously carried (e.g. `icon` becoming empty) leaves the
 * stale previous value in place rather than removing it.
 *
 * @param obj JsonObject for one Modulix app entry. Not NULL. Read-only.
 * @param plugin GsPlugin used as the app's management plugin and as the
 *   plugin-cache namespace (`gs_plugin_cache_lookup()`/`_add()`). Not
 *   NULL.
 * @param is_installed Fallback installed state, used only when @p obj has
 *   no `"installed"` key.
 * @pre None.
 * @post On success, the plugin cache holds a GsApp for key
 *   `"<kind>\x1f<name>"`, newly created or refreshed from @p obj.
 * @return A new reference to the GsApp (transfer-full: caller must
 *   g_object_unref()), reused from the plugin cache across calls sharing
 *   the same (kind, name). NULL when @p obj's `"name"` is missing or
 *   empty.
 */
GsApp *gs_modulix_make_app_from_json(JsonObject *obj, GsPlugin *plugin,
                                     gboolean is_installed) {
  const gchar *name      = gs_modulix_json_str(obj, "name");
  const gchar *base_name = gs_modulix_json_str(obj, "base_name");
  const gchar *pname     = gs_modulix_json_str(obj, "pname");
  const gchar *a_name    = gs_modulix_json_str(obj, "app_name");
  const gchar *summary   = gs_modulix_json_str(obj, "summary");
  const gchar *version   = gs_modulix_json_str(obj, "version");
  const gchar *app_id    = gs_modulix_json_str(obj, "app_id");
  const gchar *group_id  = gs_modulix_json_str(obj, "group_id");
  const gchar *icon      = gs_modulix_json_str(obj, "icon");
  const gchar *icon_name = gs_modulix_json_str(obj, "icon_name");
  const gchar *kind      = gs_modulix_json_str(obj, "kind");
  gboolean fp_pref       = gs_modulix_json_bool(obj, "flatpak_preferred");
  gint variant_rank      = gs_modulix_json_int(obj, "variant_rank");
  gboolean has_score     = json_object_has_member(obj, "score");
  gint score             = has_score ? gs_modulix_json_int(obj, "score") : 0;

  if (!name || !*name)
    return NULL;

  if (!base_name || !*base_name)
    base_name = name;

  gboolean is_module = (g_strcmp0(kind, "module") == 0);

  gboolean installed = json_object_has_member(obj, "installed")
                           ? gs_modulix_json_bool(obj, "installed")
                           : is_installed;

  const gchar *app_unique_id = (group_id && *group_id) ? group_id
                               : (app_id && *app_id)    ? app_id
                                                        : name;
  g_autofree gchar *cache_key =
      g_strdup_printf("%s\x1f%s", (kind && *kind) ? kind : "package", name);
  static GMutex cache_key_mutex;
  GsApp *app;
  {
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&cache_key_mutex);
    app = gs_plugin_cache_lookup(plugin, cache_key);
    if (app == NULL) {
      app = gs_app_new(app_unique_id);
      gs_plugin_cache_add(plugin, cache_key, app);
    }
  }

  gs_app_set_management_plugin(app, plugin);
  if (is_module)
    gs_app_set_priority(app, MODULIX_PRIORITY_MODULE);

  const gchar *display_name = (a_name && *a_name)   ? a_name
                              : (pname && *pname)    ? pname
                                                      : name;
  gs_app_set_name(app, GS_APP_QUALITY_NORMAL, display_name);
  gs_app_set_summary(app, GS_APP_QUALITY_NORMAL, summary);
  gs_app_set_kind(app, AS_COMPONENT_KIND_DESKTOP_APP);
  gs_app_set_scope(app, AS_COMPONENT_SCOPE_SYSTEM);
  if (!state_is_transient(gs_app_get_state(app)))
    gs_app_set_state(app, installed ? GS_APP_STATE_INSTALLED
                                    : GS_APP_STATE_AVAILABLE);
  gs_app_set_bundle_kind(app, AS_BUNDLE_KIND_PACKAGE);

  {
    gint sort_key = is_module ? MODULIX_MODULE_SORT_KEY
                              : MODULIX_PACKAGE_SORT_BASE + variant_rank;
    g_autofree gchar *sort_key_str = g_strdup_printf("%d", sort_key);
    gs_app_set_metadata(app, "GnomeSoftware::SortKey", sort_key_str);
  }

  if (has_score)
    gs_app_set_match_value(app, modulix_match_value(score, is_module));

  if (version && *version)
    gs_app_set_version(app, version);

  g_autofree gchar *cached_icon = NULL;
  if (app_id && *app_id) {
    if (icon && *icon) {
      gs_modulix_icon_cache_put(app_id, icon);
    } else {
      cached_icon = gs_modulix_icon_cache_get(app_id);
      if (cached_icon != NULL)
        icon = cached_icon;
    }
  }

  if (!gs_app_has_icons(app) &&
      gs_modulix_icon_resolve(app, app_id, icon_name, name, base_name, icon))
    g_object_set_data(G_OBJECT(app), "modulix::icon-themed", GINT_TO_POINTER(1));

  gs_app_set_origin(app, is_module ? "modulix" : name);
  gs_app_set_origin_hostname(app, "nixos.org");

  if (is_module) {
    gs_app_set_origin_ui(app, _("Modulix OS"));
    gs_app_set_metadata(app, "GnomeSoftware::PackagingFormat", _("Module"));
    gs_app_set_metadata(app, "GnomeSoftware::PackagingIcon", "modulix-logo");
    gs_app_set_metadata(app, "GnomeSoftware::PackagingBaseCssColor", "accent_color");
  } else {
    gs_app_set_origin_ui(app, name);
    gs_app_set_metadata(app, "GnomeSoftware::PackagingFormat", _("Nix Package"));
    gs_app_set_metadata(app, "GnomeSoftware::PackagingIcon", "nix-snowflake");
    gs_app_set_metadata(app, "GnomeSoftware::PackagingBaseCssColor", "error_color");
  }

  g_object_set_data_full(G_OBJECT(app), "modulix::kind",
                         g_strdup(kind && *kind ? kind : "package"), g_free);
  g_object_set_data_full(G_OBJECT(app), "modulix::name", g_strdup(name), g_free);
  if (app_id && *app_id)
    g_object_set_data_full(G_OBJECT(app), "modulix::app_id",
                           g_strdup(app_id), g_free);
  if (icon && *icon)
    g_object_set_data_full(G_OBJECT(app), "modulix::icon", g_strdup(icon), g_free);
  if (icon_name && *icon_name)
    g_object_set_data_full(G_OBJECT(app), "modulix::icon_name",
                           g_strdup(icon_name), g_free);
  g_object_set_data_full(G_OBJECT(app), "modulix::base_name",
                         g_strdup(base_name), g_free);
  if (fp_pref)
    g_object_set_data(G_OBJECT(app), "modulix::flatpak_preferred",
                      GINT_TO_POINTER(1));

  return app;
}

/**
 * @brief Tells whether @p app's management plugin is @p plugin.
 *
 * Thin wrapper over gs_app_has_management_plugin(); see
 * gs-modulix-app.h for the full contract.
 *
 * @param app GsApp to inspect. Not NULL.
 * @param plugin GsPlugin to test ownership against. Not NULL.
 * @pre None.
 * @post None (no mutation).
 * @return The result of gs_app_has_management_plugin(app, plugin).
 */
gboolean gs_modulix_app_is_ours(GsApp *app, GsPlugin *plugin) {
  return gs_app_has_management_plugin(app, plugin);
}

/**
 * @brief Reads back the `"modulix::kind"` object data set by
 *        gs_modulix_make_app_from_json()/gs_modulix_add_module_plugins().
 *
 * @param app GsApp to inspect. Not NULL.
 * @pre None.
 * @post None (no mutation).
 * @return Transfer-none pointer into @p app's object data (`"package"`,
 *   `"module"` or `"plugin"`), or NULL if never set (app is not ours).
 */
const gchar *gs_modulix_app_kind(GsApp *app) {
  return g_object_get_data(G_OBJECT(app), "modulix::kind");
}

/**
 * @brief Reads back the `"modulix::name"` object data set by
 *        gs_modulix_make_app_from_json()/gs_modulix_add_module_plugins().
 *
 * @param app GsApp to inspect. Not NULL.
 * @pre None.
 * @post None (no mutation).
 * @return Transfer-none pointer into @p app's object data (the nix
 *   attribute, module name, or `"<module>/<plugin>"` addon id), or NULL if
 *   never set (app is not ours).
 */
const gchar *gs_modulix_app_name(GsApp *app) {
  return g_object_get_data(G_OBJECT(app), "modulix::name");
}

/**
 * @brief Fetches @p module_name's plugin list and attaches it to
 *        @p module_app as ADDON GsApps.
 *
 * Fetches the module's plugin JSON via
 * gs_modulix_plugins_cache_get_or_fetch() (`gs-modulix-plugins-cache.h`,
 * not documented here), which returns a JSON string or NULL on failure.
 * The JSON is expected to decode, at its top level, to an array of plugin
 * objects (the `"plugins"` label passed to gs_modulix_json_parse_array()
 * is only used in its parse-error g_warning). Per plugin object:
 *
 *   - `"name"` — read via gs_modulix_json_str() (`""` default); a plugin
 *     with an empty name is skipped entirely (no addon GsApp is built for
 *     it).
 *   - `"description"` — read the same way, passed to
 *     gs_app_set_summary() verbatim (possibly empty).
 *   - `"installed"` — presence-checked via json_object_has_member(); when
 *     present, its boolean value is used. Unlike
 *     gs_modulix_make_app_from_json(), there is no caller-supplied
 *     fallback parameter here: an absent key defaults to FALSE
 *     unconditionally.
 *
 * Unlike gs_modulix_make_app_from_json(), each addon is *always* rebuilt
 * from the freshly-fetched JSON (cheap: it comes from the client-side
 * plugins cache, not a re-run of the daemon's namespace-wide `nix eval`) —
 * this is what lets `installed` refresh every time the details page
 * reopens, instead of freezing at whatever it was on first refine.
 *
 * Each addon's GsApp id is `"<module_name>/<plugin name>"`; the *plugin
 * cache* key (`gs_plugin_cache_lookup()`/`_add()`, same reuse pattern as
 * gs_modulix_make_app_from_json()) is `"plugin\x1f<id>"`, guarded by its
 * own function-local static GMutex distinct from the one in
 * gs_modulix_make_app_from_json(). Properties set per addon:
 * `management-plugin` = @p plugin; `name` at GS_APP_QUALITY_NORMAL =
 * plugin name; `summary` at GS_APP_QUALITY_NORMAL = description;
 * `kind` = AS_COMPONENT_KIND_ADDON; `state` = INSTALLED/AVAILABLE per
 * the resolved `installed` value, unless state_is_transient() says the
 * addon's current state must be left alone; `bundle-kind` =
 * AS_BUNDLE_KIND_PACKAGE. Object data: `"modulix::kind"` = `"plugin"`,
 * `"modulix::name"` = the addon id, `"modulix::parent"` = @p
 * module_name, `"modulix::plugin_name"` = the bare plugin name.
 *
 * @param module_app The module's GsApp to attach addons to. Not NULL.
 * @param module_name Module name: fetch key and addon-id prefix. Not
 *   NULL.
 * @param plugin GsPlugin used as each addon's management plugin and
 *   plugin-cache namespace. Not NULL.
 * @pre None.
 * @post Every named plugin of @p module_name has an up-to-date addon
 *   GsApp in the plugin cache. @b Quirk: gs_app_add_addons() is not
 *   idempotent — there is no public addon getter to de-dup against
 *   (`gs_app_dup_addons` lives in the unexported `gs-app-private.h`) — so
 *   this function only calls it the *first* time it finds at least one
 *   plugin for @p module_app (guarded by the `"modulix::addons-added"`
 *   object data on @p module_app); a later call with a different plugin
 *   set (e.g. a plugin added to the module since) will refresh each
 *   existing addon's own state but will not add any newly-appeared addon
 *   to @p module_app's addon list.
 * @return None. A NULL fetch, an unparsable/non-array JSON, or an empty
 *   plugin array leaves @p module_app unchanged.
 */
void gs_modulix_add_module_plugins(GsApp *module_app, const gchar *module_name,
                                   GsPlugin *plugin) {
  g_autofree gchar *json = gs_modulix_plugins_cache_get_or_fetch(module_name);
  if (json == NULL)
    return;

  g_autoptr(JsonParser) parser = json_parser_new();
  JsonArray *array = gs_modulix_json_parse_array(parser, json, "plugins");
  if (array == NULL)
    return;

  g_autoptr(GsAppList) addons = gs_app_list_new();
  static GMutex plugin_cache_key_mutex;
  for (guint i = 0; i < json_array_get_length(array); i++) {
    JsonObject *obj = json_array_get_object_element(array, i);
    const gchar *pname = gs_modulix_json_str(obj, "name");
    const gchar *desc = gs_modulix_json_str(obj, "description");
    if (!pname || !*pname)
      continue;
    gboolean installed = json_object_has_member(obj, "installed")
                             ? gs_modulix_json_bool(obj, "installed")
                             : FALSE;

    g_autofree gchar *id = g_strdup_printf("%s/%s", module_name, pname);
    g_autofree gchar *cache_key =
        g_strdup_printf("plugin\x1f%s", id);

    GsApp *addon;
    {
      g_autoptr(GMutexLocker) locker =
          g_mutex_locker_new(&plugin_cache_key_mutex);
      addon = gs_plugin_cache_lookup(plugin, cache_key);
      if (addon == NULL) {
        addon = gs_app_new(id);
        gs_plugin_cache_add(plugin, cache_key, addon);
      }
    }

    gs_app_set_management_plugin(addon, plugin);
    gs_app_set_name(addon, GS_APP_QUALITY_NORMAL, pname);
    gs_app_set_summary(addon, GS_APP_QUALITY_NORMAL, desc);
    gs_app_set_kind(addon, AS_COMPONENT_KIND_ADDON);
    if (!state_is_transient(gs_app_get_state(addon)))
      gs_app_set_state(addon, installed ? GS_APP_STATE_INSTALLED
                                        : GS_APP_STATE_AVAILABLE);
    gs_app_set_bundle_kind(addon, AS_BUNDLE_KIND_PACKAGE);
    g_object_set_data_full(G_OBJECT(addon), "modulix::kind", g_strdup("plugin"),
                           g_free);
    g_object_set_data_full(G_OBJECT(addon), "modulix::name", g_strdup(id),
                           g_free);
    g_object_set_data_full(G_OBJECT(addon), "modulix::parent",
                           g_strdup(module_name), g_free);
    g_object_set_data_full(G_OBJECT(addon), "modulix::plugin_name",
                           g_strdup(pname), g_free);
    gs_app_list_add(addons, addon);
    g_object_unref(addon);
  }

  if (gs_app_list_length(addons) > 0 &&
      !g_object_get_data(G_OBJECT(module_app), "modulix::addons-added")) {
    gs_app_add_addons(module_app, addons);
    g_object_set_data(G_OBJECT(module_app), "modulix::addons-added",
                      GINT_TO_POINTER(1));
  }
}

/**
 * @brief Appends every app decoded from a Modulix JSON `"apps"` array to
 *        @p list, deduplicating by GsApp id when @p seen_ids is given.
 *
 * @p json is expected to decode, at its top level, to a JSON array of app
 * objects (the `"apps"` label passed to gs_modulix_json_parse_array() is
 * only used in its parse-error g_warning, not looked up as a key). NULL,
 * empty, malformed or non-array JSON is tolerated: the function returns
 * without appending anything (a warning is logged for a genuine parse
 * failure by gs_modulix_json_parse_array()).
 *
 * Each array element is passed to gs_modulix_make_app_from_json() with
 * @p plugin/@p is_installed forwarded unchanged; a NULL result (missing
 * `"name"`) is silently skipped. When @p seen_ids is non-NULL, an app
 * whose gs_app_get_id() is already a member is unreffed and dropped
 * instead of being appended, and every newly-appended app's id is
 * g_strdup()'d into @p seen_ids — this lets a caller share one table
 * across several calls (e.g. modules-then-packages) so the
 * first-emitted entry for a given id wins over a later one, rather than
 * relying on gs_app_list_filter_duplicates()'s own first-wins pass.
 * @p seen_ids must be NULL on any path where entries intentionally
 * share one GsApp id (the `alternate_of`/Sources-popover path — see
 * CLAUDE.md), since deduplicating there would drop every variant but
 * the first.
 *
 * Module plugins (addons) are deliberately *not* attached here — see
 * gs_modulix_add_module_plugins(), called later during refine only for
 * the details page (`GS_PLUGIN_REFINE_REQUIRE_FLAGS_ADDONS`), since building
 * them costs a full-namespace `nix eval` per module.
 *
 * @param list GsAppList to append to. Not NULL. Mutated as described
 *   above.
 * @param json Raw JSON text for the `"apps"` array. Nullable.
 * @param plugin GsPlugin forwarded to gs_modulix_make_app_from_json() for
 *   every entry. Not NULL.
 * @param is_installed Fallback installed state forwarded to
 *   gs_modulix_make_app_from_json() for every entry.
 * @param parser JsonParser reused to decode @p json; its internal parse
 *   tree is overwritten by this call. Not NULL.
 * @param seen_ids Nullable GHashTable<owned gchar*, unused> for
 *   cross-call id dedup; see above. Not freed by this function.
 * @pre @p parser and @p seen_ids (when non-NULL) are valid.
 * @post @p list holds the newly-built, non-duplicate apps; @p seen_ids
 *   (when given) gained their ids.
 * @return None.
 */
void gs_modulix_append_apps_from_json(GsAppList *list, const gchar *json,
                                      GsPlugin *plugin, gboolean is_installed,
                                      JsonParser *parser,
                                      GHashTable *seen_ids) {
  JsonArray *array = gs_modulix_json_parse_array(parser, json, "apps");
  if (array == NULL)
    return;

  for (guint i = 0; i < json_array_get_length(array); i++) {
    JsonObject *obj = json_array_get_object_element(array, i);
    GsApp *app = gs_modulix_make_app_from_json(obj, plugin, is_installed);
    if (app == NULL)
      continue;
    if (seen_ids != NULL) {
      const gchar *id = gs_app_get_id(app);
      if (g_hash_table_contains(seen_ids, id)) {
        g_object_unref(app);
        continue;
      }
      g_hash_table_add(seen_ids, g_strdup(id));
    }
    gs_app_list_add(list, app);
    g_object_unref(app);
  }
}
