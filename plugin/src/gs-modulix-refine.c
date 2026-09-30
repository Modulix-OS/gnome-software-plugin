/**
 * @file gs-modulix-refine.c
 * @brief The `refine` vfunc body: addons, icons, descriptions, licenses and
 * Flathub enrichment for the apps this plugin manages.
 *
 * Refine is GNOME Software's fill-in-the-missing-metadata pass: it runs
 * often, always over a batch of `GsApp` rather than one app at a time (once
 * per page load/refresh, over the whole result list), and must only fill in
 * the metadata named by the caller's `GsPluginRefineRequireFlags` — see
 * `MODULIX_REFINE_FLAGS_WE_HANDLE` below for exactly which bits this plugin
 * honours; every other bit, and every app this plugin does not own, is left
 * untouched.
 *
 * Two batched prefetches run before the per-app pass, so the per-app work is
 * pure cache reads:
 *   - enrichment: one batched `GetAppEnrichment` daemon round-trip
 *     for every distinct app-id of the list (see prefetch_enrichment());
 *     an app-id the daemon has no data for is simply absent from the reply,
 *     which is not an error — the per-app step just finds no cache entry
 *     for it,
 *   - licenses (one daemon call, details page only — it costs a `nix eval`
 *     per attribute daemon-side).
 *
 * All of the per-app work below (and both prefetches) run on the `GTask`
 * worker thread spawned by gs_modulix_refine_async() — never on the
 * GNOME Software main thread — except for the up-front
 * `MODULIX_REFINE_FLAGS_WE_HANDLE` check, which runs synchronously on the
 * caller's thread so a request this plugin cannot help with never pays for
 * a thread hop.
 *
 * Missing metadata (no enrichment for an id, no license resolved, no icon
 * past the generic fallback, …) is never treated as an error anywhere in
 * this file: the corresponding `GsApp` field is simply left unset, and the
 * refine operation still completes successfully. The only failure mode
 * surfaced through gs_modulix_refine_finish() is cancellation
 * (`GCancellable`) mid-batch.
 */

#include "gs-modulix-refine.h"

#include "gs-modulix-app.h"
#include "gs-modulix-enrichment-cache.h"
#include "gs-modulix-icon-cache.h"
#include "gs-modulix-icon-resolver.h"
#include "gs-modulix-markup.h"

#include "dbus/gs-modulix-store1.h"

/**
 * @brief Per-task state for one gs_modulix_refine_async() invocation,
 * carried as the `GTask`'s task data.
 *
 * @var RefineData::plugin
 *   The owning `GsPlugin`. Borrowed: the `GTask`'s source object keeps it
 *   alive for at least the lifetime of this struct, so no reference is
 *   taken here.
 * @var RefineData::list
 *   The batch of apps being refined. Owned: one reference taken in
 *   gs_modulix_refine_async(), released by refine_data_free().
 * @var RefineData::licenses
 *   Owned `GHashTable` mapping nix attribute (owned `gchar *` key) to SPDX
 *   license expression (owned `gchar *` value). NULL unless the current
 *   refine is a details-page refine (LICENSE + ADDONS both set), in which
 *   case it is allocated in refine_thread() and filled by
 *   prefetch_licenses(); an attribute prefetch_licenses() found no license
 *   for is simply absent as a key, not an error.
 * @var RefineData::installed_plugins
 *   Owned `GHashTable` (set semantics: `g_free` key, no value) of
 *   `"<module>/<name>"` ids currently installed, covering every distinct
 *   parent module of a plugin (addon) app in RefineData::list. Allocated
 *   and filled in refine_thread() by prefetch_installed_plugins() whenever
 *   the list holds at least one plugin app; NULL otherwise.
 */
typedef struct {
  GsPlugin *plugin; /* borrowed: the task's source object outlives the data */
  GsAppList *list;
  GsPluginRefineRequireFlags require_flags;
  /* nix attribute (owned) -> SPDX expression (owned), filled by
   * prefetch_licenses() on the details-page refine only. */
  GHashTable *licenses;
  /* "<module>/<name>" (owned) set, filled by prefetch_installed_plugins(). */
  GHashTable *installed_plugins;
} RefineData;

/**
 * @brief `GDestroyNotify` for #RefineData: releases the list reference and
 * the licenses table, then frees the struct itself.
 * @param d The #RefineData to free. Must not be NULL; after this call, @p d
 *   is no longer valid.
 * @pre @p d was allocated with `g_new0(RefineData, 1)` and installed as a
 *   `GTask`'s task data (or is otherwise solely owned by the caller).
 * @post @p d->list is unreffed (if non-NULL), @p d->licenses and
 *   @p d->installed_plugins are unreffed (if non-NULL, freeing every owned
 *   key/value they held), and @p d itself is freed. Nothing is returned.
 */
static void refine_data_free(RefineData *d) {
  g_clear_object(&d->list);
  g_clear_pointer(&d->licenses, g_hash_table_unref);
  g_clear_pointer(&d->installed_plugins, g_hash_table_unref);
  g_free(d);
}

/**
 * @brief Reads the AppStream component id this plugin stashed on @p app.
 * @param app The `GsApp` to inspect. Must not be NULL.
 * @pre None beyond @p app being a valid `GsApp`; this accessor does not
 *   check `gs_modulix_app_is_ours()` itself.
 * @return (transfer none) (nullable): the id set as `"modulix::app_id"`
 *   object data, or NULL when the app carries none (e.g. a nix package with
 *   no matching Flathub/AppStream entry, or an app this plugin did not
 *   create). The returned string is owned by @p app; the caller must not
 *   free it and must not use it past @p app's lifetime.
 */
static const gchar *app_id_of(GsApp *app) {
  return g_object_get_data(G_OBJECT(app), "modulix::app_id");
}

/**
 * @brief Reads the owning module this plugin stashed on a plugin (addon)
 * @p app.
 * @param app The `GsApp` to inspect. Must not be NULL.
 * @pre None beyond @p app being a valid `GsApp`.
 * @return (transfer none) (nullable): the module name set as
 *   `"modulix::parent"` object data, or NULL for an app that is not a
 *   module-plugin row. Owned by @p app; the caller must not free it.
 */
static const gchar *parent_of(GsApp *app) {
  return g_object_get_data(G_OBJECT(app), "modulix::parent");
}

/* ── per-app refine steps ───────────────────────────────────────────────── */

/**
 * @brief Attaches a module's plugins as ADDON addons, honouring the ADDONS
 * require flag only.
 *
 * Addons (module plugins) are only needed by the details page
 * (GS_PLUGIN_REFINE_REQUIRE_FLAGS_ADDONS); building them costs a
 * full-namespace `nix eval`, so it must never happen on the search path.
 *
 * @param app The app to add addons to. Must not be NULL.
 * @param require_flags Caller's `GsPluginRefineRequireFlags`. Only
 *   `GS_PLUGIN_REFINE_REQUIRE_FLAGS_ADDONS` is consulted; every other bit is
 *   ignored by this function.
 * @param plugin The owning `GsPlugin`, forwarded to
 *   gs_modulix_add_module_plugins() for cache/ownership checks.
 * @pre None beyond @p app, @p plugin being valid.
 * @post No-op unless ADDONS is requested and @p app's `modulix::kind` is
 *   `"module"` with a non-empty name; otherwise
 *   gs_modulix_add_module_plugins() is called, which is itself guarded
 *   against re-adding addons to an app already carrying them (see
 *   `modulix::addons-added` in gs-modulix-app.c). Nothing is returned; a
 *   module with no resolvable plugins list simply gets no addons, which is
 *   not an error.
 */
static void refine_addons(GsApp *app, GsPluginRefineRequireFlags require_flags,
                          GsPlugin *plugin) {
  if (!(require_flags & GS_PLUGIN_REFINE_REQUIRE_FLAGS_ADDONS))
    return;
  if (g_strcmp0(gs_modulix_app_kind(app), "module") != 0)
    return;

  const gchar *name = gs_modulix_app_name(app);
  if (name == NULL || *name == '\0')
    return;

  gint64 t0 = g_get_monotonic_time();
  gs_modulix_add_module_plugins(app, name, plugin);
  g_debug("[modulix] add_module_plugins(%s) %.0fms", name,
          (g_get_monotonic_time() - t0) / 1000.0);
}

/**
 * @brief Resolves and attaches an icon for @p app, honouring the ICON
 * require flag only.
 *
 * ICON refine is local-only (theme/file-cache lookups, no network): handled
 * unconditionally here rather than folded into the DESCRIPTION/SCREENSHOTS
 * Flathub-fetch path. Only replayed when the app still has no icon at all —
 * e.g. an `alternate_of` row, whose backend entry carries `icon: None` (see
 * CLAUDE.md "Icons").
 *
 * @param app The app to attach an icon to. Must not be NULL.
 * @param require_flags Caller's `GsPluginRefineRequireFlags`. Only
 *   `GS_PLUGIN_REFINE_REQUIRE_FLAGS_ICON` is consulted here.
 * @param app_id (nullable): the app's AppStream id (as returned by
 *   app_id_of()), forwarded to gs_modulix_icon_resolve() for its
 *   flatpak-appstream/remote-icon-cache lookups. NULL is accepted — the
 *   resolver falls back to its other lookup keys.
 * @pre None beyond @p app being valid.
 * @post No-op unless ICON is requested and @p app has no icon yet
 *   (`gs_app_has_icons()`). Otherwise gs_modulix_icon_resolve() is called,
 *   which always attaches exactly one `GIcon` (never zero — it falls back
 *   to a generic icon), and on success this function additionally marks
 *   @p app with `"modulix::icon-themed"` object data so
 *   refine_from_enrichment() knows not to add a competing remote icon
 *   later. Nothing is returned.
 */
static void refine_icon(GsApp *app, GsPluginRefineRequireFlags require_flags,
                        const gchar *app_id) {
  if (!(require_flags & GS_PLUGIN_REFINE_REQUIRE_FLAGS_ICON))
    return;
  if (gs_app_has_icons(app))
    return;

  const gchar *icon_name = g_object_get_data(G_OBJECT(app), "modulix::icon_name");
  const gchar *base_name = g_object_get_data(G_OBJECT(app), "modulix::base_name");
  const gchar *icon = g_object_get_data(G_OBJECT(app), "modulix::icon");

  if (gs_modulix_icon_resolve(app, app_id, icon_name, gs_modulix_app_name(app),
                              base_name, icon))
    g_object_set_data(G_OBJECT(app), "modulix::icon-themed", GINT_TO_POINTER(1));
}

/**
 * @brief Seeds @p app's description from its summary at
 * `GS_APP_QUALITY_LOWEST`, so an app with no Flathub match still passes the
 * Installed page's "has a description" gate.
 *
 * A NULL description is enough on its own to hide an app: the Installed
 * page drops every row that has none (`gs_installed_page_is_actual_app`,
 * src/gs-installed-page.c). Ours only ever comes from Flathub enrichment,
 * which the early returns in refine_one() skip entirely for an app with no
 * app_id — so a nix package with no Flatpak counterpart vanished from the
 * Installed list altogether. nixpkgs has nothing better to offer either
 * (`meta.longDescription` is empty for these attributes), so seed the
 * summary at LOWEST quality: `gs_app_set_description` (lib/gs-app.c)
 * ignores a write of lower quality than the one stored, so the Flathub
 * description set at NORMAL still wins whenever there is one.
 *
 * @param app The app to seed a description on. Must not be NULL.
 * @param flags Caller's `GsPluginRefineRequireFlags`. Only
 *   `GS_PLUGIN_REFINE_REQUIRE_FLAGS_DESCRIPTION` is consulted, and only to
 *   decide whether an already-present description may be left alone; this
 *   function still runs (as a fallback seed) when DESCRIPTION is not
 *   requested but @p app currently has no description at all.
 * @pre None beyond @p app being valid.
 * @post No-op when DESCRIPTION was requested and @p app already has a
 *   description, or when @p app's summary is NULL/empty. Otherwise
 *   `gs_app_set_description()` is called at `GS_APP_QUALITY_LOWEST` with
 *   the summary, markup-escaped so a bare `&`/`<` cannot break the Pango
 *   markup renderer. A later higher-quality write (e.g. Flathub's, at
 *   NORMAL) still overrides this one. Nothing is returned; there is no
 *   error case.
 */
static void seed_description_from_summary(GsApp *app,
                                          GsPluginRefineRequireFlags flags) {
  if (!(flags & GS_PLUGIN_REFINE_REQUIRE_FLAGS_DESCRIPTION) &&
      gs_app_get_description(app) != NULL)
    return;

  const gchar *summary = gs_app_get_summary(app);
  if (summary == NULL || *summary == '\0')
    return;

  /* Unlike the summary, the description is rendered as Pango markup
   * (gs_description_box_set_text -> gtk_label_set_markup), so a bare `&` or
   * `<` in meta.description -- common enough -- would fail the markup parser
   * and blank the whole block. The Flathub description takes the same route
   * through gs_modulix_html_to_pango(). */
  g_autofree gchar *escaped = g_markup_escape_text(summary, -1);
  gs_app_set_description(app, GS_APP_QUALITY_LOWEST, escaped);
}

/**
 * @brief Applies the nixpkgs `meta.license` of the attribute @p app would
 * install, honouring the LICENSE require flag only.
 *
 * Set at HIGHEST quality because it describes the exact build the user
 * gets, and must therefore override a Flathub license a previous (search
 * page) refine of the *same* GsApp may already have set at NORMAL.
 * @p licenses is only populated on the details-page refine — see
 * prefetch_licenses().
 *
 * @param app The app to set the license on. Must not be NULL.
 * @param flags Caller's `GsPluginRefineRequireFlags`. Only
 *   `GS_PLUGIN_REFINE_REQUIRE_FLAGS_LICENSE` is consulted.
 * @param licenses (nullable): nix attribute → SPDX expression map (both
 *   borrowed from the table's own storage), as filled by
 *   prefetch_licenses(); NULL on a non-details-page refine, in which case
 *   this function always returns %FALSE without touching @p app.
 * @pre None beyond @p app being valid.
 * @post No-op (returns %FALSE) when LICENSE was not requested, @p licenses
 *   is NULL, @p app has no nix attribute name, or that attribute is absent
 *   from @p licenses — the last case is not an error, just "nothing to
 *   apply yet" (the Flathub fallback in refine_from_enrichment() covers
 *   it). Otherwise `gs_app_set_license()` is called at
 *   `GS_APP_QUALITY_HIGHEST`.
 * @return %TRUE when the nix license was applied (i.e. the Flathub fallback
 *   must not overwrite it), %FALSE otherwise.
 */
static gboolean refine_license_from_nix(GsApp *app,
                                        GsPluginRefineRequireFlags flags,
                                        GHashTable *licenses) {
  if (!(flags & GS_PLUGIN_REFINE_REQUIRE_FLAGS_LICENSE) || licenses == NULL)
    return FALSE;

  const gchar *attr = gs_modulix_app_name(app);
  const gchar *license =
      attr != NULL ? g_hash_table_lookup(licenses, attr) : NULL;
  if (license == NULL)
    return FALSE;

  gs_app_set_license(app, GS_APP_QUALITY_HIGHEST, license);
  return TRUE;
}

/**
 * @brief Applies the Flathub enrichment payload for @p app_id: description,
 * screenshots, icon URL and the fallback `project_license` (modules, and
 * attributes `nix eval` could not resolve).
 *
 * Reads the enrichment cache rather than fetching directly, so this call
 * costs a daemon round-trip only on the first miss of a given @p app_id —
 * the batch's ids are normally already warmed by prefetch_enrichment()
 * before refine_one() reaches this call. An @p app_id the daemon has no
 * enrichment for is not an error: the cache holds
 * #GS_MODULIX_ENRICHMENT_EMPTY for it and this function simply returns
 * without changing @p app.
 *
 * @param app The app to enrich. Must not be NULL.
 * @param app_id The AppStream component id to fetch enrichment for. Must be
 *   non-NULL and non-empty (callers already guard this — see refine_one()).
 * @param license_set Whether refine_license_from_nix() already set a
 *   HIGHEST-quality license on @p app; when %TRUE, the enrichment's
 *   `project_license` is not applied, since `gs_app_set_license()` would
 *   reject the lower-quality write anyway and there is no need to pay for
 *   the JSON lookup.
 * @pre None beyond @p app, @p app_id being valid as described above.
 * @post No-op if the cache has no data for @p app_id. Otherwise: license is
 *   set at `GS_APP_QUALITY_NORMAL` when present and not already
 *   @p license_set; description is set at NORMAL (HTML converted to Pango
 *   markup) when present; icon URL, when present, is always cached via
 *   gs_modulix_icon_cache_put() for reuse by other instances of the same
 *   app, but a new `GsRemoteIcon` is only built and added to @p app itself
 *   (gs_app_add_icon() takes its own reference; the local `g_autoptr` owner
 *   still unrefs it afterwards) when @p app has not already won a themed
 *   icon (`"modulix::icon-themed"` object data unset) — otherwise adding it
 *   would let a later-completing download override the themed icon.
 *   Screenshots are added unconditionally via
 *   gs_modulix_add_app_screenshots(). Nothing is returned.
 */
static void refine_from_enrichment(GsApp *app, const gchar *app_id,
                                   gboolean license_set) {
  gint64 t0 = g_get_monotonic_time();
  g_autoptr(GVariant) obj = gs_modulix_enrichment_cache_get_or_fetch(app_id);
  g_debug("[modulix] enrichment(%s) %.0fms", app_id,
          (g_get_monotonic_time() - t0) / 1000.0);
  if (obj == NULL)
    return;

  const gchar *html_desc = NULL, *icon = NULL, *license = NULL;
  g_variant_lookup(obj, "description", "&s", &html_desc);
  g_variant_lookup(obj, "icon", "&s", &icon);
  g_variant_lookup(obj, "license", "&s", &license);

  if (!license_set && license && *license)
    gs_app_set_license(app, GS_APP_QUALITY_NORMAL, license);

  if (html_desc && *html_desc) {
    g_autofree gchar *pango = gs_modulix_html_to_pango(html_desc);
    if (pango && *pango)
      gs_app_set_description(app, GS_APP_QUALITY_NORMAL, pango);
  }

  if (icon && *icon) {
    gs_modulix_icon_cache_put(app_id, icon);
    /* Once a themed (Papirus/…) icon has won at listing time, adding this
     * remote one would let gs_app_get_icon_for_size()'s first (sized-icon)
     * pass override it as soon as the download completes — repaint the
     * enrichment icon URL into the cache for other instances, but don't
     * re-add it to this app. */
    if (!g_object_get_data(G_OBJECT(app), "modulix::icon-themed")) {
      g_autoptr(GIcon) remote = gs_modulix_remote_icon_new(icon);
      gs_app_add_icon(app, remote);
    }
  }

  gs_modulix_add_app_screenshots(app, obj);
}

/**
 * @brief Decides whether refine_one() must call refine_from_enrichment()
 * for the current app.
 *
 * The enrichment payload carries the fallback license too, so a
 * LICENSE-only refine still has to fetch it — unless the nixpkgs license
 * already won.
 *
 * @param flags Caller's `GsPluginRefineRequireFlags`. Consults DESCRIPTION,
 *   SCREENSHOTS and LICENSE; every other bit is irrelevant to this
 *   decision.
 * @param license_set Whether a HIGHEST-quality nix license was already
 *   applied by refine_license_from_nix() for this app.
 * @pre None.
 * @return %TRUE when DESCRIPTION or SCREENSHOTS is requested, or when
 *   LICENSE is requested and @p license_set is %FALSE; %FALSE otherwise (no
 *   error case — a %FALSE result simply means enrichment is skipped for
 *   this app on this refine).
 */
static gboolean want_enrichment(GsPluginRefineRequireFlags flags,
                                gboolean license_set) {
  if (flags & (GS_PLUGIN_REFINE_REQUIRE_FLAGS_DESCRIPTION |
               GS_PLUGIN_REFINE_REQUIRE_FLAGS_SCREENSHOTS))
    return TRUE;
  return (flags & GS_PLUGIN_REFINE_REQUIRE_FLAGS_LICENSE) && !license_set;
}

/**
 * @brief Runs every per-app refine step for one `GsApp`, in the order that
 * makes each later step's precondition hold (icon before enrichment's
 * icon-themed check, license-from-nix before want_enrichment()'s license
 * check, …).
 *
 * @param app The app to refine. Must not be NULL; must already be confirmed
 *   as owned by this plugin (`gs_modulix_app_is_ours()`) by the caller —
 *   this function does not check it itself.
 * @param require_flags Caller's `GsPluginRefineRequireFlags`, forwarded
 *   unchanged to every step. Only the flags each step individually
 *   documents are honoured overall (ADDONS, ICON, DESCRIPTION, LICENSE,
 *   SCREENSHOTS); any other bit is ignored.
 * @param plugin The owning `GsPlugin`, forwarded to refine_addons().
 * @param licenses (nullable): nix attribute → SPDX map from
 *   prefetch_licenses(), forwarded to refine_license_from_nix(); NULL
 *   outside a details-page refine.
 * @param installed_plugins (nullable): forwarded to refine_plugin() for an
 *   app whose `modulix::kind` is `"plugin"`.
 * @pre None beyond @p app, @p plugin being valid.
 * @post Delegates to refine_plugin() for an app whose `modulix::kind` is
 *   `"plugin"` (addon rows use a dedicated, smaller refine path — see
 *   refine_plugin()). Otherwise runs addons, icon, description-seed and
 *   license-from-nix unconditionally (each
 *   internally gated on its own flag), then calls
 *   refine_from_enrichment() only when want_enrichment() says so AND
 *   @p app has a non-empty app-id — an app with no app-id (no Flathub
 *   counterpart) never reaches the enrichment step, which is why
 *   seed_description_from_summary() exists. Nothing is returned; every
 *   step already treats missing data as "leave unset", not as an error.
 */
/**
 * @brief Refines a module-plugin (addon) `GsApp`: seeds its icon and
 * refreshes its installed state from the parent module's currently
 * installed-plugins set.
 *
 * Addon rows built by gs_modulix_add_module_plugins() are already
 * up-to-date at creation time, but a plugin's addon `GsApp` is also reached
 * standalone — from the "Installed" page's Add-ons listing
 * (gs-modulix-list.c's collect_plugins_into()) — where nothing else
 * refreshes it afterwards. Without this step its state froze at whatever it
 * was when first built, so a later install/uninstall done from the parent
 * module's details page, or from the `mx` CLI, never showed up here.
 *
 * @param app The plugin (addon) app to refine. Must not be NULL; must have
 *   `modulix::kind` `"plugin"`.
 * @param require_flags Caller's `GsPluginRefineRequireFlags`. Only ICON is
 *   consulted, same rule as refine_icon().
 * @param installed_plugins (nullable): set of currently installed
 *   `"<module>/<name>"` ids, from prefetch_installed_plugins(); NULL (or a
 *   miss) is treated as "not installed" for this app, deferring to
 *   whatever state @p app already carries under state_is_transient()'s
 *   guard.
 * @pre None beyond @p app being valid.
 * @post @p app's icon is resolved when missing and ICON is requested; its
 *   state is refreshed unless state_is_transient() says it must be left
 *   alone. Nothing is returned.
 */
static void refine_plugin(GsApp *app, GsPluginRefineRequireFlags require_flags,
                          GHashTable *installed_plugins) {
  refine_icon(app, require_flags, NULL);

  const gchar *id = gs_modulix_app_name(app);
  if (id == NULL || *id == '\0' ||
      gs_modulix_state_is_transient(gs_app_get_state(app)))
    return;

  gboolean installed =
      installed_plugins != NULL && g_hash_table_contains(installed_plugins, id);
  gs_app_set_state(app,
                   installed ? GS_APP_STATE_INSTALLED : GS_APP_STATE_AVAILABLE);
}

static void refine_one(GsApp *app, GsPluginRefineRequireFlags require_flags,
                       GsPlugin *plugin, GHashTable *licenses,
                       GHashTable *installed_plugins) {
  const gchar *app_id = app_id_of(app);
  const gchar *kind = gs_modulix_app_kind(app);

  if (g_strcmp0(kind, "plugin") == 0) {
    refine_plugin(app, require_flags, installed_plugins);
    return;
  }

  /* Synthetic rows: the system-update line and the release-upgrade banner.
   * Both carry everything they need from the start, and the distro-upgrade
   * job refines its list with REQUIRE_FLAGS_SETUP_ACTION on every visit to
   * the Updates page. */
  if (g_strcmp0(kind, "update") == 0 || g_strcmp0(kind, "upgrade") == 0)
    return;

  refine_addons(app, require_flags, plugin);
  refine_icon(app, require_flags, app_id);
  seed_description_from_summary(app, require_flags);

  gboolean license_set = refine_license_from_nix(app, require_flags, licenses);

  if (!want_enrichment(require_flags, license_set))
    return;
  if (app_id == NULL || *app_id == '\0')
    return;

  refine_from_enrichment(app, app_id, license_set);
}

/* ── batched prefetches ─────────────────────────────────────────────────── */

/**
 * @brief Collects the distinct, non-empty values @p key_fn returns over the
 * apps of @p list this plugin manages, restricted to those whose kind
 * passes @p kind_filter.
 *
 * Used by both batched prefetches (prefetch_enrichment(),
 * prefetch_licenses()) to turn a `GsAppList` into the deduplicated id/attr
 * array their one-shot daemon calls need.
 *
 * @param list The apps to scan. Must not be NULL.
 * @param plugin The owning `GsPlugin`, forwarded to
 *   `gs_modulix_app_is_ours()` to filter out apps this plugin does not
 *   manage.
 * @param kind_filter (nullable): when non-NULL, only apps whose
 *   `modulix::kind` equals this string are considered; when NULL, every
 *   kind except `"plugin"` is considered.
 * @param key_fn Accessor called on each surviving app to obtain the value
 *   to collect; an app for which it returns NULL or the empty string is
 *   skipped.
 * @pre None beyond @p list, @p plugin, @p key_fn being valid.
 * @post No mutation of @p list or its apps.
 * @return (transfer container) (element-type utf8): a newly allocated
 *   `GPtrArray` the caller must free with `g_ptr_array_unref()` (or
 *   `g_autoptr`); its elements are `const gchar *` strings borrowed from
 *   the apps of @p list (via @p key_fn) and from an internal deduplication
 *   table — they are only valid as long as @p list and its apps are, and
 *   must not be freed individually. Possibly empty, never NULL.
 */
static GPtrArray *collect_distinct(GsAppList *list, GsPlugin *plugin,
                                   const gchar *kind_filter,
                                   const gchar *(*key_fn)(GsApp *app)) {
  GPtrArray *keys = g_ptr_array_new(); /* borrowed const gchar* */
  g_autoptr(GHashTable) seen =
      g_hash_table_new(g_str_hash, g_str_equal); /* borrowed keys */

  for (guint i = 0; i < gs_app_list_length(list); i++) {
    GsApp *app = gs_app_list_index(list, i);
    if (!gs_modulix_app_is_ours(app, plugin))
      continue;

    const gchar *kind = gs_modulix_app_kind(app);
    if (kind_filter != NULL ? g_strcmp0(kind, kind_filter) != 0
                            : g_strcmp0(kind, "plugin") == 0)
      continue;

    const gchar *key = key_fn(app);
    if (key == NULL || *key == '\0')
      continue;
    if (g_hash_table_add(seen, (gpointer)key))
      g_ptr_array_add(keys, (gpointer)key);
  }

  return keys;
}

/**
 * @brief Warms the enrichment cache for every distinct app-id of @p list
 * this plugin manages, in one batched backend call.
 *
 * This is the batched round-trip the class-level documentation refers to:
 * it fetches every id at once through
 * gs_modulix_enrichment_cache_prefetch_many(), which itself makes exactly
 * one batched `GetAppEnrichment` daemon call for the whole list, so
 * the per-app gs_modulix_enrichment_cache_get_or_fetch() calls later in
 * refine_from_enrichment() become cache hits (or cached
 * #GS_MODULIX_ENRICHMENT_EMPTY misses) instead of N individual daemon
 * round-trips.
 *
 * @param list The batch being refined. Must not be NULL.
 * @param plugin The owning `GsPlugin`, forwarded to collect_distinct().
 * @pre Intended to be called once per refine_thread() invocation, before
 *   the per-app loop.
 * @post No daemon call is made when @p list has no distinct app-ids to
 *   prefetch (an empty list, or one with no app_id anywhere). Nothing is
 *   returned; an id the daemon has no enrichment for is not an error, it is
 *   simply cached as empty.
 */
static void prefetch_enrichment(GsAppList *list, GsPlugin *plugin) {
  g_autoptr(GPtrArray) ids = collect_distinct(list, plugin, NULL, app_id_of);

  if (ids->len > 0)
    gs_modulix_enrichment_cache_prefetch_many((const gchar *const *)ids->pdata,
                                              ids->len);
}

/**
 * @brief Fills @p licenses (nix attr → SPDX, both owned) for every nix
 * package of @p list this plugin manages, in one batched daemon call.
 *
 * Only ever called on a details-page refine (see refine_thread()): the
 * daemon runs one `nix eval` per uncached attribute, which the search page
 * — which also asks for LICENSE, over its whole result list — must never
 * trigger. Search results keep the Flathub license from the enrichment
 * payload instead. Modules and module plugins have no nixpkgs attribute to
 * evaluate, so they are excluded from the `"package"` kind filter passed to
 * collect_distinct().
 *
 * @param list The batch being refined. Must not be NULL.
 * @param plugin The owning `GsPlugin`, forwarded to collect_distinct().
 * @param licenses (transfer none): destination map, owned by the caller
 *   (RefineData::licenses); entries are inserted with newly `g_strdup()`ed
 *   key and value, so this function's writes are independent of the
 *   lifetime of @p list's strings.
 * @pre @p licenses is a valid, already-allocated `GHashTable` with
 *   `g_free` key/value destroy functions (see refine_thread()).
 * @post Returns early, leaving @p licenses unchanged, when @p list has no
 *   distinct nix package attributes, or when the daemon call
 *   (`GetPackageLicenses`) fails — treated as "no licenses available", not
 *   as an error surfaced to the caller. An attribute for which the daemon
 *   returned no license (or an empty one) is simply omitted from
 *   @p licenses. Nothing is returned.
 */
static void prefetch_licenses(GsAppList *list, GsPlugin *plugin,
                              GHashTable *licenses) {
  g_autoptr(GPtrArray) attrs =
      collect_distinct(list, plugin, "package", gs_modulix_app_name);
  if (attrs->len == 0)
    return;

  gint64 t0 = g_get_monotonic_time();
  g_autoptr(GVariant) result = gs_modulix_store1_get_package_licenses(
      (const gchar *const *)attrs->pdata, attrs->len);
  g_debug("[modulix] licenses(n=%u) %.0fms", attrs->len,
          (g_get_monotonic_time() - t0) / 1000.0);
  if (result == NULL)
    return;

  GVariantIter iter;
  g_variant_iter_init(&iter, result);
  const gchar *attr, *license;
  while (g_variant_iter_loop(&iter, "{&s&s}", &attr, &license)) {
    if (license != NULL && *license != '\0')
      g_hash_table_insert(licenses, g_strdup(attr), g_strdup(license));
  }
}

/**
 * @brief Fills @p installed_plugins (`"<module>/<name>"` set) for every
 * distinct parent module of a plugin (addon) app in @p list, in one
 * `ListInstalledPlugins` daemon call per module.
 *
 * No `nix eval` runs on this path (see `ListInstalledPlugins` in
 * modulix-daemon): each call is a config-derived, 5s-cached lookup.
 *
 * @param list The batch being refined. Must not be NULL.
 * @param plugin The owning `GsPlugin`, forwarded to collect_distinct().
 * @param installed_plugins (transfer none): destination set, owned by the
 *   caller (RefineData::installed_plugins); entries are inserted with a
 *   newly `g_strdup()`ed key, independent of @p list's own strings.
 * @pre @p installed_plugins is a valid, already-allocated `GHashTable` with
 *   a `g_free` key destroy function and no value destroy function (see
 *   refine_thread()).
 * @post Returns early, leaving @p installed_plugins unchanged, when @p list
 *   has no plugin app. A module whose daemon call fails simply contributes
 *   nothing — its plugins are left absent from @p installed_plugins, i.e.
 *   treated as not installed. Nothing is returned.
 */
static void prefetch_installed_plugins(GsAppList *list, GsPlugin *plugin,
                                       GHashTable *installed_plugins) {
  g_autoptr(GPtrArray) modules =
      collect_distinct(list, plugin, "plugin", parent_of);
  if (modules->len == 0)
    return;

  for (guint i = 0; i < modules->len; i++) {
    const gchar *module = g_ptr_array_index(modules, i);

    gint64 t0 = g_get_monotonic_time();
    g_autoptr(GVariant) array =
        gs_modulix_store1_list_installed_plugins(module);
    g_debug("[modulix] list_installed_plugins(%s) %.0fms", module,
            (g_get_monotonic_time() - t0) / 1000.0);
    if (array == NULL)
      continue;

    GVariantIter iter;
    g_variant_iter_init(&iter, array);
    GVariant *dict;
    while ((dict = g_variant_iter_next_value(&iter)) != NULL) {
      const gchar *name = NULL;
      g_variant_lookup(dict, "name", "&s", &name);
      if (name != NULL && *name != '\0')
        g_hash_table_add(installed_plugins,
                         g_strdup_printf("%s/%s", module, name));
      g_variant_unref(dict);
    }
  }
}

/* ── vfunc ──────────────────────────────────────────────────────────────── */

#define MODULIX_REFINE_FLAGS_WE_HANDLE                                         \
  (GS_PLUGIN_REFINE_REQUIRE_FLAGS_DESCRIPTION |                                \
   GS_PLUGIN_REFINE_REQUIRE_FLAGS_SCREENSHOTS |                                \
   GS_PLUGIN_REFINE_REQUIRE_FLAGS_ADDONS |                                     \
   GS_PLUGIN_REFINE_REQUIRE_FLAGS_LICENSE |                                    \
   GS_PLUGIN_REFINE_REQUIRE_FLAGS_ICON)

/**
 * @brief Worker body of a refine pass: prefetches in batch, then refines each
 *        owned app.
 *
 * @param task The GTask to complete. Not NULL.
 * @param source_object Unused.
 * @param task_data_ptr The pass's RefineData: plugin, app list, require flags,
 *   license table and installed-plugins set. Not NULL.
 * @param cancellable Cancellable checked before each app, or NULL.
 * @pre Runs on a GTask worker thread, never on the main thread: it blocks on the
 *   daemon.
 * @post Enrichment is prefetched in a single batched round-trip when the
 *   description or the screenshots are wanted. Licenses are prefetched only when
 *   ADDONS is also requested, i.e. only for the details page, since each one
 *   costs a `nix eval` on the daemon. Installed plugins are prefetched
 *   unconditionally, one cheap config-derived call per distinct parent module
 *   of a plugin app in the list (see prefetch_installed_plugins()), so a
 *   plugin's state stays correct wherever its addon GsApp is refined. Apps of
 *   other plugins are skipped. On cancellation the task fails with the
 *   cancellation error and the apps already refined keep what they got.
 * @return None.
 */
static void refine_thread(GTask *task, gpointer source_object G_GNUC_UNUSED,
                          gpointer task_data_ptr, GCancellable *cancellable) {
  RefineData *data = task_data_ptr;
  GsAppList *list = data->list;

  if (data->require_flags & (GS_PLUGIN_REFINE_REQUIRE_FLAGS_DESCRIPTION |
                             GS_PLUGIN_REFINE_REQUIRE_FLAGS_SCREENSHOTS))
    prefetch_enrichment(list, data->plugin);

  /* ADDONS is the details page's own marker (the search page never asks for
   * it), and the details page is the only caller allowed to pay for a per-
   * attribute `nix eval` — see prefetch_licenses(). */
  if ((data->require_flags & GS_PLUGIN_REFINE_REQUIRE_FLAGS_LICENSE) &&
      (data->require_flags & GS_PLUGIN_REFINE_REQUIRE_FLAGS_ADDONS)) {
    data->licenses =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    prefetch_licenses(list, data->plugin, data->licenses);
  }

  data->installed_plugins = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, NULL);
  prefetch_installed_plugins(list, data->plugin, data->installed_plugins);

  for (guint i = 0; i < gs_app_list_length(list); i++) {
    g_autoptr(GError) err = NULL;
    if (g_cancellable_set_error_if_cancelled(cancellable, &err)) {
      g_task_return_error(task, g_steal_pointer(&err));
      return;
    }
    GsApp *app = gs_app_list_index(list, i);
    if (gs_modulix_app_is_ours(app, data->plugin))
      refine_one(app, data->require_flags, data->plugin, data->licenses,
                data->installed_plugins);
  }
  g_task_return_boolean(task, TRUE);
}

/**
 * @brief Starts a refine pass over @p list.
 *
 * @param plugin Plugin whose apps are to be refined; also the GTask's source
 *   object. Not NULL.
 * @param list Apps GNOME Software wants refined; reffed for the pass and left
 *   otherwise untouched. Not NULL.
 * @param require_flags What the caller needs filled in.
 * @param cancellable Cancellable of the pass, or NULL.
 * @param callback Called on completion, or NULL.
 * @param user_data Data passed to @p callback.
 * @param source_tag Source tag set on the GTask.
 * @pre Main thread, as GNOME Software's refine vfunc.
 * @post A pass asking for none of the flags this plugin handles - an
 *   icon-or-id-only refine from the overview page, typically - completes
 *   successfully straight away, without a thread hop and without any daemon
 *   call. Otherwise the work happens on a worker thread; collect the outcome
 *   with gs_modulix_refine_finish().
 * @return None.
 */
void gs_modulix_refine_async(GsPlugin *plugin, GsAppList *list,
                             GsPluginRefineRequireFlags require_flags,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data,
                             gpointer source_tag) {
  GTask *task = g_task_new(plugin, cancellable, callback, user_data);
  g_task_set_source_tag(task, source_tag);

  g_debug("[modulix] refine(require_flags=0x%x, n_apps=%u) enter",
          require_flags, gs_app_list_length(list));

  /* Nothing we handle was requested (e.g. ICON/ID-only refine from the
   * overview page): skip the thread hop entirely rather than doing the
   * blocking Flathub fetch for flags nobody asked for. */
  if (!(require_flags & MODULIX_REFINE_FLAGS_WE_HANDLE)) {
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
    return;
  }

  RefineData *data = g_new0(RefineData, 1);
  data->plugin = plugin;
  data->list = g_object_ref(list);
  data->require_flags = require_flags;

  g_task_set_task_data(task, data, (GDestroyNotify)refine_data_free);
  g_task_run_in_thread(task, refine_thread);
  g_object_unref(task);
}

/**
 * @brief Collects the outcome of a refine pass.
 *
 * @param result The GAsyncResult handed to the completion callback. Not NULL.
 * @param error Return location for the error, or NULL.
 * @pre Called once, from the callback of gs_modulix_refine_async().
 * @post None beyond consuming the task's result.
 * @return TRUE when the pass ran to completion, including a pass that had
 *   nothing to do; FALSE with @p error set when it was cancelled. Metadata that
 *   the daemon could not supply is left unset rather than reported as an error.
 */
gboolean gs_modulix_refine_finish(GAsyncResult *result, GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}
