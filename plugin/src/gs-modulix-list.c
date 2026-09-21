/**
 * @file gs-modulix-list.c
 * @brief Implements the `list_apps` vfunc: search, installed listing, and the
 * details page's "Sources" (`alternate_of`) query.
 *
 * All three shapes boil down to the same three steps — one blocking
 * `mx_store_*` read, one JSON array appended to the result list, one debug
 * timing line — factored here into collect_into(). Every `mx_store_*` read
 * used here returns a JSON array string that must be freed with
 * `mx_store_free_string()`; NULL means the read failed (bus unreachable,
 * D-Bus error, or bad UTF-8 input) and is treated as "0 apps from this read"
 * rather than as a fatal error for the whole task (see collect_into()) — a
 * legitimate empty result set is instead the JSON text `"[]"`, which
 * `gs_modulix_append_apps_from_json()` also turns into 0 apps appended, with
 * no warning. Apps are appended to the `GsAppList` in the daemon's reply
 * order (relevance order for a search, since `Store1` does the ranking and
 * this file only appends in array order); a malformed individual JSON
 * element (missing/empty `"name"`) is skipped by
 * `gs_modulix_make_app_from_json()`, not fatal to the read.
 */

#include "gs-modulix-list.h"

#include "gs-modulix-app.h"
#include "gs-modulix-config.h"

#include "modulix-store-client.h"

/**
 * @brief Per-task private data for one `list_apps` `GTask`, set as its task
 * data and read back on the worker thread.
 *
 * @var ListData::plugin
 *   The `GsPlugin` driving this task. Borrowed: the task's source object
 *   (the same `GsPlugin`) outlives this struct, so no reference is taken.
 * @var ListData::query
 *   Joined keyword search string (space-separated), or NULL when this task
 *   is not a keyword search. Owned by this struct, freed by
 *   list_data_free().
 * @var ListData::alternate_of
 *   AppStream id to fetch nix/module variants for, or NULL when this task is
 *   not an `alternate_of` query. Owned by this struct, freed by
 *   list_data_free().
 * @var ListData::installed
 *   TRUE when this task is an `is_installed == TRUE` listing query.
 * @var ListData::max_results
 *   Upper bound forwarded to `mx_store_search_{modules,packages}`; taken
 *   from `GsAppQuery`'s own max-results when set, else
 *   `MODULIX_SEARCH_LIMIT`.
 */
typedef struct {
  GsPlugin *plugin; /* borrowed: the task's source object outlives the data */
  gchar *query;
  gchar *alternate_of;
  gboolean installed;
  guint max_results;
} ListData;

/**
 * @brief Frees a #ListData and everything it owns.
 *
 * @param d The struct to free. Not NULL.
 * @pre @p d was allocated with g_new0() and is not referenced elsewhere
 *   (i.e. the owning `GTask` is being destroyed).
 * @post @p d, @p d->query and @p d->alternate_of are freed; @p d is
 *   dangling.
 * @return None.
 */
static void list_data_free(ListData *d) {
  g_free(d->query);
  g_free(d->alternate_of);
  g_free(d);
}

/**
 * @brief Which Store1 read to issue for one collect_into() call.
 *
 * The shim's entry points differ in arity, so the dispatch is an enum rather
 * than a function pointer.
 *
 * - MODULIX_READ_SEARCH_MODULES: `mx_store_search_modules()`.
 * - MODULIX_READ_SEARCH_PACKAGES: `mx_store_search_packages()`.
 * - MODULIX_READ_INSTALLED_MODULES: `mx_store_list_installed_modules()`.
 * - MODULIX_READ_INSTALLED_PACKAGES: `mx_store_list_installed_packages()`.
 * - MODULIX_READ_ALTERNATES: `mx_store_packages_for_app_id()`.
 */
typedef enum {
  MODULIX_READ_SEARCH_MODULES,
  MODULIX_READ_SEARCH_PACKAGES,
  MODULIX_READ_INSTALLED_MODULES,
  MODULIX_READ_INSTALLED_PACKAGES,
  MODULIX_READ_ALTERNATES,
} ModulixRead;

/**
 * @brief Human-readable name of a #ModulixRead, for debug/log lines only.
 *
 * @param read The read kind to label.
 * @pre None.
 * @post None.
 * @return A static, non-NULL string constant naming the underlying
 *   `mx_store_*` call (e.g. `"search_modules"`). Transfer-none: must not be
 *   freed. Any unrecognised value (there is none among the enum's current
 *   members) falls through to the `MODULIX_READ_ALTERNATES` label, i.e.
 *   `"packages_for_app_id"`.
 */
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

/**
 * @brief Issues the one blocking `mx_store_*` Store1 call @p read selects.
 *
 * @param read Which read to perform; also selects which field of @p data is
 *   used as the call's argument (`data->query` for a search, none for an
 *   installed listing, `data->alternate_of` for `MODULIX_READ_ALTERNATES`).
 * @param data Task data supplying the call's argument(s) and
 *   `max_results`. Not NULL. Not mutated.
 * @pre The relevant field of @p data (`query` or `alternate_of`) is set
 *   when @p read needs it; the shim tolerates NULL there too (returning
 *   NULL itself) but the caller in this file never passes NULL for those
 *   reads.
 * @post None beyond the daemon round-trip; this call blocks the calling
 *   (worker) thread for its duration.
 * @return JSON array text (transfer-full: caller must free with
 *   `mx_store_free_string()`), or NULL if the call failed (bus unreachable,
 *   D-Bus error, or invalid UTF-8 argument). A query that legitimately
 *   matches nothing returns `"[]"`, not NULL.
 */
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

/**
 * @brief Runs one daemon read via store_read() and appends its apps to
 * @p list, timed and logged.
 *
 * Calls store_read(@p read, @p data), logs a `[modulix]` debug line with the
 * read's label, its query/alternate_of argument, and its wall-clock time,
 * then hands the JSON text to gs_modulix_append_apps_from_json() to decode
 * and append. If store_read() returned NULL (the read failed), nothing is
 * appended and no error is raised here — the failure is silently equivalent
 * to an empty result for this one read; the caller (list_apps_thread() /
 * collect_pair()) does not distinguish "read failed" from "read found
 * nothing". A malformed JSON array, or an individual element missing a
 * usable `"name"`, is likewise absorbed by
 * gs_modulix_append_apps_from_json() (a g_warning may be logged for a parse
 * failure, but nothing here escalates it).
 *
 * @param list GsAppList apps are appended to, in the JSON array's order
 *   (the daemon's own relevance order for a search). Not NULL. Mutated.
 * @param data Task data supplying store_read()'s argument(s) and used here
 *   only for its `query`/`alternate_of` strings in the debug line. Not
 *   NULL. Not mutated.
 * @param read Which Store1 read to perform (see store_read()).
 * @param is_installed Fallback installed state forwarded to
 *   gs_modulix_append_apps_from_json() (used only for entries whose JSON
 *   predates the `"installed"` field).
 * @param parser JsonParser reused across every call in the current task
 *   (transfer-none, its parse tree is overwritten by this call). Not NULL.
 * @param seen_ids Nullable id-dedup table forwarded to
 *   gs_modulix_append_apps_from_json(); NULL disables dedup (required on
 *   the `alternate_of` path, where rows intentionally share one id).
 * @pre @p list, @p data and @p parser are valid, live objects.
 * @post @p list holds zero or more additional apps, in the order the daemon
 *   returned them; @p seen_ids (when non-NULL) gained one entry per newly
 *   appended app; the JSON string returned by store_read() (if any) has
 *   been freed with `mx_store_free_string()`.
 * @return None.
 */
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

/**
 * @brief Checks @p cancellable and, if cancelled, completes @p task with the
 * cancellation error.
 *
 * @param task GTask to complete with an error when cancelled. Not NULL.
 * @param cancellable GCancellable to check, or NULL (then this always
 *   returns FALSE).
 * @param list_to_free The in-progress GsAppList to discard when cancelled,
 *   or NULL. Transfer-full on the cancelled path: unreffed via
 *   g_clear_object() before this returns. @p list_to_free is passed by
 *   value, so only this function's local copy of the pointer is cleared —
 *   the caller's own variable is left dangling and must not be
 *   dereferenced again; this is why callers are required to return
 *   immediately on TRUE.
 * @pre None.
 * @post When cancelled: the GsAppList @p list_to_free pointed to (if
 *   non-NULL) has been unreffed (freed, if that was the last reference);
 *   @p task has been completed with a `G_IO_ERROR_CANCELLED`-class GError
 *   via g_task_return_error(). When not cancelled: no side effects.
 * @return TRUE if @p cancellable had been cancelled (and @p task was just
 *   completed with an error) — callers must return immediately without
 *   touching @p task or the list they passed as @p list_to_free again;
 *   FALSE otherwise, meaning the caller should proceed.
 */
static gboolean bail_if_cancelled(GTask *task, GCancellable *cancellable,
                                  GsAppList *list_to_free) {
  g_autoptr(GError) err = NULL;
  if (!g_cancellable_set_error_if_cancelled(cancellable, &err))
    return FALSE;
  g_clear_object(&list_to_free);
  g_task_return_error(task, g_steal_pointer(&err));
  return TRUE;
}

/**
 * @brief Runs a modules-then-packages pair of collect_into() calls, deduped
 * against each other by GsApp id.
 *
 * Modules are always collected before packages, sharing one seen_ids table:
 * intra-plugin dedup then keeps the module and drops a package with the same
 * GsApp id, so a module wins its own dedup regardless of list order
 * downstream (search-page/installed-page ordering is a separate concern,
 * decided later by match-value / priority — see gs-modulix-app.c). Either
 * half is compiled out entirely when its `MODULIX_ENABLE_{MODULES,PACKAGES}`
 * flag (gs-modulix-config.h) is off.
 *
 * @param list GsAppList apps are appended to: modules first, then packages,
 *   each half in the daemon's own reply order. Not NULL. Mutated.
 * @param data Task data forwarded to collect_into(). Not NULL. Not mutated.
 * @param task GTask checked for cancellation before each half and completed
 *   with an error (via bail_if_cancelled()) if cancelled. Not NULL.
 * @param cancellable GCancellable checked before each half, or NULL.
 * @param modules Which #ModulixRead to run for the modules half (one of the
 *   `MODULIX_READ_*_MODULES` values).
 * @param packages Which #ModulixRead to run for the packages half (one of
 *   the `MODULIX_READ_*_PACKAGES` values).
 * @param is_installed Fallback installed state forwarded to both
 *   collect_into() calls.
 * @param parser JsonParser reused for both calls (transfer-none). Not NULL.
 * @pre None beyond those of collect_into() and bail_if_cancelled().
 * @post On success: @p list holds the modules' apps followed by the
 *   packages' apps, deduped against each other by id (a package sharing a
 *   module's id is dropped). On cancellation: @p task has been completed
 *   with an error and @p list has already been unreffed by
 *   bail_if_cancelled() — the caller must not touch either again.
 * @return TRUE if both (enabled) halves ran to completion; FALSE if
 *   @p cancellable was cancelled partway through, in which case @p task has
 *   already been completed with an error and the caller must return
 *   immediately.
 */
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

/**
 * @brief Worker-thread body of gs_modulix_list_apps_async(): runs the one
 * `mx_store_*` read (or pair of reads) the task data selects and completes
 * @p task with the resulting GsAppList.
 *
 * Dispatches on `data->alternate_of`/`data->installed`/`data->query` (set by
 * gs_modulix_list_apps_async(), mutually exclusive by construction) to
 * exactly one of: a single MODULIX_READ_ALTERNATES collect_into() call, an
 * installed-modules+packages collect_pair(), or a search-modules+packages
 * collect_pair(). Neither `installed` nor a non-empty `query` being set
 * (i.e. an `alternate_of`-less, non-installed, keyword-less task, which
 * gs_modulix_list_apps_async() should never actually construct) falls
 * through with an empty list rather than reading anything.
 *
 * @param task The GTask to run and complete. Not NULL.
 * @param source_object Unused (the source object — the GsPlugin — is
 *   reached via @p task itself where needed).
 * @param task_data_ptr The task's #ListData, as set by
 *   g_task_set_task_data() in gs_modulix_list_apps_async(). Not NULL. Not
 *   mutated.
 * @param cancellable GCancellable checked before/between every daemon read.
 * @pre Runs on a GLib worker-pool thread (via `g_task_run_in_thread()`),
 *   never the caller's thread.
 * @post @p task has been completed exactly once: with a new, transfer-full
 *   GsAppList (possibly empty — an empty list is the normal outcome for "no
 *   results", not an error) via g_task_return_pointer(), or with a
 *   cancellation GError via bail_if_cancelled() — except on the
 *   `alternate_of` path, which is documented to never fail the task on
 *   cancellation, always returning whatever it collected instead (see the
 *   `alternate_of` branch's own comment and gs_modulix_list_apps_async()).
 * @return None.
 */
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
