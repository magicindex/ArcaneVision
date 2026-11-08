/* ext4 裸设备读取器
 *
 * 依据公开的 ext4 on-disk 格式规范独立实现，不链接、不引用 e2fsprogs 的代码。
 *
 * 思路：
 *   1) 顺序读各组已初始化的 inode 表（只读 itable_unused 之前的部分），筛出目录 inode；
 *   2) 在内存里解析 extent 树，得到每个目录的数据块号；
 *   3) 把所有目录块按物理块号排序后顺序读，等价于把随机 IO 变成顺序 IO；
 *   4) 解析 dir_entry 得到 (父 inode, 子 inode, 名字) 边，再 DFS 还原成记录数组。
 */
#include "xl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXT4_SUPER_MAGIC      0xEF53
#define EXT4_EXTENT_MAGIC     0xF30A
#define EXT4_ROOT_INO         2u

#define EXT4_EXTENTS_FL       0x00080000u
#define EXT4_INLINE_DATA_FL   0x10000000u

#define EXT4_BG_INODE_UNINIT  0x0001u

#define INCOMPAT_64BIT        0x0080u
#define INCOMPAT_META_BG      0x0010u
#define INCOMPAT_INLINE_DATA  0x8000u

#define S_IFMT_  0170000
#define S_IFDIR_ 0040000

/* 只保留读取必需的字段 */
typedef struct {
    int fd;
    uint32_t block_size, inode_size;
    uint32_t blocks_per_group, inodes_per_group;
    uint32_t desc_size;
    uint32_t first_data_block;
    uint64_t n_groups, n_inodes, n_blocks;
    uint8_t *gd;          /* 组描述符表 */
    uint8_t *is_dir_bits; /* inode 号 -> 是否目录 */
    uint8_t *inode_bitmap;/* 权威分配位图：bit (ino-1) */
    size_t   bits_len;
} ext4_fs;

typedef struct {
    uint32_t ino;
    uint32_t flags;
    uint32_t size;
    uint32_t blk[15];
} dir_ino;

typedef struct {
    uint64_t block;
    uint32_t ino;
} blockref;

typedef struct {
    uint32_t parent;
    uint32_t child;
    uint32_t name_off;
    uint16_t name_len;
    uint8_t  type;
    uint8_t  pad;
} edge;

/* 按 inode 号升序收集的 size/mtime，避免为超大 inode 表分配 8 字节/项 */
typedef struct {
    uint32_t ino;
    uint32_t size;
    uint32_t mtime;
} ino_meta;

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* 位图是唯一权威：inode 表尾部/已删除槽位可能残留旧数据 */
static int ino_allocated(const ext4_fs *fs, uint32_t ino) {
    if (ino == 0 || ino >= fs->n_inodes) return 0;
    uint32_t b = ino - 1;
    return (fs->inode_bitmap[b >> 3] >> (b & 7)) & 1;
}

static int is_dir_ino(const ext4_fs *fs, uint32_t ino) {
    if (ino == 0 || ino >= fs->n_inodes) return 0;
    return (fs->is_dir_bits[ino >> 3] >> (ino & 7)) & 1;
}

static void mark_dir(ext4_fs *fs, uint32_t ino) {
    if (ino >= fs->n_inodes) return;
    fs->is_dir_bits[ino >> 3] |= (uint8_t)(1u << (ino & 7));
}

static int pread_full(int fd, void *buf, size_t n, uint64_t off) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;
        p += r; n -= (size_t)r; off += (uint64_t)r;
    }
    return 0;
}

static int fs_open(ext4_fs *fs, const char *dev, int verbose) {
    memset(fs, 0, sizeof(*fs));
    fs->fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fs->fd < 0) {
        fprintf(stderr, "xuanlan: cannot open %s: %s\n", dev, strerror(errno));
        return -1;
    }
    uint8_t sb[1024];
    if (pread_full(fs->fd, sb, sizeof(sb), 1024) < 0) {
        fprintf(stderr, "xuanlan: read superblock failed\n");
        return -1;
    }
    if (rd16(sb + 0x38) != EXT4_SUPER_MAGIC) {
        fprintf(stderr, "xuanlan: %s is not ext4\n", dev);
        return -1;
    }
    uint32_t log_bs = rd32(sb + 0x18);
    if (log_bs > 6) { fprintf(stderr, "xuanlan: bad block size\n"); return -1; }
    fs->block_size = 1024u << log_bs;
    uint32_t incompat = rd32(sb + 0x60);
    if (incompat & INCOMPAT_META_BG) {
        fprintf(stderr, "xuanlan: meta_bg 文件系统暂不支持，请改用目录扫描\n");
        return -1;
    }
    if (incompat & INCOMPAT_INLINE_DATA) {
        fprintf(stderr, "xuanlan: 启用 inline_data 的文件系统，目录需回退到 getdents\n");
        return -1;
    }
    if (!(incompat & 0x40)) {
        fprintf(stderr, "xuanlan: 无 extent 特性，暂不支持\n");
        return -1;
    }
    fs->inode_size = rd16(sb + 0x58);
    if (fs->inode_size < 128 || fs->inode_size > 4096) {
        fprintf(stderr, "xuanlan: 异常 inode 大小 %u\n", fs->inode_size);
        return -1;
    }
    fs->blocks_per_group = rd32(sb + 0x20);
    fs->inodes_per_group = rd32(sb + 0x28);
    fs->first_data_block = rd32(sb + 0x14);
    fs->n_inodes = rd32(sb + 0x00);
    fs->n_blocks = rd32(sb + 0x04);
    if (incompat & INCOMPAT_64BIT) {
        uint32_t hi = rd32(sb + 0x150);
        fs->n_blocks |= (uint64_t)hi << 32;
        fs->desc_size = rd16(sb + 0xFE);
    } else {
        fs->desc_size = 32;
    }
    if (fs->desc_size < 32) fs->desc_size = 32;
    fs->n_groups = (fs->n_blocks + fs->blocks_per_group - 1) / fs->blocks_per_group;

    size_t gd_len = (size_t)fs->n_groups * fs->desc_size;
    fs->gd = xmalloc(gd_len);
    uint64_t gd_off = (uint64_t)(fs->first_data_block + 1) * fs->block_size;
    if (pread_full(fs->fd, fs->gd, gd_len, gd_off) < 0) {
        fprintf(stderr, "xuanlan: read group descriptors failed\n");
        return -1;
    }
    fs->bits_len = (size_t)((fs->n_inodes + 8) / 8);
    fs->is_dir_bits = xcalloc(fs->bits_len ? fs->bits_len : 1, 1);
    fs->inode_bitmap = xcalloc(fs->bits_len ? fs->bits_len : 1, 1);
    {
        uint8_t *bm = xmalloc(fs->block_size);
        for (uint64_t g = 0; g < fs->n_groups; g++) {
            const uint8_t *gd = fs->gd + g * fs->desc_size;
            if (rd16(gd + 0x12) & EXT4_BG_INODE_UNINIT) continue; /* 全空组 */
            uint64_t bmp = rd32(gd + 0x04);
            if (fs->desc_size >= 64) bmp |= (uint64_t)rd32(gd + 0x24) << 32;
            if (pread_full(fs->fd, bm, fs->block_size, bmp * fs->block_size) < 0)
                continue;
            for (uint32_t i = 0; i < fs->inodes_per_group; i++) {
                if (!((bm[i >> 3] >> (i & 7)) & 1)) continue;
                uint32_t ino = (uint32_t)(g * fs->inodes_per_group + i) + 1;
                if (ino >= fs->n_inodes) break;
                uint32_t b = ino - 1;
                fs->inode_bitmap[b >> 3] |= (uint8_t)(1u << (b & 7));
            }
        }
        free(bm);
    }

    if (verbose)
        fprintf(stderr, "ext4: %llu blocks @ %u B, %llu inodes, %u inodes/group, %llu groups\n",
                (unsigned long long)fs->n_blocks, fs->block_size,
                (unsigned long long)fs->n_inodes, fs->inodes_per_group,
                (unsigned long long)fs->n_groups);
    return 0;
}

static void fs_close(ext4_fs *fs) {
    if (fs->fd >= 0) close(fs->fd);
    free(fs->gd);
    free(fs->is_dir_bits);
    free(fs->inode_bitmap);
    memset(fs, 0, sizeof(*fs));
    fs->fd = -1;
}

/* 阶段一：读 inode 表，收集目录 inode */
static int collect_dir_inodes(ext4_fs *fs, dir_ino **out, size_t *out_n,
                              ino_meta **meta_out, size_t *meta_n, int verbose) {
    size_t cap = 1u << 20, n = 0;
    dir_ino *dirs = xmalloc(cap * sizeof(dir_ino));
    uint8_t *buf = xmalloc(fs->inodes_per_group * (size_t)fs->inode_size);
    size_t mcap = 1u << 16, mn = 0;
    ino_meta *meta = xmalloc(mcap * sizeof(ino_meta));
    uint64_t scanned = 0;

    for (uint64_t g = 0; g < fs->n_groups; g++) {
        const uint8_t *gd = fs->gd + g * fs->desc_size;
        uint16_t bg_flags = rd16(gd + 0x12);
        if (bg_flags & EXT4_BG_INODE_UNINIT) continue;
        uint64_t itable = rd32(gd + 0x08);
        if (fs->desc_size >= 64) itable |= (uint64_t)rd32(gd + 0x28) << 32;
        uint32_t unused = rd16(gd + 0x1C);
        if (fs->desc_size >= 64) unused |= (uint32_t)rd16(gd + 0x32) << 16;
        if (unused >= fs->inodes_per_group) continue;
        uint32_t used = fs->inodes_per_group - unused;
        size_t bytes = (size_t)used * fs->inode_size;
        if (pread_full(fs->fd, buf, bytes, (uint64_t)itable * fs->block_size) < 0) {
            if (verbose) fprintf(stderr, "  group %llu: itable read failed, skip\n",
                                 (unsigned long long)g);
            continue;
        }
        for (uint32_t i = 0; i < used; i++) {
            const uint8_t *ip = buf + (size_t)i * fs->inode_size;
            uint16_t mode = rd16(ip + 0x00);
            uint32_t ino = (uint32_t)(g * fs->inodes_per_group + i + 1);
            if (ino >= fs->n_inodes) break;
            if (mode == 0 || !ino_allocated(fs, ino)) continue;
            scanned++;
            if (mn == mcap) {
                mcap *= 2;
                meta = xrealloc(meta, mcap * sizeof(ino_meta));
            }
            meta[mn].ino = ino;
            meta[mn].size = rd32(ip + 0x04);
            meta[mn].mtime = rd32(ip + 0x10);
            mn++;
            if ((mode & S_IFMT_) != S_IFDIR_) continue;
            mark_dir(fs, ino);
            if (n == cap) { cap *= 2; dirs = xrealloc(dirs, cap * sizeof(dir_ino)); }
            dirs[n].ino = ino;
            dirs[n].flags = rd32(ip + 0x20);
            dirs[n].size = rd32(ip + 0x04);
            memcpy(dirs[n].blk, ip + 0x28, 60);
            n++;
        }
    }
    free(buf);
    *out = dirs; *out_n = n;
    if (meta_out) *meta_out = meta;
    if (meta_n) *meta_n = mn;
    if (verbose)
        fprintf(stderr, "ext4: %llu inodes scanned, %zu directories\n",
                (unsigned long long)scanned, n);
    return 0;
}

/* 追加目录数据块；上限由 i_size 决定 */
static void push_blocks(blockref **v, size_t *n, size_t *cap,
                        uint64_t first, uint32_t count, uint32_t ino,
                        uint32_t *budget) {
    for (uint32_t i = 0; i < count && *budget; i++, (*budget)--) {
        if (*n == *cap) { *cap *= 2; *v = xrealloc(*v, *cap * sizeof(blockref)); }
        (*v)[*n].block = first + i;
        (*v)[*n].ino = ino;
        (*n)++;
    }
}

static int extent_walk(ext4_fs *fs, const uint8_t *node, uint32_t ino,
                       blockref **v, size_t *n, size_t *cap, uint32_t *budget);

static int extent_node_read(ext4_fs *fs, uint64_t blk, uint32_t ino,
                            blockref **v, size_t *n, size_t *cap, uint32_t *budget) {
    uint8_t *buf = xmalloc(fs->block_size);
    int rc = 0;
    if (pread_full(fs->fd, buf, fs->block_size, blk * fs->block_size) < 0) {
        rc = -1;
    } else if (rd16(buf) != EXT4_EXTENT_MAGIC) {
        rc = -1;
    } else {
        rc = extent_walk(fs, buf, ino, v, n, cap, budget);
    }
    free(buf);
    return rc;
}

static int extent_walk(ext4_fs *fs, const uint8_t *node, uint32_t ino,
                       blockref **v, size_t *n, size_t *cap, uint32_t *budget) {
    if (rd16(node) != EXT4_EXTENT_MAGIC) return -1;
    uint16_t entries = rd16(node + 2);
    uint16_t depth = rd16(node + 6);
    if ((uint32_t)entries * 12 + 12 > fs->block_size) return -1;
    for (uint16_t i = 0; i < entries; i++) {
        const uint8_t *e = node + 12 + (size_t)i * 12;
        if (depth == 0) {
            uint32_t logical = rd32(e);
            uint16_t len = rd16(e + 4);
            uint32_t uninit = 0;
            if (len > 32768) { len = (uint16_t)(len - 32768); uninit = 1; }
            uint64_t start = ((uint64_t)rd16(e + 6) << 32) | rd32(e + 8);
            (void)logical; (void)uninit;
            push_blocks(v, n, cap, start, len, ino, budget);
        } else {
            uint64_t child = ((uint64_t)rd16(e + 8) << 32) | rd32(e + 4);
            if (extent_node_read(fs, child, ino, v, n, cap, budget) < 0) return -1;
        }
        if (*budget == 0) break;
    }
    return 0;
}

/* 经典间接块（老文件系统兜底） */
static void indirect_blocks(ext4_fs *fs, uint64_t blk, uint32_t ino, int level,
                            blockref **v, size_t *n, size_t *cap, uint32_t *budget) {
    if (!blk || !*budget) return;
    uint8_t *buf = xmalloc(fs->block_size);
    if (pread_full(fs->fd, buf, fs->block_size, blk * fs->block_size) == 0) {
        uint32_t per = fs->block_size / 4;
        for (uint32_t i = 0; i < per && *budget; i++) {
            uint64_t b = rd32(buf + (size_t)i * 4);
            if (!b) continue;
            if (level == 1) push_blocks(v, n, cap, b, 1, ino, budget);
            else indirect_blocks(fs, b, ino, level - 1, v, n, cap, budget);
        }
    }
    free(buf);
}

static int cmp_blockref(const void *a, const void *b, void *arg) {
    uint64_t x = ((const blockref *)a)->block, y = ((const blockref *)b)->block;
    (void)arg;
    return x < y ? -1 : (x > y);
}

static int cmp_edge_parent(const void *a, const void *b, void *arg) {
    uint32_t x = ((const edge *)a)->parent, y = ((const edge *)b)->parent;
    (void)arg;
    return x < y ? -1 : (x > y);
}

/* 阶段三：按物理顺序读目录块并解析 */
static int read_dir_blocks(ext4_fs *fs, blockref *v, size_t n,
                           edge **edges, size_t *en, size_t *ecap,
                           char **names, size_t *names_len, size_t *names_cap,
                           int verbose) {
    qsort_r(v, n, sizeof(blockref), cmp_blockref, NULL);
    if (verbose) fprintf(stderr, "ext4: %zu directory blocks, reading in physical order\n", n);

    uint8_t *buf = xmalloc((size_t)1 << 20);
    size_t i = 0;
    while (i < n) {
        /* 合并物理连续的块，单次最多 1MB */
        size_t j = i + 1;
        uint64_t max_blocks = ((size_t)1 << 20) / fs->block_size;
        while (j < n && v[j].block == v[j - 1].block + 1 && (j - i) < max_blocks) j++;
        size_t count = j - i;
        if (pread_full(fs->fd, buf, count * fs->block_size,
                       v[i].block * fs->block_size) < 0) {
            i = j;
            continue;
        }
        for (size_t k = 0; k < count; k++) {
            const uint8_t *blk = buf + k * fs->block_size;
            uint32_t parent = v[i + k].ino;
            uint32_t off = 0;
            while (off + 8 <= fs->block_size) {
                uint32_t child = rd32(blk + off);
                uint16_t rec_len = rd16(blk + off + 4);
                uint8_t name_len = blk[off + 6];
                uint8_t type = blk[off + 7];
                if (rec_len < 8 || (rec_len & 3) || off + rec_len > fs->block_size) break;
                if (child && name_len && (uint32_t)name_len + 8 <= rec_len) {
                    const char *nm = (const char *)(blk + off + 8);
                    int dot = (name_len == 1 && nm[0] == '.') ||
                              (name_len == 2 && nm[0] == '.' && nm[1] == '.');
                    /* 默认跳过 . / @ 开头的隐藏项；不产生边，子树自然也不会被 DFS 展开 */
                    int hidden = scan_skip_hidden() && name_len > 0 &&
                                 (nm[0] == '.' || nm[0] == '@');
                    if (!dot && !hidden) {
                        if (*names_len + name_len > *names_cap) {
                            while (*names_len + name_len > *names_cap) *names_cap *= 2;
                            *names = xrealloc(*names, *names_cap);
                        }
                        if (*en == *ecap) {
                            *ecap *= 2;
                            *edges = xrealloc(*edges, *ecap * sizeof(edge));
                        }
                        edge *e = &(*edges)[(*en)++];
                        e->parent = parent;
                        e->child = child;
                        e->name_off = (uint32_t)*names_len;
                        e->name_len = name_len;
                        e->type = type;
                        e->pad = 0;
                        memcpy(*names + *names_len, nm, name_len);
                        *names_len += name_len;
                    }
                }
                off += rec_len;
            }
        }
        i = j;
    }
    free(buf);
    return 0;
}

/* 阶段四：DFS 生成记录 */
typedef struct {
    uint32_t ino;
    uint32_t rec;
    size_t pos, end;
} frame;

int fsreader_ext4_scan(const char *dev, const char *mount_root, xl_store *s,
                       int verbose) {
    ext4_fs fs;
    fs.fd = -1;
    if (fs_open(&fs, dev, verbose) < 0) return -1;

    dir_ino *dirs = NULL;
    ino_meta *meta = NULL;
    size_t ndirs = 0, nmeta = 0;
    collect_dir_inodes(&fs, &dirs, &ndirs, &meta, &nmeta, verbose);

    blockref *blocks = xmalloc((size_t)ndirs * 2 * sizeof(blockref) + 16);
    size_t nb = 0, bcap = (size_t)ndirs * 2 + 16;

    for (size_t i = 0; i < ndirs; i++) {
        uint32_t nblk = (dirs[i].size + fs.block_size - 1) / fs.block_size;
        if (nblk == 0) nblk = 1;
        uint32_t budget = nblk;
        if (dirs[i].flags & EXT4_EXTENTS_FL) {
            if (extent_walk(&fs, (const uint8_t *)dirs[i].blk, dirs[i].ino,
                            &blocks, &nb, &bcap, &budget) < 0 && verbose)
                fprintf(stderr, "  inode %u: extent 解析失败\n", dirs[i].ino);
        } else {
            for (int k = 0; k < 12 && budget; k++, budget--)
                if (dirs[i].blk[k]) push_blocks(&blocks, &nb, &bcap, dirs[i].blk[k], 1,
                                                dirs[i].ino, &budget);
            if (budget && dirs[i].blk[12])
                indirect_blocks(&fs, dirs[i].blk[12], dirs[i].ino, 1, &blocks, &nb,
                                &bcap, &budget);
            if (budget && dirs[i].blk[13])
                indirect_blocks(&fs, dirs[i].blk[13], dirs[i].ino, 2, &blocks, &nb,
                                &bcap, &budget);
        }
    }
    free(dirs);

    edge *edges = xmalloc(1u << 20);
    size_t en = 0, ecap = 1u << 20 >> 4;
    char *names = xmalloc(1u << 22);
    size_t names_len = 0, names_cap = 1u << 22;

    read_dir_blocks(&fs, blocks, nb, &edges, &en, &ecap, &names, &names_len,
                    &names_cap, verbose);
    free(blocks);

    qsort_r(edges, en, sizeof(edge), cmp_edge_parent, NULL);

    /* DFS：根 inode 2 */
    store_push_root(s, XL_F_DIR);

    frame *stack = xmalloc(8192 * sizeof(frame));
    size_t sp = 0;
    stack[sp].ino = EXT4_ROOT_INO;
    stack[sp].rec = 0;
    stack[sp].pos = 0;
    stack[sp].end = en;
    sp++;

    /* 每个父 inode 的子边区间用二分求 */
    size_t visited = 0;
    while (sp) {
        frame *f = &stack[sp - 1];
        if (f->pos == 0) {
            /* 首次进入：定位该 inode 的子边区间 */
            uint32_t lo = 0, hi = (uint32_t)en;
            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2;
                if (edges[mid].parent < f->ino) lo = mid + 1;
                else hi = mid;
            }
            size_t start = lo;
            lo = start; hi = (uint32_t)en;
            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2;
                if (edges[mid].parent <= f->ino) lo = mid + 1;
                else hi = mid;
            }
            f->pos = start;
            f->end = lo;
        }
        if (f->pos >= f->end) { sp--; continue; }

        const edge *e = &edges[f->pos++];
        int isdir = is_dir_ino(&fs, e->child);
        uint8_t flags = 0;
        if (isdir) flags |= XL_F_DIR;
        else if (e->type == 7) flags |= XL_F_LINK;
        uint32_t fsize = 0, fmtime = 0;
        size_t lo = 0, hi = nmeta;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (meta[mid].ino < e->child) lo = mid + 1;
            else hi = mid;
        }
        if (lo < nmeta && meta[lo].ino == e->child) {
            if (!isdir) fsize = meta[lo].size;
            fmtime = meta[lo].mtime;
        }
        uint32_t rec = store_push_meta(s, f->rec, names + e->name_off, e->name_len,
                                       flags, fsize, fmtime);
        visited++;

        if (isdir && e->child != EXT4_ROOT_INO) {
            if (sp >= 8192) continue;
            stack[sp].ino = e->child;
            stack[sp].rec = rec;
            stack[sp].pos = 0;
            stack[sp].end = 0;
            sp++;
        }
    }
    free(stack);
    free(meta);

    if (verbose)
        fprintf(stderr, "ext4: %zu edges, %zu records, %zu names bytes\n",
                en, visited + 1, names_len);

    free(edges);
    free(names);
    fs_close(&fs);
    (void)mount_root;
    return 0;
}
