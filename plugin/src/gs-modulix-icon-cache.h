#pragma once

#include <glib.h>

G_BEGIN_DECLS

void gs_modulix_icon_cache_put(const gchar *app_id, const gchar *url);
gchar *gs_modulix_icon_cache_get(const gchar *app_id);
void gs_modulix_icon_cache_clear(void);

G_END_DECLS
