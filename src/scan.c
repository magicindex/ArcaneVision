/* 扫描器：getdents64 + DFS 先序
 * 先序保证父记录编号小于所有子记录，路径可由 parent 链单向上溯还原。
 */
#include "xl.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char d_name[];
};

/* 排除前缀（整棵子树不进入） */
static char **g_excl;
static size_t g_excl_n;

void scan_add_exclude(const char *prefix) {
    if (!prefix || !*prefix) return;
    char *p = strdup(prefix);
    if (!p) return;
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
    g_excl = xrealloc(g_excl, (g_excl_n + 1) * sizeof(char *));
    g_excl[g_excl_n++] = p;
}

int scan_has_exclude(void) { return g_excl_n > 0; }

/* 隐藏项：. 开头（.DS_Store、.recycle）或 @ 开头（@eaDir 等群晖/fnOS 元数据目录）。
 * 默认跳过；设置页「忽略隐藏目录」关闭后可重新纳入索引。 */
static int g_skip_hidden = 1;

void scan_set_skip_hidden(int on) { g_skip_hidden = on ? 1 : 0; }
int  scan_skip_hidden(void) { return g_skip_hidden; }
int  scan_name_hidden(const char *name) {
    return name && (name[0] == '.' || name[0] == '@');
}

static int excluded(const char *path) {
    for (size_t i = 0; i < g_excl_n; i++) {
        size_t n = strlen(g_excl[i]);
        if (strncmp(path, g_excl[i], n) == 0 && (path[n] == '/' || path[n] == 0))
            return 1;
    }
    return 0;
}

void store_init(xl_store *s) {
    memset(s, 0, sizeof(*s));
    s->cap = 1u << 16;
    s->recs = xmalloc((size_t)s->cap * sizeof(xl_rec));
    s->names_cap = 1u << 20;
    s->names = xmalloc(s->names_cap);
}

void store_free(xl_store *s) {
    free(s->recs);
    free(s->names);
    memset(s, 0, sizeof(*s));
}

void store_shrink(xl_store *s) {
    s->recs = xrealloc(s->recs, (size_t)(s->n ? s->n : 1) * sizeof(xl_rec));
    s->names = xrealloc(s->names, s->names_len ? s->names_len : 1);
}

volatile uint64_t xl_scan_records;

/* 根文件系统的 st_dev：进入其它设备的挂载点时不递归（等价 find -xdev） */
static dev_t g_scan_dev;

static uint32_t add_rec(xl_store *s, uint32_t parent, const char *name, size_t len,
                        uint8_t flags, uint32_t size, uint32_t mtime) {
    if (s->n == s->cap) {
        s->cap *= 2;
        s->recs = xrealloc(s->recs, (size_t)s->cap * sizeof(xl_rec));
    }
    if (s->names_len + len > s->names_cap) {
        while (s->names_len + len > s->names_cap) s->names_cap *= 2;
        s->names = xrealloc(s->names, s->names_cap);
    }
    xl_rec *r = &s->recs[s->n];
    r->parent   = parent;
    r->name_off = (uint32_t)s->names_len;
    r->size     = size;
    r->mtime    = mtime;
    r->name_len = (uint16_t)len;
    r->flags    = flags;
    r->reserved = 0;
    memcpy(s->names + s->names_len, name, len);
    s->names_len += len;
    xl_scan_records++;
    return s->n++;
}

static int walk(xl_store *s, uint32_t self, const char *path, int want_stat,
                int verbose, int depth) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;

    if (depth > 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_dev != g_scan_dev) {
            /* 其它文件系统的挂载点：容器 overlay 内部跳过；其余继续遍历，
               部分 fnOS 系统上用户家目录是独立子卷（bind/subvol） */
            if (strstr(path, "/overlay2/") || strstr(path, "/merged")) {
                close(fd);
                return 0;
            }
        }
    }

    char buf[64 * 1024] __attribute__((aligned(8)));
    char child[PATH_MAX];
    int rc = 0;

    for (;;) {
        long n = syscall(SYS_getdents64, fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            rc = -1;
            break;
        }
        if (n == 0) break;

        for (long off = 0; off < n;) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            off += d->d_reclen;
            const char *name = d->d_name;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
                continue;
            if (g_skip_hidden && scan_name_hidden(name))
                continue;
            size_t nlen = strlen(name);
            if (nlen == 0 || nlen > XL_NAME_MAX) continue;

            uint8_t flags = 0;
            if (d->d_type == DT_DIR) flags |= XL_F_DIR;
            else if (d->d_type == DT_LNK) flags |= XL_F_LINK;

            uint32_t size = 0, mtime = 0;
            if (want_stat || d->d_type == DT_UNKNOWN) {
                struct stat st;
                if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                    if (!(flags & XL_F_DIR)) size = (uint32_t)st.st_size;
                    mtime = (uint32_t)st.st_mtime;
                }
            }

            uint32_t idx = add_rec(s, self, name, nlen, flags, size, mtime);

            if (verbose && (idx & 0x3FFFFu) == 0)
                fprintf(stderr, "\r  scanned %u", idx);

            if ((flags & XL_F_DIR) && depth < 512) {
                int cl = snprintf(child, sizeof(child), "%s/%s", path, name);
                if (cl > 0 && (size_t)cl < sizeof(child)) {
                    if (excluded(child)) {
                        /* 整棵子树跳过，但目录本身已记录（可搜到入口） */
                        continue;
                    }
                    walk(s, idx, child, want_stat, verbose, depth + 1);
                }
            }
        }
    }

    close(fd);
    return rc;
}

uint32_t store_push_root(xl_store *s, uint8_t flags) {
    return add_rec(s, XL_ROOT, "", 0, flags, 0, 0);
}

uint32_t store_push_meta(xl_store *s, uint32_t parent, const char *name, size_t len,
                         uint8_t flags, uint32_t size, uint32_t mtime) {
    return add_rec(s, parent, name, len, flags, size, mtime);
}

uint32_t store_push(xl_store *s, uint32_t parent, const char *name, size_t len,
                    uint8_t flags) {
    return add_rec(s, parent, name, len, flags, 0, 0);
}

int scan_tree(const char *root, xl_store *s, int want_stat, int verbose) {
    char *r = realpath(root, NULL);
    if (!r) r = strdup(root);
    if (!r) return -1;
    size_t n = strlen(r);
    while (n > 1 && r[n - 1] == '/') r[--n] = 0;
    if (excluded(r)) { free(r); return -1; }

    struct stat rst;
    g_scan_dev = (stat(r, &rst) == 0) ? rst.st_dev : 0;

    uint32_t root_idx = add_rec(s, XL_ROOT, "", 0, XL_F_DIR, 0, 0);
    int rc = walk(s, root_idx, r, want_stat, verbose, 0);
    if (verbose) fprintf(stderr, "\r  scanned %u\n", s->n);
    free(r);
    return rc;
}
