/*
 * gs-modulix-markup.c — AppStream HTML → Pango markup converter
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

/* ── helpers ────────────────────────────────────────────────────────────── */

/* Extract the tag name starting at *pos (which points just past '<' or '</').
 * Writes at most buf_size-1 bytes into buf and NUL-terminates.
 * Returns the length written (0 on overflow or empty). */
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

/* ── inline formatter (recursive, mirrors gs_appstream_format_description_text)
 *
 * Appends the Pango-markup representation of the inline content beginning at
 * **p to `out`.  Stops (and advances *p past the closing tag) when it
 * encounters </tag_name>.  Unknown inline tags have their content emitted
 * verbatim (no wrapper markup). */
static void format_inline(GString *out, const gchar **p,
                          const gchar *tag_name) {
  while (**p) {
    if (**p != '<') {
      /* Plain text: escape for Pango. */
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

    /* Our own closing tag → return to caller. */
    if (closing && g_ascii_strcasecmp(tbuf, tag_name) == 0)
      return;

    if (closing)
      continue; /* stray closing tag, ignore */

    /* Known inline wrappers. */
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
      /* Unknown tag: consume its content without any wrapper. */
      format_inline(out, p, tbuf);
    }
  }
}

/* ── public API ─────────────────────────────────────────────────────────── */

/**
 * gs_modulix_html_to_pango:
 * @html: (nullable): an AppStream HTML description string
 *
 * Converts a subset of AppStream HTML to Pango markup suitable for passing
 * to gs_app_set_description().
 *
 * Returns: (transfer full): a newly allocated Pango markup string,
 *   or an empty string if @html is %NULL or empty.
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
      /* Top-level text outside block elements: skip (AppStream
       * descriptions must live inside <p> or <li>). */
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

    /* ── <p> ── */
    if (g_ascii_strcasecmp(tbuf, "p") == 0 && !closing) {
      if (out->len > 0)
        g_string_append_c(out, '\n');
      format_inline(out, &p, "p");
    }

    /* ── <ul> / <ol> ── */
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

    /* ── <li> ── */
    else if (g_ascii_strcasecmp(tbuf, "li") == 0 && !closing && in_list) {
      if (out->len > 0)
        g_string_append_c(out, '\n');
      if (ordered)
        g_string_append_printf(out, "%u. ", ++list_counter);
      else
        g_string_append(out, "\xe2\x80\xa2 "); /* U+2022 BULLET */
      format_inline(out, &p, "li");
    }
  }

  gchar *result = g_string_free(out, FALSE);
  g_strstrip(result);
  return result;
}
