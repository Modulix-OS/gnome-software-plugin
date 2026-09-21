/**
 * @file gs-modulix-json-utils.h
 * @brief Total accessors over the JSON the daemon sends through the
 *        store-client shim.
 *
 * Every payload is produced by `modulix-store-client` out of typed D-Bus rows,
 * so a missing member or a wrong type is a bug on the producer side rather
 * than user input. These helpers therefore never fail: they substitute a
 * neutral value and let the caller carry on, which keeps the parsing sites
 * free of error plumbing.
 */

#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * @brief Reads a string member of a JSON object.
 *
 * @param obj Object to read from. Not NULL.
 * @param key Member name to read.
 * @pre @p obj is a live JsonObject.
 * @post None.
 * @return The member's string value, borrowed from @p obj's parse tree
 *   (transfer-none: valid only while the owning JsonParser keeps this tree).
 *   The empty string `""` when the member is absent, is JSON null, or is not
 *   a scalar — never NULL, so the result is always safe to pass on. A scalar
 *   of another type (number, boolean) yields NULL from json-glib and is
 *   returned as such.
 */
const gchar *gs_modulix_json_str(JsonObject *obj, const gchar *key);

/**
 * @brief Reads a boolean member of a JSON object.
 *
 * @param obj Object to read from. Not NULL.
 * @param key Member name to read.
 * @pre @p obj is a live JsonObject.
 * @post None.
 * @return The member's boolean value; FALSE when the member is absent, is
 *   JSON null, or is not a scalar. An absent member and an explicit `false`
 *   are therefore indistinguishable.
 */
gboolean     gs_modulix_json_bool(JsonObject *obj, const gchar *key);

/**
 * @brief Reads an integer member of a JSON object.
 *
 * @param obj Object to read from. Not NULL.
 * @param key Member name to read.
 * @pre @p obj is a live JsonObject.
 * @post None.
 * @return The member's integer value, narrowed from json-glib's gint64 to
 *   gint; 0 when the member is absent, is JSON null, or is not a scalar. An
 *   absent member and an explicit `0` are therefore indistinguishable.
 */
gint         gs_modulix_json_int(JsonObject *obj, const gchar *key);

/**
 * @brief Parses a JSON payload whose root must be an object.
 *
 * @param parser JsonParser to decode into, reused across payloads by the
 *   caller (transfer-none: the caller owns and frees it). Not NULL. Its
 *   previous parse tree is discarded by this call, which invalidates every
 *   string still borrowed from it.
 * @param json Raw JSON text, typically a string returned by an `mx_store_*`
 *   call. NULL and the empty string are accepted and yield NULL.
 * @param ctx Short label naming the call site; used only in the g_warning
 *   emitted on a parse error, never looked up as a member name.
 * @pre @p parser is a live JsonParser, and no string borrowed from its
 *   previous payload is still in use.
 * @post @p parser holds the parse tree of @p json, which owns every string
 *   the returned object exposes.
 * @return The root object, borrowed from @p parser (transfer-none); NULL when
 *   @p json is NULL or empty, fails to parse (a warning is logged), or decodes
 *   to something other than an object (silently, no warning).
 */
JsonObject *gs_modulix_json_parse_object(JsonParser *parser, const gchar *json,
                                         const gchar *ctx);

/**
 * @brief Parses a JSON payload whose root must be an array.
 *
 * @param parser JsonParser to decode into, as in
 *   gs_modulix_json_parse_object(). Not NULL.
 * @param json Raw JSON text; NULL and the empty string yield NULL.
 * @param ctx Short label naming the call site, used only in the parse-error
 *   g_warning.
 * @pre @p parser is a live JsonParser, and no string borrowed from its
 *   previous payload is still in use.
 * @post @p parser holds the parse tree of @p json.
 * @return The root array, borrowed from @p parser (transfer-none); NULL when
 *   @p json is NULL or empty, fails to parse (warning logged), or decodes to
 *   something other than an array (silently). An empty array is returned as
 *   such and is not an error.
 */
JsonArray  *gs_modulix_json_parse_array(JsonParser *parser, const gchar *json,
                                        const gchar *ctx);

G_END_DECLS
