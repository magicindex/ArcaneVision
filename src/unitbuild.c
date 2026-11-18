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
    /* 组单元精确交集：所有成员都可读才记录 */
    if (s->nmembers <= 0) return 0;
    for (int i = 0; i < s->nmembers; i++)
        if (!perm_mode_readable(s->members[i], path, &st)) return 0;
    return 1;
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
