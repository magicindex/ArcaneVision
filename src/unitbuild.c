/* 隔离单元独立扫描（P5）：不再从共享快照过滤。
 *
 * 目录进入条件：主体可穿越 x；目录本身是否可列由查询期 ACL 再复核，
 * 因此“可穿越不可列”的父目录会作为结构节点保留，保证子路径可还原。
 * 文件条件：父目录已进入，且文件对主体可读。
 * 组单元按成员并集扫描，查询期仍按请求 uid 复核，不越权。
 */
#include "unitbuild.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>

static int can_enter(const xl_unit_subject *s, const char *path) {
    if (s->kind == 1) return perm_check(s->uid, path, 0x1 /* X */, 1);
    /* 组单元按成员精确交集：所有成员都能穿越才进入 */
    if (s->nmembers <= 0) return 0;
    for (int i = 0; i < s->nmembers; i++)
        if (!perm_check(s->members[i], path, 0x1, 1)) return 0;
    return 1;
}

static int can_read(const xl_unit_subject *s, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (s->kind == 1) return perm_mode_readable(s->uid, path, &st);
    /* 组单元精确交集：所有成员都可读才记录；无 ACL 时只取一次 xattr。 */
    return perm_mode_readable_all(s->members, s->nmembers, path, &st);
}

static int enter_cb(const char *path, void *ctx) {
    return can_enter((const xl_unit_subject *)ctx, path);
}

static int file_cb(const char *path, void *ctx) {
    const xl_unit_subject *s = (const xl_unit_subject *)ctx;
    /* 卷根下的文件：父级就是扫描根，根本身也必须可穿越 */
    if (s->root) {
        const char *sl = strrchr(path, '/');
        if (sl) {
            size_t rl = strlen(s->root);
            if ((size_t)(sl - path) == rl && !strncmp(path, s->root, rl)) {
                if (!can_enter(s, s->root)) return 0;
            }
        }
    }
    return can_read(s, path);
}

int xl_unit_scan_to_file(const char *root, const xl_unit_subject *subj,
                         const char *out_path) {
    if (!root || !subj || !out_path) return -1;
    xl_unit_subject local = *subj;
    local.root = root;
    xl_store st;
    store_init(&st);
    int old_team = scan_include_team();
    scan_set_include_team(1);          /* 单元扫描始终放行卷根 @team */
    int rc = scan_tree_scoped(root, &st, 1, 0, enter_cb, file_cb, (void *)&local);
    scan_set_include_team(old_team);
    if (rc < 0 && st.n < 2) {
        store_free(&st);
        return -1;
    }
    store_shrink(&st);

    xl_index ix;
    index_init(&ix);
    index_build(&ix, &st, 0);

    char tmp[1400];
    int w = snprintf(tmp, sizeof tmp, "%s.tmp-%ld", out_path, (long)getpid());
    int ret = -1;
    if (w > 0 && (size_t)w < sizeof tmp) {
        if (snapshot_save(tmp, &st, &ix, root, 0) == 0) {
            chmod(tmp, 0600);
            if (rename(tmp, out_path) == 0) ret = 0;
            else unlink(tmp);
        } else {
            unlink(tmp);
        }
    }
    index_free(&ix);
    store_free(&st);
    return ret;
}


static int store_add_copy(xl_store *s, const char *name, size_t len,
                          uint32_t parent, uint8_t flags,
                          uint32_t size, uint32_t mtime) {
    if (len > XL_NAME_MAX) return -1;
    if (s->n == s->cap) {
        uint32_t nc = s->cap ? s->cap * 2 : 256;
        xl_rec *nr = realloc(s->recs, (size_t)nc * sizeof(*nr));
        if (!nr) return -1;
        s->recs = nr; s->cap = nc;
    }
    while (s->names_len + len > s->names_cap) {
        size_t nc = s->names_cap ? s->names_cap * 2 : 4096;
        char *nb = realloc(s->names, nc);
        if (!nb) return -1;
        s->names = nb; s->names_cap = nc;
    }
    xl_rec *r = &s->recs[s->n];
    r->parent = parent;
    r->name_off = (uint32_t)s->names_len;
    r->size = size;
    r->mtime = mtime;
    r->name_len = (uint16_t)len;
    r->flags = flags;
    r->reserved = 0;
    memcpy(s->names + s->names_len, name, len);
    s->names_len += len;
    return (int)s->n++;
}

static int path_descendant(const char *path, const char *dir) {
    size_t dl = strlen(dir);
    return strncmp(path, dir, dl) == 0 && path[dl] == '/';
}
static int path_equal(const char *a, const char *b) { return !strcmp(a, b); }

int xl_unit_patch_to_file(const char *root, const char *changed,
                          const xl_unit_subject *subj,
                          const char *old_path, const char *out_path) {
    if (!root || !changed || !subj || !old_path || !out_path) return -1;
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", changed);
    struct stat st;
    if (stat(dir, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) {
        char *sl = strrchr(dir, '/');
        if (!sl || sl == dir) return -1;
        *sl = 0;
    }
    size_t rl = strlen(root);
    if (strncmp(dir, root, rl) != 0 || (dir[rl] != 0 && dir[rl] != '/')) return -1;

    xl_snap sn;
    if (snapshot_open(old_path, &sn) != 0) return -1;
    xl_store dst;
    store_init(&dst);
    int *omap = malloc((size_t)sn.h.rec_count * sizeof(int));
    if (!omap) { snapshot_close(&sn); store_free(&dst); return -1; }
    for (uint32_t i = 0; i < (uint32_t)sn.h.rec_count; i++) omap[i] = -1;

    char path[PATH_MAX];
    uint32_t dir_idx = UINT32_MAX;
    int bad = 0;
    for (uint32_t i = 0; i < (uint32_t)sn.h.rec_count && !bad; i++) {
        snap_path(&sn, i, path, sizeof path);
        if (i > 0 && path_descendant(path, dir)) continue;
        const xl_rec *r = &sn.recs[i];
        uint32_t parent = XL_ROOT;
        if (r->parent != XL_ROOT) {
            if (r->parent >= i || omap[r->parent] < 0) { bad = 1; break; }
            parent = (uint32_t)omap[r->parent];
        }
        const char *name = sn.names + r->name_off;
        int ni = store_add_copy(&dst, name, r->name_len, parent, r->flags,
                                r->size, r->mtime);
        if (ni < 0) { bad = 1; break; }
        omap[i] = ni;
        if (path_equal(path, dir)) dir_idx = (uint32_t)ni;
    }
    snapshot_close(&sn);
    if (bad || dir_idx == UINT32_MAX) { free(omap); store_free(&dst); return -1; }

    xl_store sub;
    store_init(&sub);
    xl_unit_subject local = *subj;
    local.root = root;
    int old_team = scan_include_team();
    scan_set_include_team(1);
    int rc = scan_tree_scoped(dir, &sub, 1, 0, enter_cb, file_cb, &local);
    scan_set_include_team(old_team);
    if (rc < 0) { free(omap); store_free(&dst); store_free(&sub); return -1; }

    int *smap = malloc((size_t)(sub.n ? sub.n : 1) * sizeof(int));
    if (!smap) { free(omap); store_free(&dst); store_free(&sub); return -1; }
    for (uint32_t i = 0; i < sub.n; i++) smap[i] = -1;
    smap[0] = (int)dir_idx;
    for (uint32_t i = 1; i < sub.n; i++) {
        const xl_rec *r = &sub.recs[i];
        int parent;
        if (r->parent == XL_ROOT) parent = (int)dir_idx;
        else if (r->parent < i && smap[r->parent] >= 0) parent = smap[r->parent];
        else { bad = 1; break; }
        const char *name = sub.names + r->name_off;
        int ni = store_add_copy(&dst, name, r->name_len, (uint32_t)parent,
                                r->flags, r->size, r->mtime);
        if (ni < 0) { bad = 1; break; }
        smap[i] = ni;
    }
    free(smap);
    free(omap);
    store_free(&sub);
    if (bad) { store_free(&dst); return -1; }

    xl_index ix;
    index_init(&ix);
    index_build(&ix, &dst, 0);
    char tmp[1400];
    int w = snprintf(tmp, sizeof tmp, "%s.tmp-patch-%ld", out_path, (long)getpid());
    int ret = -1;
    if (w > 0 && (size_t)w < sizeof tmp) {
        if (snapshot_save(tmp, &dst, &ix, root, 0) == 0) {
            chmod(tmp, 0600);
            if (rename(tmp, out_path) == 0) ret = 0;
            else unlink(tmp);
        } else unlink(tmp);
    }
    index_free(&ix);
    store_free(&dst);
    return ret;
}
