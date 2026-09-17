/*
 * gs-plugin-modulix.c — GNOME Software plugin for Modulix-OS (gnome-software
 * 49/50)
 *
 * Both reads (search / list / metadata) and writes (install / uninstall) go
 * through the `modulix-store-client` shim (`modulix-store-client.h`,
 * `mx_store_*`), a thin C ABI over two D-Bus interfaces served by the
 * `mx-daemon` system daemon: `org.modulix.Store1` (reads, JSON payloads
 * reshaped by the shim from `a{sv}`) and `org.modulix.Daemon` (writes,
 * polkit-gated on the daemon side).
 *
 * Apps map to GsApp as: nix package → desktop app, Modulix module → desktop app
 * (with its plugins attached as ADDON addons), module plugin → ADDON. The
 * canonical AppStream id is used as the GsApp id so entries deduplicate with
 * the Flatpak/AppStream of the same app.
 *
 * Dedup priority against other plugins (module/nix > Flatpak) comes from
 * GS_PLUGIN_RULE_BETTER_THAN edges declared in gs_plugin_modulix_init(), the
 * only lever a plugin has — there is no public per-app priority setter (see
 * CLAUDE.md). Module > nix package, when both come from us, is instead
 * enforced by emission order + a per-task seen_ids table in gs-modulix-list.c
 * (modules first). The "GnomeSoftware::SortKey" metadata, computed from the
 * daemon's neutral `kind`/`variant_rank` fields (see gs-modulix-app.c), only
 * orders the details-page "Sources" popover, not this dedup.
 *
 * This file is the GObject shell only: type definition, vfunc plumbing and
 * process-wide setup/teardown. The vfunc bodies live next door —
 *   gs-modulix-list.c       list_apps (search / installed / alternate_of)
 *   gs-modulix-refine.c     refine (addons, icons, descriptions, licenses)
 *   gs-modulix-lifecycle.c  install / uninstall (coalescing queue)
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

#include "modulix-store-client.h"
#include <glib/gi18n-lib.h>
#include <gnome-software.h>

/* ─── GObject type ──────────────────────────────────────────────────────── */

#define GS_TYPE_PLUGIN_MODULIX (gs_plugin_modulix_get_type())

G_DECLARE_FINAL_TYPE(GsPluginModulix, gs_plugin_modulix, GS, PLUGIN_MODULIX,
                     GsPlugin)

struct _GsPluginModulix {
  GsPlugin parent_instance;

  GsModulixLifecycle *lifecycle; /* install/uninstall queue */
};

G_DEFINE_TYPE(GsPluginModulix, gs_plugin_modulix, GS_TYPE_PLUGIN)

/* ─── setup ─────────────────────────────────────────────────────────────── */

static void gs_plugin_modulix_setup_async(GsPlugin *plugin,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
  GTask *task = g_task_new(plugin, cancellable, callback, user_data);
  g_task_set_source_tag(task, gs_plugin_modulix_setup_async);

  bindtextdomain(GETTEXT_PACKAGE, MODULIX_LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");

  /* Order matters: the resolver snapshots whatever the icon theme knows, so
   * the search paths must be in place first. Both are main-thread only. */
  gs_modulix_icon_theme_setup();
  gs_modulix_icon_resolver_init();

  if (mx_store_init() != STORE_OK) {
    g_task_return_new_error(task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
                            "failed to initialise the Modulix backend");
  } else {
    g_task_return_boolean(task, TRUE);
  }
  g_object_unref(task);
}

static gboolean gs_plugin_modulix_setup_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                               GAsyncResult *result,
                                               GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ─── vfuncs ────────────────────────────────────────────────────────────── */

static void gs_plugin_modulix_list_apps_async(
    GsPlugin *plugin, GsAppQuery *query,
    GsPluginListAppsFlags flags G_GNUC_UNUSED,
    GsPluginEventCallback event_cb G_GNUC_UNUSED,
    gpointer event_data G_GNUC_UNUSED, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  gs_modulix_list_apps_async(plugin, query, cancellable, callback, user_data,
                             gs_plugin_modulix_list_apps_async);
}

static GsAppList *
gs_plugin_modulix_list_apps_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                   GAsyncResult *result, GError **error) {
  return gs_modulix_list_apps_finish(result, error);
}

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

static gboolean gs_plugin_modulix_refine_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                                GAsyncResult *result,
                                                GError **error) {
  return gs_modulix_refine_finish(result, error);
}

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

static gboolean
gs_plugin_modulix_install_apps_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                      GAsyncResult *result, GError **error) {
  return gs_modulix_lifecycle_finish(result, error);
}

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

static gboolean
gs_plugin_modulix_uninstall_apps_finish(GsPlugin *plugin G_GNUC_UNUSED,
                                        GAsyncResult *result, GError **error) {
  return gs_modulix_lifecycle_finish(result, error);
}

/* ─── GObject ───────────────────────────────────────────────────────────── */

static void gs_plugin_modulix_init(GsPluginModulix *self) {
  self->lifecycle = gs_modulix_lifecycle_new();

  gs_plugin_add_rule(GS_PLUGIN(self), GS_PLUGIN_RULE_RUN_AFTER, "appstream");
  /* Inter-plugin dedup priority (gs_app_get_priority(), lib/gs-app.c) comes
   * from the fixed point over these BETTER_THAN edges, not from plugin
   * order: flatpak declares BETTER_THAN packagekit (priority 1), so modulix
   * must beat both to win the search-page / installed-page dedup against a
   * same-id Flatpak. packagekit is the fallback target if flatpak is
   * disabled; an unresolvable rule target is only a g_debug, not an error. */
  gs_plugin_add_rule(GS_PLUGIN(self), GS_PLUGIN_RULE_BETTER_THAN, "flatpak");
  gs_plugin_add_rule(GS_PLUGIN(self), GS_PLUGIN_RULE_BETTER_THAN, "packagekit");
  /* A GsRemoteIcon we add during a DESCRIPTION refine must still be queued
   * for download in the same job — the "icons" plugin (RUN_AFTER appstream
   * only) needs to run after us to pick it up. */
  gs_plugin_add_rule(GS_PLUGIN(self), GS_PLUGIN_RULE_RUN_BEFORE, "icons");
}

static void gs_plugin_modulix_finalize(GObject *object) {
  GsPluginModulix *self = GS_PLUGIN_MODULIX(object);

  mx_store_shutdown();
  gs_modulix_icon_cache_clear();
  gs_modulix_enrichment_cache_clear();
  gs_modulix_plugins_cache_clear();
  gs_modulix_icon_resolver_shutdown();
  /* A running lifecycle worker holds a plugin ref, so finalize cannot race a
   * drain. */
  g_clear_pointer(&self->lifecycle, gs_modulix_lifecycle_free);

  G_OBJECT_CLASS(gs_plugin_modulix_parent_class)->finalize(object);
}

static void gs_plugin_modulix_class_init(GsPluginModulixClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GsPluginClass *plugin_class = GS_PLUGIN_CLASS(klass);

  object_class->finalize = gs_plugin_modulix_finalize;

  plugin_class->setup_async = gs_plugin_modulix_setup_async;
  plugin_class->setup_finish = gs_plugin_modulix_setup_finish;
  plugin_class->list_apps_async = gs_plugin_modulix_list_apps_async;
  plugin_class->list_apps_finish = gs_plugin_modulix_list_apps_finish;
  plugin_class->refine_async = gs_plugin_modulix_refine_async;
  plugin_class->refine_finish = gs_plugin_modulix_refine_finish;
  plugin_class->install_apps_async = gs_plugin_modulix_install_apps_async;
  plugin_class->install_apps_finish = gs_plugin_modulix_install_apps_finish;
  plugin_class->uninstall_apps_async = gs_plugin_modulix_uninstall_apps_async;
  plugin_class->uninstall_apps_finish = gs_plugin_modulix_uninstall_apps_finish;
}

/* ─── Entry point ───────────────────────────────────────────────────────── */

GType gs_plugin_query_type(void) { return GS_TYPE_PLUGIN_MODULIX; }
