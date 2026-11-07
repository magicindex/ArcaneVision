#ifndef XL_MARKDOWN_H
#define XL_MARKDOWN_H

#include <stddef.h>

/* Render Markdown to a safe HTML fragment. Raw HTML is escaped; only
 * generated tags and http(s)/mailto links are emitted. Returns a malloc'd,
 * NUL-terminated string, or NULL on allocation failure. */
char *md_render_html(const char *src, size_t len, size_t *out_len);

#endif
