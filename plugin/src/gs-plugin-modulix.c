/**
 * @file gs-plugin-modulix.c
 * @brief GObject shell and GsPluginClass vfunc plumbing for the Modulix
 *   GNOME Software plugin.
 *
 * Discovery and naming: GNOME Software's GsPluginLoader finds this plugin by
 * `g_module_open()`-ing the built shared object and resolving the
 * `gs_plugin_query_type` symbol (`gs_plugin_create()`, gnome-software's
 * `lib/gs-plugin.c`); the plugin's *name* — the string every
 * `gs_plugin_add_rule()` edge and log line refers to it by — is derived from
 * that filename by stripping the `libgs_plugin_` prefix. This plugin ships
 * as `libgs_plugin_modulix.so` (see the project's meson/Justfile), so its
 * name is `modulix`. There is no numeric load-order/priority knob beyond the
 * two `GS_PLUGIN_RULE_RUN_AFTER`/`RUN_BEFORE` edges declared in
 * gs_plugin_modulix_init() — see that function's doc for exactly what they
 * order.
 *
 * Vtable hooks installed (gs_plugin_modulix_class_init()) and call order:
 * gs_plugin_modulix_init() (the GObject instance `init`, not a GsPluginClass
 * vfunc) runs once, synchronously, immediately after `g_object_new()` for
 * every constructed instance — enabled or not. `setup_async`/`setup_finish`
 * then run once, but only if the plugin is still enabled at construction
 * time; a `setup_finish` error disables the plugin for the rest of the
 * process's life. After a successful setup, `list_apps_async`,
 * `refresh_metadata_async`, `refine_async`, `update_apps_async`,
 * `install_apps_async` and `uninstall_apps_async` are each
 * invoked by GsPluginLoader an arbitrary number of times, in whatever order
 * the UI issues jobs — GsPluginClass defines no fixed relative order between
 * them. `finalize` runs once, when the GsPlugin object is disposed (process
 * shutdown or a plugin reload).
 *
 * Threads: `setup_async` runs its own body synchronously on the thread that
 * calls it — the main thread, per the comments on the icon-theme/resolver
 * setup calls it makes — it never hops to a worker despite the `_async` name.
 * `list_apps_async`, `refresh_metadata_async` and `refine_async` hand off to
 * gs-modulix-list.c / gs-modulix-update.c / gs-modulix-refine.c, which
 * reject or short-circuit synchronously on the calling thread when they can,
 * and otherwise run their body on a GLib worker-pool thread via
 * `g_task_run_in_thread()`. `install_apps_async` and `uninstall_apps_async`
 * hand off to gs-modulix-lifecycle.c: enqueueing (and moving the apps to
 * their transient in-progress state) happens synchronously on the calling
 * thread, while the actual daemon write call runs on one dedicated,
 * serialized `GThread` ("modulix-lifecycle") shared by every coalesced
 * install/uninstall. `update_apps_async` hands off to gs-modulix-update.c,
 * which similarly moves the "Modulix OS" app to its transient state
 * synchronously before running the blocking `UpdateSystem` call on a GLib
 * worker-pool thread, guarded by its own process-wide in-flight flag (at
 * most one system update at a time, never coalesced like install/uninstall).
 *
 * Reaching the daemon: this file itself calls only `gs_modulix_bus_init()`
 * (`setup_async`) and `gs_modulix_bus_shutdown()` (`finalize`) directly, via
 * `plugin/src/dbus/gs-modulix-bus.h` — the single system-bus connection
 * shared by every other `dbus/` wrapper. Every other D-Bus call (searches,
 * listings, enrichment, licenses, install/uninstall writes) is made by the
 * sibling files below, always through `plugin/src/dbus/gs-modulix-store1.h`/
 * `gs-modulix-daemon1.h`'s synchronous GDBus/GVariant wrappers, never by
 * JSON. A failed read logs its own warning and returns NULL, which this
 * plugin's consumers treat as "0 results"; a failed write returns NULL with
 * the reason in a `GError` (bad UTF-8 input, no bus connection, a D-Bus
 * error, or — for writes — a denied polkit prompt).
 *
 * Both reads (search / list / metadata) and writes (install / uninstall) go
 * straight to two D-Bus interfaces served by the `mx-daemon` system daemon:
 * `org.modulix.Store1` (reads, native `a{sv}`/`aa{sv}` payloads, no JSON
 * anywhere on the wire or in this process) and `org.modulix.Daemon`
 * (writes, polkit-gated on the daemon side).
 *
 * Apps map to GsApp as: nix package → desktop app, Modulix module → desktop app
 * (with its plugins attached as ADDON addons), module plugin → ADDON. The
 * canonical AppStream id is used as the GsApp id so entries deduplicate with
 * the Flatpak/AppStream of the same app.
 *
 * Dedup priority against other plugins is per-app, not rule-based:
 * gs_plugin_modulix_init() declares no GS_PLUGIN_RULE_BETTER_THAN edge, so this
 * plugin's own fixed-point priority stays at the 0 sentinel, and a module is
 * instead raised to MODULIX_PRIORITY_MODULE (2) by gs_app_set_priority() in
 * gs-modulix-app.c. Net order: module (2) > Flatpak (1, from its own
 * BETTER_THAN packagekit edge) > bare nix package (0). Module > nix package,
 * when both come from us, is enforced separately by emission order plus a
 * per-task seen_ids table in gs-modulix-list.c (modules first). The
 * "GnomeSoftware::SortKey" metadata, computed from the daemon's neutral
 * `kind`/`variant_rank` fields (see gs-modulix-app.c), only orders the
 * details-page "Sources" popover, and governs neither of those two.
 *
 * This file is the GObject shell only: type definition, vfunc plumbing and
 * process-wide setup/teardown. The vfunc bodies live next door —
 *   gs-modulix-list.c       list_apps (search / installed / alternate_of / for-update)
 *   gs-modulix-refine.c     refine (addons, icons, descriptions, licenses)
 *   gs-modulix-lifecycle.c  install / uninstall (coalescing queue)
 *   gs-modulix-update.c     refresh_metadata / update_apps (system update)
 */

/**
 * @def I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
 * @brief Acknowledgement macro `gnome-software.h` requires from any
 *   out-of-tree includer, since gnome-software makes no API/ABI stability
 *   promise across releases.
 *
 * @pre None.
 * @post `gnome-software.h` (included right below) does not `#error` out.
 */
#ifndef I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#define I_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE
#endif

#include "gs-modulix-enrichment-cache.h"
#include "gs-modulix-icon-cache.h"
#include "gs-modulix-icon-resolver.h"
#include "gs-modulix-icon-theme.h"
#include "gs-modulix-lifecycle.h"
#include "gs-modulix-list.h"
#include "gs-modulix-plugins-cache.h"
#include "gs-modulix-refine.h"
#include "gs-modulix-update.h"

#include "dbus/gs-modulix-bus.h"
#include <glib/gi18n-lib.h>
#include <gnome-software.h>

/**
 * @def GS_TYPE_PLUGIN_MODULIX
 * @brief GType macro for #GsPluginModulix, GLib's standard `xxx_get_type()`
 *   accessor wrapper.
 *
 * @return The #GType registered for #GsPluginModulix, computed once by
 *   gs_plugin_modulix_get_type() (generated by G_DEFINE_TYPE below) and
 *   cached by GObject from then on.
 */
#define GS_TYPE_PLUGIN_MODULIX (gs_plugin_modulix_get_type())

/**
 * @brief Declares the #GsPluginModulix final GObject type (and its
 *   companion #GsPluginModulixClass, cast macros, and the
 *   gs_plugin_modulix_get_type() prototype) via GLib's
 *   `G_DECLARE_FINAL_TYPE`.
 *
 * @param GsPluginModulix Instance struct typedef name to declare.
 * @param gs_plugin_modulix Lower-case function-name prefix used to build
 *   `gs_plugin_modulix_get_type()` and the `GS_PLUGIN_MODULIX()` cast macro.
 * @param GS Upper-case namespace prefix used in generated macro names.
 * @param PLUGIN_MODULIX Upper-case type name used in generated macro names.
 * @param GsPlugin Parent type: this plugin subclasses #GsPlugin directly,
 *   overriding the vfuncs listed in gs_plugin_modulix_class_init().
 *
 * @post `struct _GsPluginModulix` (defined right below) is the type's
 *   instance layout; being a "final" type, it cannot itself be subclassed.
 */
G_DECLARE_FINAL_TYPE(GsPluginModulix, gs_plugin_modulix, GS, PLUGIN_MODULIX,
                     GsPlugin)

/**
 * @brief Instance struct of the Modulix `GsPlugin` subclass; one instance is
 *   constructed by GsPluginLoader for the whole process (see
 *   gs_plugin_create(), gnome-software's `lib/gs-plugin.c`).
 */
struct _GsPluginModulix {
  /** Base-class instance data; must come first for the GObject cast macros
   * generated by G_DECLARE_FINAL_TYPE to work. */
  GsPlugin parent_instance;

  /** Install/uninstall coalescing queue for this plugin instance. Owned
   * exclusively by this struct: created in gs_plugin_modulix_init(),
   * consulted by gs_plugin_modulix_install_apps_async() and
   * gs_plugin_modulix_uninstall_apps_async(), and released with
   * gs_modulix_lifecycle_free() in gs_plugin_modulix_finalize(). NULL only
   * before gs_plugin_modulix_init() has run. */
  GsModulixLifecycle *lifecycle;
};

/**
 * @brief Registers #GsPluginModulix with the GObject type system and
 *   defines gs_plugin_modulix_get_type(), gs_plugin_modulix_init() (instance
 *   init) and gs_plugin_modulix_class_init() (class init) plumbing, via
 *   GLib's `G_DEFINE_TYPE`.
 *
 * @param GsPluginModulix Instance struct typedef being registered.
 * @param gs_plugin_modulix Function-name prefix matching the one passed to
 *   `G_DECLARE_FINAL_TYPE` above.
 * @param GS_TYPE_PLUGIN Parent GType macro (#GsPlugin).
 *
 * @post gs_plugin_modulix_init() runs once per new instance, right after
 *   `g_object_new()`; gs_plugin_modulix_class_init() runs once, the first
 *   time the type is referenced.
 */
G_DEFINE_TYPE(GsPluginModulix, gs_plugin_modulix, GS_TYPE_PLUGIN)

/**
 * @brief Implementation of `GsPluginClass::setup_async`: the plugin's
 *   one-time, process-wide initialization, called by GsPluginLoader once
 *   after the #GsPluginModulix instance is constructed and before any other
 *   vfunc runs — but only if the plugin is still enabled at that point.
 *
 * Runs synchronously on the thread that calls it (the main thread — see
 * gs_modulix_icon_theme_setup()/gs_modulix_icon_resolver_init(), which
 * require it) despite the `_async` shape: every step below executes before
 * this function returns, and the #GTask is completed inline rather than
 * handed to a worker thread.
 *
 * Order matters: gettext binding, then icon-theme search-path setup, then
 * icon-resolver indexing (which snapshots what the theme setup just
 * registered), then the failure monitor, then the daemon connection last.
 *
 * Daemon-side write failures are only visible on the bus; the failure
 * monitor turns them into a notification with the full Nix error one click
 * away.
 *
 * @param plugin The #GsPluginModulix instance being set up (unused beyond
 *   the #GTask source object).
 * @param cancellable Cancellation token forwarded to `g_task_new()`;
 *   nullable per `GsPluginClass::setup_async`'s contract, not otherwise
 *   consulted by this body.
 * @param callback Invoked (via `g_task_return_*`) once setup has finished,
 *   with the result retrievable through gs_plugin_modulix_setup_finish().
 * @param user_data Opaque pointer forwarded unchanged to @p callback.
 *
 * @pre None beyond `GsPluginClass::setup_async`'s own contract: called at
 *   most once per instance.
 * @post `gs_modulix_bus_init()` has been attempted; on failure the task carries a
 *   `GS_PLUGIN_ERROR_FAILED` #GError, which `setup_finish` propagates,
 *   causing GsPluginLoader to disable this plugin for the process's
 *   lifetime. On success, every other vfunc in this file may now assume a
 *   live daemon connection, an indexed icon theme, and an initialized
 *   failure monitor.
 */
static void gs_plugin_modulix_setup_async(GsPlugin *plugin,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
  GTask *task = g_task_new(plugin, cancellable, callback, user_data);
  g_task_set_source_tag(task, gs_plugin_modulix_setup_async);

  bindtextdomain(GETTEXT_PACKAGE, MODULIX_LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");

  gs_modulix_icon_theme_setup();
  gs_modulix_icon_resolver_init();

  g_autoptr(GError) error = NULL;
  if (!gs_modulix_bus_init(&error)) {
    g_task_return_new_error(task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
                            "failed to initialise the Modulix backend: %s",
                            error->message);
  } else {
    g_task_return_boolean(task, TRUE);
  }
  g_object_unref(task);
}

/**
 * @brief Implementation of `GsPluginClass::setup_finish`: retrieves the
 *   result of gs_plugin_modulix_setup_async().
 *
 * @param plugin Unused (required by the `GsPluginClass::setup_finish`
 *   signature).
 * @param result The #GAsyncResult (actually a #GTask) handed to the
 *   `callback` of gs_plugin_modulix_setup_async().
 * @param error Set to a `GS_PLUGIN_ERROR_FAILED` #GError, transfer full to
 *   the caller (who must `g_error_free()` it), if `gs_modulix_bus_init()`
 *   failed; left untouched on success. Nullable per GLib convention.
 *
 * @return `TRUE` if setup succeeded, `FALSE` otherwise (with @p error set).
 *   A `FALSE` return disables this plugin for the rest of the process's
 *   life, per `GsPluginClass::setup_finish`'s documented contract.
 *
 * @pre Must be called exactly once, from the `callback` passed to
 *   gs_plugin_modulix_setup_async(), with that same call's #GAsyncResult.
 */
static gboolean gs_plugin_modulix_setup_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                               GAsyncResult *result,
                                               GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

/**
 * @brief Implementation of `GsPluginClass::list_apps_async`: forwards to
 *   gs_modulix_list_apps_async() (gs-modulix-list.c), which answers exactly
 *   three query shapes — `alternate_of`, `is_installed == TRUE`, and a
 *   keyword search — over `mx_store_search_{packages,modules}`,
 *   `mx_store_list_installed_{packages,modules}` or
 *   `mx_store_packages_for_app_id`, and fails any other query shape
 *   synchronously with `G_IO_ERROR_NOT_SUPPORTED`.
 *
 * Threading: the body rejects an unsupported query synchronously on the
 * calling thread; a supported one runs on a GLib worker-pool thread via
 * `g_task_run_in_thread()` (see `list_apps_thread` in gs-modulix-list.c).
 *
 * @param plugin The #GsPluginModulix instance the query runs against;
 *   forwarded as-is.
 * @param query Describes what to list (search terms, installed-only, or
 *   `alternate_of`); owned by the caller, borrowed for the duration of the
 *   call.
 * @param flags Unused by this plugin.
 * @param event_cb Unused by this plugin.
 * @param event_data Unused by this plugin.
 * @param cancellable Cancellation token forwarded unchanged; nullable.
 * @param callback Invoked once the (possibly worker-thread) body completes;
 *   result retrievable through gs_plugin_modulix_list_apps_finish().
 * @param user_data Opaque pointer forwarded unchanged to @p callback.
 *
 * @pre gs_plugin_modulix_setup_async() must have completed successfully
 *   (daemon connection live).
 * @post On success, the resulting #GsAppList is available from
 *   gs_plugin_modulix_list_apps_finish(); each #GsApp in it is either newly
 *   allocated or reused from the plugin cache (see
 *   gs_modulix_make_app_from_json(), gs-modulix-app.h).
 */
static void gs_plugin_modulix_list_apps_async(
    GsPlugin *plugin, GsAppQuery *query,
    GsPluginListAppsFlags flags G_GNUC_UNUSED,
    GsPluginEventCallback event_cb G_GNUC_UNUSED,
    gpointer event_data G_GNUC_UNUSED, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  gs_modulix_list_apps_async(plugin, query, cancellable, callback, user_data,
                             gs_plugin_modulix_list_apps_async);
}

/**
 * @brief Implementation of `GsPluginClass::list_apps_finish`: retrieves the
 *   result of gs_plugin_modulix_list_apps_async() via
 *   gs_modulix_list_apps_finish() (gs-modulix-list.c).
 *
 * @param plugin Unused (required by the `GsPluginClass::list_apps_finish`
 *   signature).
 * @param result The #GAsyncResult handed to the `callback` of
 *   gs_plugin_modulix_list_apps_async().
 * @param error Set on failure (e.g. an unsupported query shape, or a
 *   cancellation), transfer full to the caller; left untouched on success.
 *
 * @return (transfer full) A newly allocated #GsAppList the caller owns and
 *   must `g_object_unref()`, or `NULL` on failure with @p error set.
 */
static GsAppList *
gs_plugin_modulix_list_apps_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                   GAsyncResult *result, GError **error) {
  return gs_modulix_list_apps_finish(result, error);
}

/**
 * @brief Implementation of `GsPluginClass::refresh_metadata_async`: forwards
 *   to gs_modulix_update_refresh_metadata_async() (gs-modulix-update.c),
 *   which re-syncs the "Modulix OS" synthetic GsApp's outdated-inputs state.
 *
 * @param plugin The #GsPluginModulix instance; forwarded as-is.
 * @param cache_age_secs Forwarded unchanged; `0` (an explicit user-triggered
 *   refresh) forces a live `ListOutdatedInputs` re-check instead of serving
 *   the daemon's 1h cache.
 * @param flags Unused by this plugin.
 * @param event_cb Unused by this plugin.
 * @param event_data Unused by this plugin.
 * @param cancellable Cancellation token forwarded unchanged; nullable.
 * @param callback Invoked once the (worker-thread) sync completes.
 * @param user_data Opaque pointer forwarded unchanged to @p callback.
 *
 * @pre gs_plugin_modulix_setup_async() must have completed successfully.
 * @post The "Modulix OS" GsApp held in the plugin cache reflects the latest
 *   `ListOutdatedInputs` read. Always succeeds — a failed daemon read is "no
 *   outdated inputs", not a job failure.
 */
static void gs_plugin_modulix_refresh_metadata_async(
    GsPlugin *plugin, guint64 cache_age_secs,
    GsPluginRefreshMetadataFlags flags G_GNUC_UNUSED,
    GsPluginEventCallback event_cb G_GNUC_UNUSED,
    gpointer event_data G_GNUC_UNUSED, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  gs_modulix_update_refresh_metadata_async(
      plugin, cache_age_secs, cancellable, callback, user_data,
      gs_plugin_modulix_refresh_metadata_async);
}

/**
 * @brief Implementation of `GsPluginClass::refresh_metadata_finish`.
 *
 * @param plugin Unused (required by the vfunc signature).
 * @param result The #GAsyncResult of gs_plugin_modulix_refresh_metadata_async().
 * @param error Set on failure (cancellation only), transfer full to the caller.
 * @return TRUE on success, FALSE with @p error set on cancellation.
 */
static gboolean gs_plugin_modulix_refresh_metadata_finish(
    GsPlugin *plugin G_GNUC_UNUSED, GAsyncResult *result, GError **error) {
  return gs_modulix_update_refresh_metadata_finish(result, error);
}

/**
 * @brief Implementation of `GsPluginClass::refine_async`: fills in the metadata
 *   GNOME Software is missing, through gs_modulix_refine_async().
 *
 * @param plugin This plugin. Not NULL.
 * @param list Apps to refine; apps of other plugins are ignored downstream.
 * @param job_flags Unused (the job-level flags carry nothing this plugin acts
 *   on).
 * @param require_flags What the caller needs filled in; forwarded as-is.
 * @param event_cb Unused: this plugin reports failures through the GTask's
 *   GError rather than as plugin events.
 * @param event_data Unused.
 * @param cancellable Cancellable of the pass, or NULL.
 * @param callback Called on completion, or NULL.
 * @param user_data Data passed to @p callback.
 * @pre Main thread. The daemon connection must be live
 *   (gs_plugin_modulix_setup_async() completed).
 * @post A pass asking for nothing this plugin handles returns at once; anything
 *   else is done on a worker thread, possibly hitting the daemon and Flathub.
 * @return None.
 */
static void gs_plugin_modulix_refine_async(
    GsPlugin *plugin, GsAppList *list,
    GsPluginRefineFlags job_flags G_GNUC_UNUSED,
    GsPluginRefineRequireFlags require_flags,
    GsPluginEventCallback event_cb G_GNUC_UNUSED,
    gpointer event_data G_GNUC_UNUSED, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  gs_modulix_refine_async(plugin, list, require_flags, cancellable, callback,
                          user_data, gs_plugin_modulix_refine_async);
}

/**
 * @brief Implementation of `GsPluginClass::refine_finish`.
 *
 * @param plugin Unused (required by the vfunc signature).
 * @param result The #GAsyncResult of gs_plugin_modulix_refine_async().
 * @param error Set on failure, transfer full to the caller.
 * @pre Called once, from the async call's callback.
 * @post None.
 * @return TRUE when the pass completed, FALSE with @p error set when it was
 *   cancelled. Metadata the daemon could not provide is left unset, not
 *   reported.
 */
static gboolean gs_plugin_modulix_refine_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                                GAsyncResult *result,
                                                GError **error) {
  return gs_modulix_refine_finish(result, error);
}

/**
 * @brief Implementation of `GsPluginClass::update_apps_async`: applies the
 *   Modulix system update, through gs_modulix_update_apps_async()
 *   (gs-modulix-update.c).
 *
 * @param plugin This plugin. Not NULL.
 * @param list Apps to update; only this plugin's synthetic "Modulix OS" app,
 *   if present, is acted on.
 * @param flags Selects `boot` vs `switch` vs no-op; see
 *   gs_modulix_update_apps_async()'s doc for the exact mapping.
 * @param progress_cb Called once with `GS_APP_PROGRESS_UNKNOWN` right before
 *   the daemon call starts — a NixOS rebuild reports no granular progress.
 * @param progress_data Opaque pointer forwarded to @p progress_cb.
 * @param event_cb Unused.
 * @param event_data Unused.
 * @param action_cb Unused: the polkit prompt is raised by the daemon, not
 *   through GNOME Software's user-action mechanism.
 * @param action_data Unused.
 * @param cancellable Cancellable of the operation, or NULL. It does not abort
 *   a rebuild the daemon already started.
 * @param callback Called on completion, or NULL.
 * @param user_data Data passed to @p callback.
 * @pre Main thread, daemon connection live.
 * @post At most one system update runs at a time process-wide; a second
 *   trigger while one is in flight fails immediately instead of queuing.
 * @return None.
 */
static void gs_plugin_modulix_update_apps_async(
    GsPlugin *plugin, GsAppList *list,
    GsPluginUpdateAppsFlags flags,
    GsPluginProgressCallback progress_cb, gpointer progress_data,
    GsPluginEventCallback event_cb G_GNUC_UNUSED,
    gpointer event_data G_GNUC_UNUSED,
    GsPluginAppNeedsUserActionCallback action_cb G_GNUC_UNUSED,
    gpointer action_data G_GNUC_UNUSED, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  gs_modulix_update_apps_async(plugin, list, flags, progress_cb, progress_data,
                               cancellable, callback, user_data,
                               gs_plugin_modulix_update_apps_async);
}

/**
 * @brief Implementation of `GsPluginClass::update_apps_finish`.
 *
 * @param plugin Unused (required by the vfunc signature).
 * @param result The #GAsyncResult of gs_plugin_modulix_update_apps_async().
 * @param error Set on failure, transfer full to the caller.
 * @return TRUE when the update succeeded or there was nothing of ours to
 *   update; FALSE with @p error set on a failed `UpdateSystem` call, an
 *   update already in flight, or cancellation.
 */
static gboolean
gs_plugin_modulix_update_apps_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                     GAsyncResult *result, GError **error) {
  return gs_modulix_update_apps_finish(result, error);
}

/**
 * @brief Implementation of `GsPluginClass::install_apps_async`: installs the
 *   plugin's apps through the lifecycle queue.
 *
 * @param plugin This plugin. Not NULL.
 * @param list Apps to install; only the ones this plugin owns are acted on.
 * @param flags Unused.
 * @param progress_cb Unused: a NixOS rebuild reports no granular progress, so
 *   the UI only ever sees the state change.
 * @param progress_data Unused.
 * @param event_cb Unused.
 * @param event_data Unused.
 * @param action_cb Unused: the polkit prompt is raised by the daemon, not
 *   through GNOME Software's user-action mechanism.
 * @param action_data Unused.
 * @param cancellable Cancellable of the operation, or NULL. It does not abort a
 *   rebuild the daemon already started.
 * @param callback Called on completion, or NULL.
 * @param user_data Data passed to @p callback.
 * @pre Main thread, daemon connection live.
 * @post The apps switch to their in-progress state immediately; the daemon call
 *   and its minutes-long rebuild happen on the lifecycle worker thread, with
 *   concurrent requests coalesced into one queue.
 * @return None.
 */
static void gs_plugin_modulix_install_apps_async(
    GsPlugin *plugin, GsAppList *list,
    GsPluginInstallAppsFlags flags G_GNUC_UNUSED,
    GsPluginProgressCallback progress_cb G_GNUC_UNUSED,
    gpointer progress_data G_GNUC_UNUSED,
    GsPluginEventCallback event_cb G_GNUC_UNUSED,
    gpointer event_data G_GNUC_UNUSED,
    GsPluginAppNeedsUserActionCallback action_cb G_GNUC_UNUSED,
    gpointer action_data G_GNUC_UNUSED, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  GsPluginModulix *self = GS_PLUGIN_MODULIX(plugin);
  gs_modulix_lifecycle_install_async(self->lifecycle, plugin, list, cancellable,
                                     callback, user_data,
                                     gs_plugin_modulix_install_apps_async);
}

/**
 * @brief Implementation of `GsPluginClass::install_apps_finish`.
 *
 * @param plugin Unused (required by the vfunc signature).
 * @param result The #GAsyncResult of gs_plugin_modulix_install_apps_async().
 * @param error Set on failure, transfer full to the caller.
 * @pre Called once, from the async call's callback.
 * @post None.
 * @return TRUE when every app of the batch was installed, or when the batch
 * held none of this plugin's apps; FALSE with @p error set when the rebuild
 * failed or authorisation was refused.
 */
static gboolean
gs_plugin_modulix_install_apps_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                      GAsyncResult *result, GError **error) {
  return gs_modulix_lifecycle_finish(result, error);
}

/**
 * @brief Implementation of `GsPluginClass::uninstall_apps_async`: uninstalls
 * the plugin's apps through the lifecycle queue.
 *
 * @param plugin This plugin. Not NULL.
 * @param list Apps to uninstall; foreign apps are ignored.
 * @param flags Unused.
 * @param progress_cb Unused (no granular progress from a rebuild).
 * @param progress_data Unused.
 * @param event_cb Unused.
 * @param event_data Unused.
 * @param action_cb Unused (polkit prompts come from the daemon).
 * @param action_data Unused.
 * @param cancellable Cancellable of the operation, or NULL.
 * @param callback Called on completion, or NULL.
 * @param user_data Data passed to @p callback.
 * @pre Main thread, daemon connection live.
 * @post Same queueing and blocking behaviour as
 *   gs_plugin_modulix_install_apps_async(), with the uninstall path.
 * @return None.
 */
static void gs_plugin_modulix_uninstall_apps_async(
    GsPlugin *plugin, GsAppList *list,
    GsPluginUninstallAppsFlags flags G_GNUC_UNUSED,
    GsPluginProgressCallback progress_cb G_GNUC_UNUSED,
    gpointer progress_data G_GNUC_UNUSED,
    GsPluginEventCallback event_cb G_GNUC_UNUSED,
    gpointer event_data G_GNUC_UNUSED,
    GsPluginAppNeedsUserActionCallback action_cb G_GNUC_UNUSED,
    gpointer action_data G_GNUC_UNUSED, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  GsPluginModulix *self = GS_PLUGIN_MODULIX(plugin);
  gs_modulix_lifecycle_uninstall_async(self->lifecycle, plugin, list,
                                       cancellable, callback, user_data,
                                       gs_plugin_modulix_uninstall_apps_async);
}

/**
 * @brief Implementation of `GsPluginClass::uninstall_apps_finish`.
 *
 * @param plugin Unused (required by the vfunc signature).
 * @param result The #GAsyncResult of gs_plugin_modulix_uninstall_apps_async().
 * @param error Set on failure, transfer full to the caller.
 * @pre Called once, from the async call's callback.
 * @post None.
 * @return TRUE when the batch was uninstalled or held none of this plugin's
 *   apps; FALSE with @p error set on a failed rebuild or refused authorisation.
 */
static gboolean
gs_plugin_modulix_uninstall_apps_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                        GAsyncResult *result, GError **error) {
  return gs_modulix_lifecycle_finish(result, error);
}

/**
 * @brief GObject `init`: builds the lifecycle queue and declares the plugin's
 *   ordering rules.
 *
 * @param self The instance being created. Not NULL.
 * @pre Called once per instance by GObject.
 * @post The plugin runs after `appstream` and before `icons` — the latter so a
 *   remote icon added during a refine is still downloaded in the same job. No
 *   `BETTER_THAN` edge is declared: dedup priority stays 0 for packages, which
 *   makes a bare nix package lose against a same-id Flatpak, while modules
 * raise their own priority per app instead.
 * @return None.
 */
static void gs_plugin_modulix_init(GsPluginModulix *self) {
  self->lifecycle = gs_modulix_lifecycle_new();

  gs_plugin_add_rule(GS_PLUGIN(self), GS_PLUGIN_RULE_RUN_AFTER, "appstream");
  gs_plugin_add_rule(GS_PLUGIN(self), GS_PLUGIN_RULE_RUN_BEFORE, "icons");
}

/**
 * @brief GObject `finalize`: tears down every process-wide resource the plugin
 *   set up.
 *
 * @param object The plugin instance being finalized. Not NULL.
 * @pre GNOME Software is dropping the last reference, and no lifecycle
 *   operation is in flight — a running worker holds a plugin ref, so it cannot
 *   race this.
 * @post The store-client connection is shut down and the icon, enrichment and
 *   plugins caches, the icon resolver and the failure monitor are all torn
 * down. The enrichment and plugins caches destroy their locks here, so the
 * plugin must not be used again after this; the icon cache deliberately keeps
 * its lock so a reload within the same process stays safe.
 * @return None.
 */
static void gs_plugin_modulix_finalize(GObject *object) {
  GsPluginModulix *self = GS_PLUGIN_MODULIX(object);

  gs_modulix_bus_shutdown();
  gs_modulix_icon_cache_clear();
  gs_modulix_enrichment_cache_clear();
  gs_modulix_plugins_cache_clear();
  gs_modulix_icon_resolver_shutdown();
  g_clear_pointer(&self->lifecycle, gs_modulix_lifecycle_free);

  G_OBJECT_CLASS(gs_plugin_modulix_parent_class)->finalize(object);
}

/**
 * @brief GObject class init: installs `finalize` and every `GsPluginClass`
 *   vfunc this plugin implements.
 *
 * @param klass The class being initialised. Not NULL.
 * @pre Called once by GObject, before the first instance exists.
 * @post The class implements setup, list_apps, refresh_metadata, refine,
 *   update_apps, install_apps and uninstall_apps (async plus finish for
 *   each). Anything else - repo management, app launching - is left to the
 *   other plugins.
 * @return None.
 */
static void gs_plugin_modulix_class_init(GsPluginModulixClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GsPluginClass *plugin_class = GS_PLUGIN_CLASS(klass);

  object_class->finalize = gs_plugin_modulix_finalize;

  plugin_class->setup_async = gs_plugin_modulix_setup_async;
  plugin_class->setup_finish = gs_plugin_modulix_setup_finish;
  plugin_class->list_apps_async = gs_plugin_modulix_list_apps_async;
  plugin_class->list_apps_finish = gs_plugin_modulix_list_apps_finish;
  plugin_class->refresh_metadata_async = gs_plugin_modulix_refresh_metadata_async;
  plugin_class->refresh_metadata_finish = gs_plugin_modulix_refresh_metadata_finish;
  plugin_class->refine_async = gs_plugin_modulix_refine_async;
  plugin_class->refine_finish = gs_plugin_modulix_refine_finish;
  plugin_class->update_apps_async = gs_plugin_modulix_update_apps_async;
  plugin_class->update_apps_finish = gs_plugin_modulix_update_apps_finish;
  plugin_class->install_apps_async = gs_plugin_modulix_install_apps_async;
  plugin_class->install_apps_finish = gs_plugin_modulix_install_apps_finish;
  plugin_class->uninstall_apps_async = gs_plugin_modulix_uninstall_apps_async;
  plugin_class->uninstall_apps_finish = gs_plugin_modulix_uninstall_apps_finish;
}

/**
 * @brief The module's entry point: tells GNOME Software which GType to
 *   instantiate.
 *
 * @pre None; this is the first symbol GNOME Software resolves after loading the
 *   shared module.
 * @post None.
 * @return The GType of this plugin, which GNOME Software instantiates once.
 */
GType gs_plugin_query_type(void) { return GS_TYPE_PLUGIN_MODULIX; }
