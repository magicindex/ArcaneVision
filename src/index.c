/* 索引构建：稀疏 trigram 表 + 分块位打包倒排
 *
 * 结构（全部原创实现）：
 *   tri_ids[]      升序排列的、实际出现过的 trigram 值
 *   tri_ranges[]   每个 trigram 在 blob 中的字节偏移与条目数
 *   blob           若干 128 条一块的块序列；块头 8B（base,n,bits,pad），
 *                  其后是 n-1 个 delta 的定宽位流（LSB 优先）
 *
 * 选择 block 位打包而非变长编码，是为查询期的可跳过性与缓存友好：
 * 块头自带 base，可对候选序列做游标式跳过求交。
 */
#include "xl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRI_SPACE (1u << 24)

static inline uint32_t tri3(const unsigned char *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static inline uint32_t bits_for(uint32_t v) {
    uint32_t b = 0;
    while (v) { b++; v >>= 1; }
    return b;
}

void index_init(xl_index *ix) { memset(ix, 0, sizeof(*ix)); }

void index_free(xl_index *ix) {
    free(ix->tri_ids);
    free(ix->tri_ranges);
    free(ix->postings);
    free(ix->blob);
    memset(ix, 0, sizeof(*ix));
}

/* 把一段升序 id 编码进 blob；返回新增字节数 */
static size_t encode_block(uint8_t **blob, size_t *len, size_t *cap,
                           const uint32_t *ids, uint32_t n) {
    uint32_t maxd = 0;
    for (uint32_t i = 1; i < n; i++) {
        uint32_t d = ids[i] - ids[i - 1];
        if (d > maxd) maxd = d;
    }
    uint32_t bits = bits_for(maxd);
    size_t dbytes = (size_t)(n - 1) * bits / 8 + ((size_t)(n - 1) * bits % 8 ? 1 : 0);
    size_t need = 8 + dbytes;
    if (*len + need > *cap) {
        while (*len + need > *cap) *cap = *cap ? *cap * 2 : (1u << 20);
        *blob = xrealloc(*blob, *cap);
    }
    uint8_t *p = *blob + *len;
    uint32_t base = ids[0];
    memcpy(p, &base, 4);
    uint16_t cnt = (uint16_t)n;
    memcpy(p + 4, &cnt, 2);
    p[6] = (uint8_t)bits;
    p[7] = 0;
    if (bits) {
        uint8_t *out = p + 8;
        memset(out, 0, dbytes);
        size_t bitpos = 0;
        for (uint32_t i = 1; i < n; i++) {
            uint32_t d = ids[i] - ids[i - 1];
            for (uint32_t b = 0; b < bits; b++) {
                if (d & (1u << b)) out[(bitpos + b) >> 3] |= (uint8_t)(1u << ((bitpos + b) & 7));
            }
            bitpos += bits;
        }
    }
    *len += need;
    return need;
}

int index_build(xl_index *ix, const xl_store *s, int verbose) {
    uint64_t t0 = now_ms();
    uint32_t *cnt = xcalloc(TRI_SPACE, sizeof(uint32_t));
    char lb[XL_NAME_MAX + 1];

    /* 第一遍：计数 */
    for (uint32_t i = 0; i < s->n; i++) {
        const xl_rec *r = &s->recs[i];
        if (r->name_len < 3) continue;
        lower_copy(lb, s->names + r->name_off, r->name_len);
        for (uint32_t j = 0; j + 3 <= r->name_len; j++)
            cnt[tri3((const unsigned char *)lb + j)]++;
    }

    /* 稀疏表：只保留出现过的 trigram，按数值升序 */
    uint32_t present = 0;
    for (uint32_t t = 0; t < TRI_SPACE; t++) if (cnt[t]) present++;
    ix->tri_count = present;
    ix->tri_ids = xmalloc((size_t)(present ? present : 1) * sizeof(uint32_t));
    ix->tri_ranges = xcalloc(present ? present : 1, sizeof(xl_range));

    uint64_t total = 0;
    uint32_t k = 0;
    for (uint32_t t = 0; t < TRI_SPACE; t++) {
        if (!cnt[t]) continue;
        ix->tri_ids[k] = t;
        ix->tri_ranges[k].off = (uint32_t)total;
        ix->tri_ranges[k].count = cnt[t];
        total += cnt[t];
        k++;
    }
    if (verbose)
        fprintf(stderr, "  trigrams: %u present, %llu postings\n",
                present, (unsigned long long)total);

    if (total == 0) {
        free(cnt);
        ix->build_ms = now_ms() - t0;
        return 0;
    }

    /* 第二遍：填充（按记录号升序，天然有序） */
    ix->postings = xmalloc((size_t)total * sizeof(uint32_t));
    uint32_t *cursor = xmalloc((size_t)present * sizeof(uint32_t));
    for (uint32_t i = 0; i < present; i++) cursor[i] = ix->tri_ranges[i].off;
    ix->postings_n = (uint32_t)total;

    for (uint32_t i = 0; i < s->n; i++) {
        const xl_rec *r = &s->recs[i];
        if (r->name_len < 3) continue;
        lower_copy(lb, s->names + r->name_off, r->name_len);
        for (uint32_t j = 0; j + 3 <= r->name_len; j++) {
            uint32_t tg = tri3((const unsigned char *)lb + j);
            uint32_t pos = tri_lookup_sorted(ix->tri_ids, present, tg);
            ix->postings[cursor[pos]++] = i;
        }
    }
    free(cursor);
    free(cnt);

    /* 编码 */
    for (uint32_t i = 0; i < present; i++) {
        xl_range *rg = &ix->tri_ranges[i];
        const uint32_t *ids = ix->postings + rg->off;
        uint32_t n = rg->count;
        size_t start = ix->blob_len;
        for (uint32_t b = 0; b < n; b += XL_BLOCK) {
            uint32_t m = n - b < XL_BLOCK ? n - b : XL_BLOCK;
            encode_block(&ix->blob, &ix->blob_len, &ix->blob_cap, ids + b, m);
        }
        rg->off = (uint32_t)start;
    }
    free(ix->postings);
    ix->postings = NULL;

    ix->build_ms = now_ms() - t0;
    if (verbose)
        fprintf(stderr, "  encoded blob: %.1f MB (%.2f B/posting)\n",
                (double)ix->blob_len / 1048576.0,
                ix->postings_n ? (double)ix->blob_len / ix->postings_n : 0.0);
    return 0;
}

uint32_t tri_lookup_sorted(const uint32_t *ids, uint32_t n, uint32_t tg) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ids[mid] < tg) lo = mid + 1;
        else hi = mid;
    }
    return lo; /* 未找到时返回插入位置；调用方保证存在 */
}

uint32_t tri_count_of(const xl_index *ix, uint32_t tg) {
    uint32_t p = tri_lookup_sorted(ix->tri_ids, ix->tri_count, tg);
    if (p >= ix->tri_count || ix->tri_ids[p] != tg) return 0;
    return ix->tri_ranges[p].count;
}

int tri_foreach(const xl_index *ix, uint32_t tg,
                void (*cb)(uint32_t id, void *ctx), void *ctx) {
    uint32_t p = tri_lookup_sorted(ix->tri_ids, ix->tri_count, tg);
    if (p >= ix->tri_count || ix->tri_ids[p] != tg) return -1;
    const xl_range *rg = &ix->tri_ranges[p];
    uint32_t remaining = rg->count;
    const uint8_t *cur = ix->blob + rg->off;
    while (remaining) {
        uint32_t base;
        uint16_t n;
        uint8_t bits;
        memcpy(&base, cur, 4);
        memcpy(&n, cur + 4, 2);
        bits = cur[6];
        const uint8_t *pbits = cur + 8;
        size_t dbytes = (size_t)(n - 1) * bits / 8 + ((size_t)(n - 1) * bits % 8 ? 1 : 0);
        uint32_t id = base;
        cb(id, ctx);
        size_t bitpos = 0;
        for (uint32_t i = 1; i < n; i++) {
            uint32_t d = 0;
            for (uint32_t b = 0; b < bits; b++) {
                if (pbits[(bitpos + b) >> 3] & (1u << ((bitpos + b) & 7)))
                    d |= (1u << b);
            }
            bitpos += bits;
            id += d;
            cb(id, ctx);
        }
        cur += 8 + dbytes;
        remaining -= n;
    }
    return 0;
}
