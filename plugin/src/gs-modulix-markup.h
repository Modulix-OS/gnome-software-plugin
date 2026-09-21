/**
 * @file gs-modulix-markup.h
 * @brief Converts the AppStream HTML description carried by Modulix JSON into
 *        the Pango markup GNOME Software renders.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * @brief Renders an AppStream HTML description as Pango markup.
 *
 * Handles the subset AppStream allows: `<p>`, `<ul>`, `<ol>` and `<li>` as
 * blocks, `<em>`/`<i>`, `<strong>`/`<b>` and `<code>` inline. Any other tag has
 * its text content kept but its markup dropped, so an unexpected tag degrades
 * to plain text instead of leaking into the output. Text sitting outside any
 * block element is discarded, AppStream requiring it to live in a `<p>` or an
 * `<li>`.
 *
 * @param html The description as the daemon supplied it (Flathub AppStream
 *   HTML). May be NULL or empty.
 * @pre None: malformed or truncated markup is tolerated, never rejected.
 * @post No global state is touched.
 * @return Freshly allocated Pango markup (transfer-full: free with g_free()).
 *   `&` and `<` in text are escaped, so a description cannot inject markup of
 *   its own; NULL or empty input yields an empty string, never NULL.
 */
gchar *gs_modulix_html_to_pango(const gchar *html);

G_END_DECLS
