/*
 * gs-modulix-list.c — the list_apps vfunc: search, installed listing, and the
 * details page's "Sources" (alternate_of) query.
 *
 * All three shapes boil down to the same three steps — one blocking
 * `mx_store_*` read, one JSON array appended to the result list, one debug
 * timing line — factored here into collect_into().
 */

#include "gs-modulix-list.h"

#include "gs-modulix-app.h"
#include "gs-modulix-config.h"

#include "modulix-store-client.h"

typedef struct {
  GsPlugin *plugin; /* borrowed: the task's source object outlives the data */
  gchar *query;
  gchar *alternate_of;
  gboolean installed;
  guint max_results;
} ListData;

static void list_data_free(ListData *d) {
  g_free(d->query);
  g_free(d->alternate_of);
  g_free(d);
}

/* Which Store1 read to issue. The shim's entry points differ in arity, so the
 * dispatch is an enum rather than a function pointer. */
typedef enum {
  MODULIX_READ_SEARCH_MODULES,
  MODULIX_READ_SEARCH_PACKAGES,
  MODULIX_READ_INSTALLED_MODULES,
  MODULIX_READ_INSTALLED_PACKAGES,
  MODULIX_READ_ALTERNATES,
} ModulixRead;

static const gchar *read_label(ModulixRead read) {
  switch (read) {
  case MODULIX_READ_SEARCH_MODULES:
    return "search_modules";
  case MODULIX_READ_SEARCH_PACKAGES:
    return "search_packages";
  case MODULIX_READ_INSTALLED_MODULES:
    return "list_installed_modules";
  case MODULIX_READ_INSTALLED_PACKAGES:
    return "list_installed_packages";
  case MODULIX_READ_ALTERNATES:
  default:
    return "packages_for_app_id";
  }
}

static gchar *store_read(ModulixRead read, const ListData *data) {
  switch (read) {
  case MODULIX_READ_SEARCH_MODULES:
    return mx_store_search_modules(data->query, data->max_results);
  case MODULIX_READ_SEARCH_PACKAGES:
    return mx_store_search_packages(data->query, data->max_results);
  case MODULIX_READ_INSTALLED_MODULES:
    return mx_store_list_installed_modules();
  case MODULIX_READ_INSTALLED_PACKAGES:
    return mx_store_list_installed_packages();
  case MODULIX_READ_ALTERNATES:
  default:
    return mx_store_packages_for_app_id(data->alternate_of);
  }
}

/* One daemon read appended to @list. @seen_ids (nullable) dedups by GsApp id
 * across the calls of one task — see gs_modulix_append_apps_from_json(). */
static void collect_into(GsAppList *list, const ListData *data,
                         ModulixRead read, gboolean is_installed,
                         JsonParser *parser, GHashTable *seen_ids) {
  gint64 t0 = g_get_monotonic_time();
  gchar *json = store_read(read, data);

  g_debug("[modulix] mx_store_%s(%s) %.0fms", read_label(read),
          data->query          ? data->query
          : data->alternate_of ? data->alternate_of
                               : "",
          (g_get_monotonic_time() - t0) / 1000.0);

  gs_modulix_append_apps_from_json(list, json, data->plugin, is_installed,
                                   parser, seen_ids);
  if (json != NULL)
    mx_store_free_string(json);
}

/* Returns TRUE (and completes @task with a cancellation error, freeing
 * @list_to_free) if @cancellable has been cancelled. Callers must return
 * immediately when this returns TRUE. */
static gboolean bail_if_cancelled(GTask *task, GCancellable *cancellable,
                                  GsAppList *list_to_free) {
  g_autoptr(GError) err = NULL;
  if (!g_cancellable_set_error_if_cancelled(cancellable, &err))
    return FALSE;
  g_clear_object(&list_to_free);
  g_task_return_error(task, g_steal_pointer(&err));
  return TRUE;
}

/* Modules are always collected before packages, sharing one seen_ids table:
 * intra-plugin dedup then keeps the module and drops a package with the same
 * GsApp id, so a module wins its own dedup regardless of list order
 * downstream. */
static gboolean collect_pair(GsAppList *list, const ListData *data, GTask *task,
                             GCancellable *cancellable, ModulixRead modules,
                             ModulixRead packages, gboolean is_installed,
                             JsonParser *parser) {
  g_autoptr(GHashTable) seen_ids =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

#if MODULIX_ENABLE_MODULES
  if (bail_if_cancelled(task, cancellable, list))
    return FALSE;
  collect_into(list, data, modules, is_installed, parser, seen_ids);
#endif

#if MODULIX_ENABLE_PACKAGES
  if (bail_if_cancelled(task, cancellable, list))
    return FALSE;
  collect_into(list, data, packages, is_installed, parser, seen_ids);
#endif

  return TRUE;
}

static void list_apps_thread(GTask *task, gpointer source_object G_GNUC_UNUSED,
                             gpointer task_data_ptr,
                             GCancellable *cancellable) {
  ListData *data = task_data_ptr;
  GsAppList *list = gs_app_list_new();
  /* Reuse a single parser for all JSON blobs in this task. */
  g_autoptr(JsonParser) parser = json_parser_new();

  g_debug("[modulix] list_apps(query=%s, installed=%d, alternate_of=%s, "
          "max_results=%u) enter",
          data->query ? data->query : "(null)", data->installed,
          data->alternate_of ? data->alternate_of : "(null)",
          data->max_results);

  if (data->alternate_of != NULL) {
    if (bail_if_cancelled(task, cancellable, list))
      return;
    /* seen_ids = NULL: every entry here intentionally shares the same GsApp
     * id (the app-id being queried) so they stack in the "Sources" popover —
     * deduping by id would collapse them to a single row. */
    collect_into(list, data, MODULIX_READ_ALTERNATES, FALSE, parser, NULL);

    /* Never fail this job on cancellation. gs-details-page shares one
     * alternate-of job across the whole "Sources" popover and hides it
     * entirely (Flatpak row included) on any error, cancellation included —
     * so a stale request cancelled by fast navigation must still return
     * whatever it has instead of poisoning the popover for the new page. */
    g_debug("[modulix] list_apps → %u apps", gs_app_list_length(list));
    g_task_return_pointer(task, list, g_object_unref);
    return;
  }

  if (data->installed) {
    if (!collect_pair(list, data, task, cancellable,
                      MODULIX_READ_INSTALLED_MODULES,
                      MODULIX_READ_INSTALLED_PACKAGES, TRUE, parser))
      return;
  } else if (data->query != NULL && *data->query != '\0') {
    if (!collect_pair(list, data, task, cancellable,
                      MODULIX_READ_SEARCH_MODULES, MODULIX_READ_SEARCH_PACKAGES,
                      FALSE, parser))
      return;
  }

  if (bail_if_cancelled(task, cancellable, list))
    return;

  g_debug("[modulix] list_apps → %u apps", gs_app_list_length(list));
  g_task_return_pointer(task, list, g_object_unref);
}

void gs_modulix_list_apps_async(GsPlugin *plugin, GsAppQuery *query,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data, gpointer source_tag) {
  GTask *task = g_task_new(plugin, cancellable, callback, user_data);
  g_task_set_source_tag(task, source_tag);

  GsApp *alt = NULL;
  GsAppQueryTristate installed_tristate = GS_APP_QUERY_TRISTATE_UNSET;
  const gchar *const *kws = NULL;
  guint max_results = 0;

  if (query != NULL) {
    alt = gs_app_query_get_alternate_of(query);
    installed_tristate = gs_app_query_get_is_installed(query);
    kws = gs_app_query_get_keywords(query);
    max_results = gs_app_query_get_max_results(query);
  }

  /* We only answer: keywords search, is_installed==TRUE, or alternate_of —
   * exactly one at a time. Everything else (overview/category/featured jobs
   * with none of our properties set) is rejected synchronously, mirroring
   * gs-plugin-packagekit.c, so we never spawn a thread for a query we cannot
   * answer. */
  gboolean supported = (alt != NULL) ||
                       (installed_tristate == GS_APP_QUERY_TRISTATE_TRUE) ||
                       (kws != NULL && kws[0] != NULL);

  if (query == NULL || !supported ||
      gs_app_query_get_n_properties_set(query) != 1) {
    g_debug("[modulix] list_apps → NOT_SUPPORTED");
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Unsupported query");
    g_object_unref(task);
    return;
  }

  ListData *data = g_new0(ListData, 1);
  data->plugin = plugin;
  data->max_results = (max_results > 0) ? max_results : MODULIX_SEARCH_LIMIT;

  if (alt != NULL)
    data->alternate_of = g_strdup(gs_app_get_id(alt));
  if (installed_tristate == GS_APP_QUERY_TRISTATE_TRUE)
    data->installed = TRUE;
  if (kws != NULL && kws[0] != NULL)
    data->query = g_strjoinv(" ", (gchar **)kws);

  g_task_set_task_data(task, data, (GDestroyNotify)list_data_free);
  g_task_run_in_thread(task, list_apps_thread);
  g_object_unref(task);
}

GsAppList *gs_modulix_list_apps_finish(GAsyncResult *result, GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}
