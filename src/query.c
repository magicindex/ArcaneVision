/* 查询：稀有 trigram 出候选 → 全名复核 → 排序输出
 * 少于 3 字节的查询没有 trigram 可用，走名字池线性扫描（P0 权衡，见文档）。
 */
#include "xl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const xl_snap *sn;
    const char *needle;
    size_t nlen;
    int dirs_only, files_only;
    uint32_t *hit;
    uint32_t n, cap, limit;
    uint64_t total;
    int truncated;
    uint32_t last_id;   /* 去重：短查询在同一个名字里可能多处命中 */
    int has_last;
} collect_ctx;

static int passes_filter(const collect_ctx *c, const xl_rec *r) {
    if (c->dirs_only && !(r->flags & XL_F_DIR)) return 0;
    if (c->files_only && (r->flags & XL_F_DIR)) return 0;
    return 1;
}

/* 结果按 mtime 新→旧、同 mtime 短名优先；这里用「最差在堆顶」的小顶堆保留最新 K 条，
 * 避免只取到 posting 顺序里的前 K 条导致按时间排序失真。 */
static int hit_worse(const xl_rec *a, const xl_rec *b) {
    if (a->mtime != b->mtime) return a->mtime < b->mtime;
    if (a->name_len != b->name_len) return a->name_len > b->name_len;
    return a->name_off > b->name_off;
}

static void heap_swap(uint32_t *h, uint32_t i, uint32_t j) {
    uint32_t t = h[i]; h[i] = h[j]; h[j] = t;
}

static void heap_up(collect_ctx *c, uint32_t i) {
    while (i) {
        uint32_t p = (i - 1) / 2;
        if (!hit_worse(&c->sn->recs[c->hit[i]], &c->sn->recs[c->hit[p]])) break;
        heap_swap(c->hit, i, p);
        i = p;
    }
}

static void heap_down(collect_ctx *c, uint32_t i) {
    for (;;) {
        uint32_t l = i * 2 + 1, r = l + 1, w = i;
        if (l < c->n && hit_worse(&c->sn->recs[c->hit[l]], &c->sn->recs[c->hit[w]])) w = l;
        if (r < c->n && hit_worse(&c->sn->recs[c->hit[r]], &c->sn->recs[c->hit[w]])) w = r;
        if (w == i) break;
        heap_swap(c->hit, i, w);
        i = w;
    }
}

static void collect(collect_ctx *c, uint32_t id) {
    if (c->has_last && c->last_id == id) return;
    c->last_id = id;
    c->has_last = 1;
    if (id >= c->sn->h.rec_count) return;
    const xl_rec *r = &c->sn->recs[id];
    if (!passes_filter(c, r)) return;
    if (r->name_len < c->nlen) return;
    if (!mem_ci(c->sn->names + r->name_off, r->name_len, c->needle, c->nlen)) return;
    c->total++;
    if (c->n < c->limit) {
        if (c->n == c->cap) {
            c->cap = c->cap ? c->cap * 2 : 1024;
            c->hit = xrealloc(c->hit, (size_t)c->cap * sizeof(uint32_t));
        }
        c->hit[c->n] = id;
        heap_up(c, c->n);
        c->n++;
        return;
    }
    c->truncated = 1;
    if (hit_worse(&c->sn->recs[c->hit[0]], r)) {
        c->hit[0] = id;
        heap_down(c, 0);
    }
}

static void cb_collect(uint32_t id, void *ctx) { collect((collect_ctx *)ctx, id); }

/* 在快照的块序列上遍历某个 trigram */
static int snap_foreach(const xl_snap *sn, uint32_t tg,
                        void (*cb)(uint32_t, void *), void *ctx) {
    uint32_t p = tri_lookup_sorted(sn->tri_ids, sn->h.tri_count, tg);
    if (p >= sn->h.tri_count || sn->tri_ids[p] != tg) return -1;
    const xl_range *rg = &sn->tri_ranges[p];
    uint32_t remaining = rg->count;
    const uint8_t *cur = sn->postings + rg->off;
    while (remaining) {
        uint32_t base;
        uint16_t n;
        uint8_t bits;
        memcpy(&base, cur, 4);
        memcpy(&n, cur + 4, 2);
        bits = cur[6];
        const uint8_t *pb = cur + 8;
        size_t dbytes = (size_t)(n - 1) * bits / 8 + ((size_t)(n - 1) * bits % 8 ? 1 : 0);
        uint32_t id = base;
        cb(id, ctx);
        size_t bitpos = 0;
        for (uint32_t i = 1; i < n; i++) {
            uint32_t d = 0;
            for (uint32_t b = 0; b < bits; b++)
                if (pb[(bitpos + b) >> 3] & (1u << ((bitpos + b) & 7))) d |= (1u << b);
            bitpos += bits;
            id += d;
            cb(id, ctx);
        }
        cur += 8 + dbytes;
        remaining -= n;
    }
    return 0;
}

/* 短查询：名字池整体扫描，命中偏移再二分回落到记录 */
static int scan_pool_short(const xl_snap *sn, collect_ctx *c) {
    const char *pool = sn->names;
    size_t total = sn->h.names_bytes;
    if (total == 0) return 0;
    size_t last = total >= c->nlen ? total - c->nlen : 0;
    const char *p = pool;
    size_t remain = total;
    while (remain >= c->nlen) {
        const char *hit = memmem_ci(p, remain, c->needle, c->nlen);
        if (!hit) break;
        size_t off = (size_t)(hit - pool);
        /* 二分：找 name_off <= off 的最后一条 */
        uint32_t lo = 0, hi = sn->h.rec_count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (sn->recs[mid].name_off <= off) lo = mid + 1;
            else hi = mid;
        }
        if (lo > 0) collect(c, lo - 1);
        size_t adv = off + 1;
        p = pool + adv;
        remain = total - adv;
        if (adv > last) break;
    }
    return 0;
}

static int cmp_hit(const void *a, const void *b, void *arg) {
    const xl_rec *recs = arg;
    const xl_rec *ra = &recs[*(const uint32_t *)a];
    const xl_rec *rb = &recs[*(const uint32_t *)b];
    if (ra->mtime != rb->mtime) return ra->mtime < rb->mtime ? 1 : -1;
    return (int)ra->name_len - (int)rb->name_len;
}

void snap_path(const xl_snap *sn, uint32_t id, char *out, size_t cap) {
    char tail[8192];
    size_t pos = sizeof(tail) - 1;
    tail[pos] = 0;
    uint32_t guard = 0;
    for (uint32_t cur = id; cur != XL_ROOT && guard < 4096; guard++) {
        const xl_rec *r = &sn->recs[cur];
        if (r->name_len == 0) { cur = r->parent; continue; } /* 根记录自身 */
        if (pos < (size_t)r->name_len + 1) break;
        pos -= r->name_len;
        memcpy(tail + pos, sn->names + r->name_off, r->name_len);
        tail[--pos] = '/';
        cur = r->parent;
    }
    const char *root = sn->root;
    size_t rl = strlen(root);
    if (rl == 1 && root[0] == '/') snprintf(out, cap, "%s", tail + pos);
    else snprintf(out, cap, "%s%s", root, tail + pos);
}

uint32_t *query_hits(const xl_snap *sn, const char *needle, const xl_query_opts *o,
                     uint32_t *n_out, int *truncated_out, uint64_t *elapsed_ms,
                     uint64_t *total_out) {
    *n_out = 0;
    *truncated_out = 0;
    if (elapsed_ms) *elapsed_ms = 0;
    if (!needle || !*needle) return NULL;
    size_t nlen = strlen(needle);
    if (nlen > XL_NAME_MAX) return NULL;

    uint64_t t0 = now_ms();
    collect_ctx c;
    memset(&c, 0, sizeof(c));
    c.sn = sn;
    c.needle = needle;
    c.nlen = nlen;
    c.dirs_only = o->dirs_only;
    c.files_only = o->files_only;
    c.limit = o->max_results ? o->max_results : 200000u;
    c.limit = c.limit < 1000 ? 1000 : c.limit;

    if (nlen < 3) {
        scan_pool_short(sn, &c);
    } else {
        char lb[XL_NAME_MAX + 1];
        lower_copy(lb, needle, nlen);
        /* 选 count 最小的 trigram 作为候选源 */
        uint32_t best_tg = 0, best_cnt = UINT32_MAX;
        for (size_t i = 0; i + 3 <= nlen; i++) {
            uint32_t tg = ((uint32_t)(unsigned char)lb[i] << 16) |
                          ((uint32_t)(unsigned char)lb[i + 1] << 8) |
                          (uint32_t)(unsigned char)lb[i + 2];
            uint32_t p = tri_lookup_sorted(sn->tri_ids, sn->h.tri_count, tg);
            uint32_t cnt = (p < sn->h.tri_count && sn->tri_ids[p] == tg)
                               ? sn->tri_ranges[p].count : 0;
            if (cnt == 0) { best_cnt = 0; best_tg = tg; break; }
            if (cnt < best_cnt) { best_cnt = cnt; best_tg = tg; }
        }
        if (best_cnt != 0) snap_foreach(sn, best_tg, cb_collect, &c);
    }

    if (c.n > 1) qsort_r(c.hit, c.n, sizeof(uint32_t), cmp_hit, (void *)sn->recs);
    *n_out = c.n;
    *truncated_out = c.truncated;
    if (total_out) *total_out = c.total;
    if (elapsed_ms) *elapsed_ms = now_ms() - t0;
    return c.hit;
}

static void json_escape(const char *s, size_t n, FILE *f) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c == '\n') fputs("\\n", f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
}

int query_run(const xl_snap *sn, const char *needle, const xl_query_opts *o) {
    if (!needle || !*needle) {
        fprintf(stderr, "xuanlan: empty query\n");
        return 2;
    }
    if (strlen(needle) > XL_NAME_MAX) {
        fprintf(stderr, "xuanlan: query too long\n");
        return 2;
    }
    uint64_t t0 = now_ms();
    uint32_t n = 0;
    int truncated = 0;
    uint64_t total = 0;
    uint32_t *hit = query_hits(sn, needle, o, &n, &truncated, NULL, &total);

    uint32_t show = o->max_results ? o->max_results : 50;
    if (show > n) show = n;
    char path[8192];
    if (o->json) {
        printf("{\"query\":\"");
        json_escape(needle, strlen(needle), stdout);
        printf("\",\"total\":%llu,\"shown\":%u,\"truncated\":%s,\"items\":[",
               (unsigned long long)total, show, truncated ? "true" : "false");
    }
    for (uint32_t i = 0; i < show; i++) {
        uint32_t id = hit[i];
        const xl_rec *r = &sn->recs[id];
        snap_path(sn, id, path, sizeof(path));
        if (o->json) {
            if (i) fputc(',', stdout);
            printf("{\"path\":\"");
            json_escape(path, strlen(path), stdout);
            printf("\",\"name\":\"");
            json_escape(sn->names + r->name_off, r->name_len, stdout);
            printf("\",\"dir\":%s,\"size\":%u,\"mtime\":%u}",
                   (r->flags & XL_F_DIR) ? "true" : "false", r->size, r->mtime);
        } else {
            printf("%s%s\n", path, (r->flags & XL_F_DIR) ? "/" : "");
        }
    }
    if (o->json) printf("]}\n");
    fprintf(stderr, "  %s%u matches (showing %u%s), total %.1f ms\n",
            truncated ? ">=" : "", n, show, truncated ? ", truncated" : "",
            (double)(now_ms() - t0));
    free(hit);
    return 0;
}
