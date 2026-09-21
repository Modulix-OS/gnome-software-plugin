/**
 * @file gs-modulix-markup.c
 * @brief AppStream HTML → Pango markup converter.
 *
 * Mirrors the logic of gs_appstream_format_description_text() in
 * lib/gs-appstream.c, adapted for a pre-serialised HTML string coming
 * out of the Modulix JSON backend instead of a live XbNode tree.
 *
 * Supported block tags  : <p>, <ul>, <ol>, <li>
 * Supported inline tags : <em>, <i>, <strong>, <b>, <code>
 * Everything else       : content consumed, no markup emitted.
 */

#include "gs-modulix-markup.h"

#include <string.h>

/**
 * @brief Reads a tag name out of a `<...>` span.
 *
 * @param pos First character of the name, i.e. just past `<` or `</`.
 * @param tag_end Position of the closing `>`, used as the hard stop.
 * @param buf Destination buffer, NUL-terminated on return. Not NULL.
 * @param buf_size Size of @p buf in bytes, including the terminator.
 * @pre @p pos and @p tag_end point into the same string, with `pos <= tag_end`.
 * @post @p buf always holds a NUL-terminated string, empty when 0 is returned.
 * @return Number of bytes written, or 0 when the name is empty or does not fit
 *   in @p buf — the caller treats both as "not a tag I handle". Reading stops
 *   at a space, a tab or a `/`, so attributes are left out of the name.
 */
static gsize extract_tag_name(const gchar *pos, const gchar *tag_end,
                              gchar *buf, gsize buf_size) {
  gsize n = 0;
  while (pos < tag_end && *pos != ' ' && *pos != '\t' && *pos != '/') {
    if (n + 1 >= buf_size)
      return 0;
    buf[n++] = *pos++;
  }
  buf[n] = '\0';
  return n;
}

/**
 * @brief Converts the inline content of one block element into Pango markup.
 *
 * Recursive; mirrors gs_appstream_format_description_text(). Known inline
 * tags (`<em>`/`<i>`, `<strong>`/`<b>`, `<code>`) are wrapped in their Pango
 * equivalent; any other tag has its content emitted verbatim, with no
 * wrapper markup, so an unrecognized tag never loses its text.
 *
 * @param out Buffer the markup is appended to. Not NULL. Mutated.
 * @param p Cursor into the HTML, advanced as the content is consumed and left
 *   just past the closing `</tag_name>`. Not NULL.
 * @param tag_name Name of the enclosing element, which tells the recursion
 *   which closing tag ends its own content.
 * @pre `*p` points at inline content, i.e. just past the opening tag of
 *   @p tag_name.
 * @post @p out has gained the converted markup and `*p` has advanced; on
 *   truncated input the cursor stops at the NUL, which the caller treats as
 *   end of input rather than an error.
 * @return None.
 */
static void format_inline(GString *out, const gchar **p,
                          const gchar *tag_name) {
  while (**p) {
    if (**p != '<') {
      switch (**p) {
      case '&':
        g_string_append(out, "&amp;");
        break;
      case '<':
        g_string_append(out, "&lt;");
        break;
      case '>':
        g_string_append(out, "&gt;");
        break;
      case '"':
        g_string_append(out, "&quot;");
        break;
      default:
        g_string_append_c(out, **p);
        break;
      }
      (*p)++;
      continue;
    }

    const gchar *tag_end = strchr(*p + 1, '>');
    if (tag_end == NULL) {
      g_string_append(out, "&lt;");
      (*p)++;
      continue;
    }

    gboolean closing = ((*p)[1] == '/');
    const gchar *name_start = *p + 1 + (closing ? 1 : 0);
    gchar tbuf[32];
    gsize tlen = extract_tag_name(name_start, tag_end, tbuf, sizeof(tbuf));

    *p = tag_end + 1;

    if (tlen == 0)
      continue;

    if (closing && g_ascii_strcasecmp(tbuf, tag_name) == 0)
      return;

    if (closing)
      continue;

    if (g_ascii_strcasecmp(tbuf, "em") == 0 ||
        g_ascii_strcasecmp(tbuf, "i") == 0) {
      g_string_append(out, "<i>");
      format_inline(out, p, tbuf);
      g_string_append(out, "</i>");
    } else if (g_ascii_strcasecmp(tbuf, "strong") == 0 ||
               g_ascii_strcasecmp(tbuf, "b") == 0) {
      g_string_append(out, "<b>");
      format_inline(out, p, tbuf);
      g_string_append(out, "</b>");
    } else if (g_ascii_strcasecmp(tbuf, "code") == 0) {
      g_string_append(out, "<tt>");
      format_inline(out, p, tbuf);
      g_string_append(out, "</tt>");
    } else {
      format_inline(out, p, tbuf);
    }
  }
}

/**
 * @brief Converts a subset of AppStream HTML to Pango markup suitable for
 *        gs_app_set_description().
 *
 * Supported block tags: `<p>`, `<ul>`, `<ol>`, `<li>`; supported inline tags:
 * `<em>`/`<i>`, `<strong>`/`<b>`, `<code>` (see format_inline()). Top-level
 * text outside a block element — not inside `<p>` or `<li>` — is dropped, since
 * AppStream descriptions are expected to keep all of their text inside a block
 * tag. An ordered list is rendered as `1. `, `2. `, … and an unordered one with
 * a bullet, since Pango markup has no list construct of its own.
 *
 * @param html An AppStream HTML description string, or NULL.
 * @pre None: malformed or truncated markup is tolerated, never rejected.
 * @post No global state is touched.
 * @return A newly allocated Pango markup string, stripped of leading and
 *   trailing whitespace (transfer-full: free with g_free()). An empty string
 *   when @p html is NULL or empty — never NULL. `&` and `<` in text are
 *   escaped, so a description cannot inject markup of its own.
 */
gchar *gs_modulix_html_to_pango(const gchar *html) {
  if (html == NULL || *html == '\0')
    return g_strdup("");

  GString *out = g_string_sized_new(strlen(html));
  const gchar *p = html;
  gboolean in_list = FALSE;
  gboolean ordered = FALSE;
  guint list_counter = 0;

  while (*p) {
    if (*p != '<') {
      p++;
      continue;
    }

    const gchar *tag_end = strchr(p + 1, '>');
    if (tag_end == NULL) {
      p++;
      continue;
    }

    gboolean closing = (p[1] == '/');
    const gchar *name_start = p + 1 + (closing ? 1 : 0);
    gchar tbuf[16];
    gsize tlen = extract_tag_name(name_start, tag_end, tbuf, sizeof(tbuf));

    p = tag_end + 1;

    if (tlen == 0)
      continue;

    if (g_ascii_strcasecmp(tbuf, "p") == 0 && !closing) {
      if (out->len > 0)
        g_string_append_c(out, '\n');
      format_inline(out, &p, "p");
    }

    else if (g_ascii_strcasecmp(tbuf, "ul") == 0 ||
             g_ascii_strcasecmp(tbuf, "ol") == 0) {
      if (!closing) {
        in_list = TRUE;
        ordered = (g_ascii_strcasecmp(tbuf, "ol") == 0);
        list_counter = 0;
      } else {
        in_list = FALSE;
      }
    }

    else if (g_ascii_strcasecmp(tbuf, "li") == 0 && !closing && in_list) {
      if (out->len > 0)
        g_string_append_c(out, '\n');
      if (ordered)
        g_string_append_printf(out, "%u. ", ++list_counter);
      else
        g_string_append(out, "\xe2\x80\xa2 ");
      format_inline(out, &p, "li");
    }
  }

  gchar *result = g_string_free(out, FALSE);
  g_strstrip(result);
  return result;
}
