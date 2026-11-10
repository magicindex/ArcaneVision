/* 快照：单文件 + 4KB 对齐段 + mmap 直接可用
 * 设计目标是"重启即用"：加载不做反序列化，只做一次头部校验。
 */
#include "xl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static inline uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static int write_at(int fd, const void *buf, size_t n, uint64_t off) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = pwrite(fd, p, n, (off_t)off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w; n -= (size_t)w; off += (uint64_t)w;
    }
    return 0;
}

int snapshot_save(const char *path, const xl_store *s, const xl_index *ix,
                  const char *root, uint64_t scan_ms) {
    xl_hdr h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, XL_MAGIC, 8);
    h.version = XL_VERSION;
    h.flags = XL_HDR_SCAN_RULES | (scan_skip_hidden() ? XL_HDR_SKIP_HIDDEN : 0);
    h.rec_count = s->n;
    h.names_bytes = s->names_len;
    h.tri_count = ix->tri_count;
    h.postings_bytes = ix->blob_len;
    h.root_len = (uint32_t)strlen(root);
    h.built_at = (uint64_t)time(NULL);
    h.scan_ms = scan_ms;

    uint64_t off = XL_HDR_SIZE;
    h.rec_off = align_up(off, XL_ALIGN);
    off = h.rec_off + h.rec_count * sizeof(xl_rec);
    h.names_off = align_up(off, XL_ALIGN);
    off = h.names_off + h.names_bytes;
    h.tri_ids_off = align_up(off, XL_ALIGN);
    off = h.tri_ids_off + (uint64_t)h.tri_count * 4;
    h.tri_ranges_off = align_up(off, XL_ALIGN);
    off = h.tri_ranges_off + (uint64_t)h.tri_count * sizeof(xl_range);
    h.postings_off = align_up(off, XL_ALIGN);
    off = h.postings_off + h.postings_bytes;
    h.root_off = align_up(off, XL_ALIGN);
    off = h.root_off + h.root_len + 1;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "xuanlan: cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }

    int rc = 0;
    if (write_at(fd, &h, sizeof(h), 0) < 0) rc = -1;
    if (!rc && write_at(fd, s->recs, (size_t)s->n * sizeof(xl_rec), h.rec_off) < 0) rc = -1;
    if (!rc && write_at(fd, s->names, s->names_len, h.names_off) < 0) rc = -1;
    if (!rc && write_at(fd, ix->tri_ids, (size_t)ix->tri_count * 4, h.tri_ids_off) < 0) rc = -1;
    if (!rc && write_at(fd, ix->tri_ranges, (size_t)ix->tri_count * sizeof(xl_range),
                        h.tri_ranges_off) < 0) rc = -1;
    if (!rc && write_at(fd, ix->blob, ix->blob_len, h.postings_off) < 0) rc = -1;
    if (!rc && write_at(fd, root, h.root_len + 1, h.root_off) < 0) rc = -1;
    if (!rc && ftruncate(fd, (off_t)off) < 0) rc = -1;
    if (!rc && fsync(fd) < 0) rc = -1;   /* rename 之前落盘，避免掉电留下半写索引 */
    if (rc)
        fprintf(stderr, "xuanlan: write %s failed: %s\n", path, strerror(errno));
    if (close(fd) < 0 && !rc) rc = -1;
    return rc;
}

int snapshot_open(const char *path, xl_snap *sn) {
    memset(sn, 0, sizeof(*sn));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "xuanlan: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    sn->map_len = (size_t)st.st_size;
    sn->map = mmap(NULL, sn->map_len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (sn->map == MAP_FAILED) {
        fprintf(stderr, "xuanlan: mmap failed: %s\n", strerror(errno));
        return -1;
    }
    if (sn->map_len < sizeof(sn->h)) { munmap(sn->map, sn->map_len); memset(sn, 0, sizeof(*sn)); return -1; }
    memcpy(&sn->h, sn->map, sizeof(sn->h));
    if (memcmp(sn->h.magic, XL_MAGIC, 8) != 0) {
        fprintf(stderr, "xuanlan: not a 玄览 index file\n");
        goto bad;
    }
    if (sn->h.version != XL_VERSION) {
        fprintf(stderr, "xuanlan: index version %u, expected %u\n",
                sn->h.version, XL_VERSION);
        goto bad;
    }
    {
        uint64_t ml = sn->map_len;
#define SEG_OK(off, len) ((uint64_t)(off) <= ml && (uint64_t)(len) <= ml - (uint64_t)(off))
        if (sn->h.rec_count > ml / sizeof(xl_rec) + 1 ||
            !SEG_OK(sn->h.rec_off, sn->h.rec_count * sizeof(xl_rec)) ||
            !SEG_OK(sn->h.names_off, sn->h.names_bytes) ||
            !SEG_OK(sn->h.tri_ids_off, (uint64_t)sn->h.tri_count * 4) ||
            !SEG_OK(sn->h.tri_ranges_off, (uint64_t)sn->h.tri_count * sizeof(xl_range)) ||
            !SEG_OK(sn->h.postings_off, sn->h.postings_bytes) ||
            !SEG_OK(sn->h.root_off, (uint64_t)sn->h.root_len + 1)) {
            fprintf(stderr, "xuanlan: index truncated\n");
            goto bad;
        }
#undef SEG_OK
    }
    const uint8_t *base = sn->map;
    sn->recs = (xl_rec *)(base + sn->h.rec_off);
    sn->names = (char *)(base + sn->h.names_off);
    sn->tri_ids = (uint32_t *)(base + sn->h.tri_ids_off);
    sn->tri_ranges = (xl_range *)(base + sn->h.tri_ranges_off);
    sn->postings = base + sn->h.postings_off;
    sn->root = (const char *)(base + sn->h.root_off);
    /* 逐条校验名称偏移与父节点，损坏索引不能把越界值带进查询 */
    for (uint32_t i = 0; i < sn->h.rec_count; i++) {
        const xl_rec *r = &sn->recs[i];
        if ((uint64_t)r->name_off + (uint64_t)r->name_len > sn->h.names_bytes ||
            (r->parent != XL_ROOT && r->parent >= sn->h.rec_count)) {
            fprintf(stderr, "xuanlan: index record out of range (#%u)\n", i);
            goto bad;
        }
    }
    return 0;

bad:
    munmap(sn->map, sn->map_len);
    memset(sn, 0, sizeof(*sn));
    return -1;
}

void snapshot_close(xl_snap *sn) {
    if (sn->map && sn->map != MAP_FAILED) munmap(sn->map, sn->map_len);
    memset(sn, 0, sizeof(*sn));
}
