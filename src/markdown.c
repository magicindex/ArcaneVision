/* 轻量 Markdown 渲染器（无第三方依赖）。
 *
 * 安全边界：源文件中的原始 HTML 一律转义，不执行脚本；链接只接受
 * http(s)、mailto 和站内相对地址，图片只接受 http(s)。因此输出可以
 * 直接作为 HTML 预览，不需要再依赖浏览器的清洗器。
 */
#include "markdown.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    char  *p;
    size_t n, cap;
    int    err;
} mbuf;

static void mb_reserve(mbuf *b, size_t extra) {
    if (b->err) return;
    if (extra > SIZE_MAX - b->n - 1) { b->err = 1; return; }
    size_t need = b->n + extra + 1;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    char *p = realloc(b->p, cap);
    if (!p) { b->err = 1; return; }
    b->p = p;
    b->cap = cap;
}

static void mb_add(mbuf *b, const char *s, size_t n) {
    if (!n || b->err) return;
    mb_reserve(b, n);
    if (b->err) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void mb_puts(mbuf *b, const char *s) { mb_add(b, s, strlen(s)); }

typedef struct { const char *p, *end; } mcur;
typedef struct { const char *s; size_t n; } mspan;

static int m_next(mcur *c, mspan *l) {
    if (c->p >= c->end) return 0;
    const char *start = c->p;
    const char *nl = memchr(start, '\n', (size_t)(c->end - start));
    const char *end = nl ? nl : c->end;
    if (end > start && end[-1] == '\r') end--;
    l->s = start;
    l->n = (size_t)(end - start);
    c->p = nl ? nl + 1 : c->end;
    return 1;
}

static int m_peek(mcur c, mspan *l) { return m_next(&c, l); }

static mspan m_trim(mspan v) {
    size_t a = 0, b = v.n;
    while (a < b && (v.s[a] == ' ' || v.s[a] == '\t')) a++;
    while (b > a && (v.s[b - 1] == ' ' || v.s[b - 1] == '\t')) b--;
    return (mspan){v.s + a, b - a};
}

static int m_find(const char *s, size_t from, size_t n,
                  const char *pat, size_t pn, size_t *at) {
    if (!pn || from > n || pn > n) return 0;
    for (size_t i = from; i + pn <= n; i++) {
        if (memcmp(s + i, pat, pn) == 0) { *at = i; return 1; }
    }
    return 0;
}

static int m_find_run(const char *s, size_t from, size_t n,
                      char ch, size_t count, size_t *at) {
    for (size_t i = from; i < n; i++) {
        if (s[i] != ch) continue;
        size_t k = 1;
        while (i + k < n && s[i + k] == ch) k++;
        if (k >= count) { *at = i; return 1; }
        i += k - 1;
    }
    return 0;
}

static void m_escape(mbuf *o, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '&': mb_puts(o, "&amp;"); break;
        case '<': mb_puts(o, "&lt;"); break;
        case '>': mb_puts(o, "&gt;"); break;
        case '"': mb_puts(o, "&quot;"); break;
        case '\'': mb_puts(o, "&#39;"); break;
        default: mb_add(o, s + i, 1); break;
        }
    }
}

static int m_safe_url(const char *s, size_t n, int image) {
    while (n && isspace((unsigned char)*s)) { s++; n--; }
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (n >= 7 && strncasecmp(s, "http://", 7) == 0) return 1;
    if (n >= 8 && strncasecmp(s, "https://", 8) == 0) return 1;
    if (!image && n >= 7 && strncasecmp(s, "mailto:", 7) == 0) return 1;
    /* 站内相对链接是安全的；拒绝 //host 形式，避免被当成外链。 */
    if (!image && n && (s[0] == '#' || (s[0] == '/' && (n < 2 || s[1] != '/')))) return 1;
    return 0;
}

static int m_blank(mspan l) { return m_trim(l).n == 0; }

static int m_fence(mspan l, char *ch, size_t *cnt, mspan *info) {
    size_t i = 0;
    while (i < l.n && i < 3 && l.s[i] == ' ') i++;
    if (i >= l.n || (l.s[i] != '`' && l.s[i] != '~')) return 0;
    char c = l.s[i];
    size_t n = 0;
    while (i + n < l.n && l.s[i + n] == c) n++;
    if (n < 3) return 0;
    *ch = c;
    *cnt = n;
    *info = m_trim((mspan){l.s + i + n, l.n - i - n});
    return 1;
}

static int m_fence_end(mspan l, char ch, size_t cnt) {
    size_t i = 0;
    while (i < l.n && i < 3 && l.s[i] == ' ') i++;
    size_t n = 0;
    while (i + n < l.n && l.s[i + n] == ch) n++;
    if (n < cnt) return 0;
    for (size_t k = i + n; k < l.n; k++)
        if (!isspace((unsigned char)l.s[k])) return 0;
    return 1;
}

static int m_heading(mspan l, int *level, mspan *content) {
    size_t i = 0;
    while (i < l.n && i < 3 && l.s[i] == ' ') i++;
    size_t n = 0;
    while (i + n < l.n && l.s[i + n] == '#') n++;
    if (n < 1 || n > 6) return 0;
    if (i + n < l.n && l.s[i + n] != ' ' && l.s[i + n] != '\t') return 0;
    *level = (int)n;
    *content = m_trim((mspan){l.s + i + n, l.n - i - n});
    return 1;
}

static int m_hr(mspan l) {
    l = m_trim(l);
    if (l.n < 3) return 0;
    char c = l.s[0];
    if (c != '-' && c != '*' && c != '_') return 0;
    int count = 0;
    for (size_t i = 0; i < l.n; i++) {
        if (l.s[i] == c) count++;
        else if (l.s[i] != ' ' && l.s[i] != '\t') return 0;
    }
    return count >= 3;
}

static int m_quote(mspan l, size_t *off) {
    size_t i = 0;
    while (i < l.n && i < 3 && l.s[i] == ' ') i++;
    if (i >= l.n || l.s[i] != '>') return 0;
    i++;
    if (i < l.n && l.s[i] == ' ') i++;
    *off = i;
    return 1;
}

static int m_ul(mspan l, size_t *off) {
    size_t i = 0;
    while (i < l.n && i < 3 && l.s[i] == ' ') i++;
    if (i >= l.n || (l.s[i] != '-' && l.s[i] != '*' && l.s[i] != '+')) return 0;
    if (i + 1 < l.n && l.s[i + 1] != ' ' && l.s[i + 1] != '\t') return 0;
    i++;
    while (i < l.n && (l.s[i] == ' ' || l.s[i] == '\t')) i++;
    *off = i;
    return 1;
}

static int m_ol(mspan l, size_t *off) {
    size_t i = 0;
    while (i < l.n && i < 3 && l.s[i] == ' ') i++;
    size_t start = i;
    while (i < l.n && i - start < 9 && isdigit((unsigned char)l.s[i])) i++;
    if (i == start || i >= l.n || l.s[i] != '.') return 0;
    i++;
    if (i < l.n && l.s[i] != ' ' && l.s[i] != '\t') return 0;
    while (i < l.n && (l.s[i] == ' ' || l.s[i] == '\t')) i++;
    *off = i;
    return 1;
}

static int m_delim(mspan l) {
    l = m_trim(l);
    if (!l.n) return 0;
    if (l.s[0] == '|') { l.s++; l.n--; }
    l = m_trim(l);
    if (l.n && l.s[l.n - 1] == '|') l.n--;
    l = m_trim(l);
    if (!l.n) return 0;
    size_t pos = 0;
    int cells = 0;
    while (pos <= l.n) {
        size_t start = pos;
        while (pos < l.n && l.s[pos] != '|') pos++;
        mspan c = m_trim((mspan){l.s + start, pos - start});
        if (!c.n) {
            if (pos < l.n) return 0;
        } else {
            size_t a = 0, b = c.n;
            if (c.s[0] == ':') a++;
            if (b > a && c.s[b - 1] == ':') b--;
            if (b - a < 3) return 0;
            for (size_t k = a; k < b; k++)
                if (c.s[k] != '-') return 0;
            cells++;
        }
        if (pos >= l.n) break;
        pos++;
        if (pos == l.n) break;
    }
    return cells > 0;
}

static mspan m_strip_pipes(mspan l);

static int m_cells(mspan l) {
    l = m_strip_pipes(l);
    if (!l.n) return 0;
    int n = 1;
    for (size_t i = 0; i < l.n; i++) if (l.s[i] == '|') n++;
    return n;
}

static mspan m_strip_pipes(mspan l) {
    l = m_trim(l);
    if (l.n && l.s[0] == '|') { l.s++; l.n--; }
    l = m_trim(l);
    if (l.n && l.s[l.n - 1] == '|') l.n--;
    return m_trim(l);
}

static int m_cell_next(mspan l, size_t *pos, mspan *cell) {
    if (*pos > l.n) return 0;
    size_t start = *pos;
    while (start < l.n && l.s[start] != '|') start++;
    *cell = m_trim((mspan){l.s + *pos, start - *pos});
    *pos = start < l.n ? start + 1 : l.n + 1;
    return 1;
}

static int m_table_at(mcur c) {
    mspan h, d;
    if (!m_peek(c, &h)) return 0;
    if (!memchr(h.s, '|', h.n)) return 0;
    mcur t = c;
    if (!m_next(&t, &h) || !m_next(&t, &d)) return 0;
    return m_delim(d) && m_cells(h) == m_cells(d);
}

static void m_inline(mbuf *o, const char *s, size_t n, int depth);

static void m_inline(mbuf *o, const char *s, size_t n, int depth) {
    if (depth > 8) { m_escape(o, s, n); return; }
    size_t i = 0;
    while (i < n) {
        char c = s[i];

        if (c == '\\' && i + 1 < n && strchr("\\`*_{}[]()#+-.!|>~", s[i + 1])) {
            m_escape(o, s + i + 1, 1);
            i += 2;
            continue;
        }
        if (c == '\n') { mb_puts(o, "<br>"); i++; continue; }
        if (c == '\r') { i++; continue; }

        if (c == '`') {
            size_t ticks = 1;
            while (i + ticks < n && s[i + ticks] == '`') ticks++;
            size_t close;
            if (m_find_run(s, i + ticks, n, '`', ticks, &close)) {
                mb_puts(o, "<code>");
                m_escape(o, s + i + ticks, close - i - ticks);
                mb_puts(o, "</code>");
                i = close + ticks;
                continue;
            }
            m_escape(o, s + i, ticks);
            i += ticks;
            continue;
        }

        if (c == '!' && i + 1 < n && s[i + 1] == '[') {
            size_t close, end;
            if (m_find(s, i + 2, n, "]", 1, &close) && close + 1 < n &&
                s[close + 1] == '(' && m_find(s, close + 2, n, ")", 1, &end)) {
                mspan url = m_trim((mspan){s + close + 2, end - close - 2});
                if (m_safe_url(url.s, url.n, 1)) {
                    mb_puts(o, "<img src=\"");
                    m_escape(o, url.s, url.n);
                    mb_puts(o, "\" alt=\"");
                    m_escape(o, s + i + 2, close - i - 2);
                    mb_puts(o, "\" loading=\"lazy\">");
                } else {
                    m_escape(o, s + i + 2, close - i - 2);
                }
                i = end + 1;
                continue;
            }
        }

        if (c == '[') {
            size_t close, end;
            if (m_find(s, i + 1, n, "]", 1, &close) && close + 1 < n &&
                s[close + 1] == '(' && m_find(s, close + 2, n, ")", 1, &end)) {
                mspan url = m_trim((mspan){s + close + 2, end - close - 2});
                if (m_safe_url(url.s, url.n, 0)) {
                    mb_puts(o, "<a href=\"");
                    m_escape(o, url.s, url.n);
                    mb_puts(o, "\" target=\"_blank\" rel=\"noopener noreferrer\">");
                    m_inline(o, s + i + 1, close - i - 1, depth + 1);
                    mb_puts(o, "</a>");
                } else {
                    m_inline(o, s + i + 1, close - i - 1, depth + 1);
                }
                i = end + 1;
                continue;
            }
        }

        if (c == '*' && i + 1 < n && s[i + 1] == '*') {
            size_t close;
            if (m_find(s, i + 2, n, "**", 2, &close) && close > i + 2) {
                mb_puts(o, "<strong>");
                m_inline(o, s + i + 2, close - i - 2, depth + 1);
                mb_puts(o, "</strong>");
                i = close + 2;
                continue;
            }
        }
        if (c == '_' && i + 1 < n && s[i + 1] == '_') {
            size_t close;
            if (m_find(s, i + 2, n, "__", 2, &close) && close > i + 2) {
                mb_puts(o, "<strong>");
                m_inline(o, s + i + 2, close - i - 2, depth + 1);
                mb_puts(o, "</strong>");
                i = close + 2;
                continue;
            }
        }
        if (c == '~' && i + 1 < n && s[i + 1] == '~') {
            size_t close;
            if (m_find(s, i + 2, n, "~~", 2, &close) && close > i + 2) {
                mb_puts(o, "<del>");
                m_inline(o, s + i + 2, close - i - 2, depth + 1);
                mb_puts(o, "</del>");
                i = close + 2;
                continue;
            }
        }
        if (c == '*' || c == '_') {
            size_t close;
            if (m_find(s, i + 1, n, c == '*' ? "*" : "_", 1, &close) &&
                close > i + 1 && !isspace((unsigned char)s[i + 1]) &&
                !isspace((unsigned char)s[close - 1])) {
                mb_puts(o, "<em>");
                m_inline(o, s + i + 1, close - i - 1, depth + 1);
                mb_puts(o, "</em>");
                i = close + 1;
                continue;
            }
        }

        m_escape(o, s + i, 1);
        i++;
    }
}

static int m_block_start(mcur c) {
    mspan l;
    if (!m_peek(c, &l) || m_blank(l)) return 1;
    char ch;
    size_t n;
    mspan info;
    int lvl;
    size_t off;
    if (m_fence(l, &ch, &n, &info)) return 1;
    if (m_heading(l, &lvl, &info)) return 1;
    if (m_hr(l)) return 1;
    if (m_quote(l, &off)) return 1;
    if (m_ul(l, &off) || m_ol(l, &off)) return 1;
    if (m_table_at(c)) return 1;
    return 0;
}

static void m_blocks(mbuf *o, mcur *c, int depth);

static void m_list(mbuf *o, mcur *c, int ordered, int depth) {
    mb_puts(o, ordered ? "<ol>" : "<ul>");
    while (1) {
        mspan l, next;
        size_t off = 0, noff = 0;
        if (!m_peek(*c, &l)) break;
        if (ordered ? !m_ol(l, &off) : !m_ul(l, &off)) break;
        m_next(c, &l);
        mb_puts(o, "<li>");
        m_inline(o, l.s + off, l.n - off, depth + 1);
        while (m_peek(*c, &next) && !m_blank(next) &&
               !m_ul(next, &noff) && !m_ol(next, &noff) &&
               !m_block_start(*c)) {
            m_next(c, &next);
            mb_puts(o, "<br>");
            m_inline(o, next.s, next.n, depth + 1);
        }
        mb_puts(o, "</li>");
    }
    mb_puts(o, ordered ? "</ol>" : "</ul>");
}

static void m_table(mbuf *o, mcur *c, int depth) {
    mspan h, d;
    m_next(c, &h);
    m_next(c, &d);
    mspan head = m_strip_pipes(h);
    int cols = m_cells(h);
    mb_puts(o, "<table><thead><tr>");
    size_t pos = 0;
    mspan cell;
    while (m_cell_next(head, &pos, &cell)) {
        mb_puts(o, "<th>");
        m_inline(o, cell.s, cell.n, depth + 1);
        mb_puts(o, "</th>");
    }
    mb_puts(o, "</tr></thead><tbody>");
    while (m_peek(*c, &h)) {
        if (m_blank(h) || !memchr(h.s, '|', h.n)) break;
        m_next(c, &h);
        mspan row = m_strip_pipes(h);
        pos = 0;
        int n = 0;
        mb_puts(o, "<tr>");
        while (pos <= row.n) {
            if (!m_cell_next(row, &pos, &cell)) break;
            mb_puts(o, "<td>");
            m_inline(o, cell.s, cell.n, depth + 1);
            mb_puts(o, "</td>");
            if (++n >= cols) break;
        }
        while (n++ < cols) mb_puts(o, "<td></td>");
        mb_puts(o, "</tr>");
    }
    mb_puts(o, "</tbody></table>");
}

static void m_blocks(mbuf *o, mcur *c, int depth) {
    if (depth > 8) {
        mspan rest;
        while (m_next(c, &rest)) { m_escape(o, rest.s, rest.n); mb_puts(o, "\n"); }
        return;
    }
    mspan l;
    while (m_peek(*c, &l)) {
        if (m_blank(l)) { m_next(c, &l); continue; }

        char ch;
        size_t fcount;
        mspan info;
        if (m_fence(l, &ch, &fcount, &info)) {
            m_next(c, &l);
            mb_puts(o, "<pre><code");
            if (info.n) {
                mb_puts(o, " class=\"language-");
                m_escape(o, info.s, info.n);
                mb_puts(o, "\"");
            }
            mb_puts(o, ">");
            while (m_peek(*c, &l) && !m_fence_end(l, ch, fcount)) {
                m_next(c, &l);
                m_escape(o, l.s, l.n);
                mb_puts(o, "\n");
            }
            if (m_peek(*c, &l) && m_fence_end(l, ch, fcount)) m_next(c, &l);
            mb_puts(o, "</code></pre>");
            continue;
        }

        int lvl;
        if (m_heading(l, &lvl, &info)) {
            m_next(c, &l);
            char tag[4];
            snprintf(tag, sizeof tag, "h%d", lvl);
            mb_puts(o, "<");
            mb_puts(o, tag);
            mb_puts(o, ">");
            m_inline(o, info.s, info.n, depth + 1);
            mb_puts(o, "</");
            mb_puts(o, tag);
            mb_puts(o, ">");
            continue;
        }

        if (m_hr(l)) { m_next(c, &l); mb_puts(o, "<hr>"); continue; }

        size_t off;
        if (m_quote(l, &off)) {
            mbuf inner = {0};
            while (m_peek(*c, &l) && m_quote(l, &off)) {
                m_next(c, &l);
                mb_add(&inner, l.s + off, l.n - off);
                mb_puts(&inner, "\n");
            }
            mb_puts(o, "<blockquote>");
            mcur ic = {inner.p, inner.p + inner.n};
            m_blocks(o, &ic, depth + 1);
            mb_puts(o, "</blockquote>");
            free(inner.p);
            continue;
        }

        if (m_ul(l, &off)) { m_list(o, c, 0, depth); continue; }
        if (m_ol(l, &off)) { m_list(o, c, 1, depth); continue; }
        if (m_table_at(*c)) { m_table(o, c, depth); continue; }

        mb_puts(o, "<p>");
        int first = 1;
        while (m_peek(*c, &l) && !m_blank(l) && !m_block_start(*c)) {
            m_next(c, &l);
            if (!first) mb_puts(o, "<br>");
            m_inline(o, l.s, l.n, depth + 1);
            first = 0;
        }
        mb_puts(o, "</p>");
    }
}

char *md_render_html(const char *src, size_t len, size_t *out_len) {
    mbuf out = {0};
    mcur c = {src, src + len};
    m_blocks(&out, &c, 0);
    if (out.err) { free(out.p); if (out_len) *out_len = 0; return NULL; }
    if (!out.p) {
        out.p = malloc(1);
        if (!out.p) { if (out_len) *out_len = 0; return NULL; }
        out.p[0] = 0;
        out.n = 0;
    }
    if (out_len) *out_len = out.n;
    return out.p;
}
