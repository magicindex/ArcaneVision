/* TrimACL 求值：读 system.trim_acl xattr，缺失时向上找最近带 ACL 的祖先，
 * 再缺失才回落到 POSIX mode。
 *
 * xattr 布局（实测 fnOS TrimACL v0.8.3）：
 *   u32 version = 1
 *   之后每 20 字节一个条目：
 *     u32 type_tag   bit0 allow / bit1 deny；0x04 user / 0x08 group /
 *                    0x10 everyone / 0x20 owner
 *     u32 perms      0x1 x、0x2 w、0x4 r、0x8 p、0x40 e、0x80 c、
 *                    0x100 a、0x200 A、0x400 E、0x2000 D
 *     u32 inherit    0x1 self / 0x2 file / 0x4 dir；0x4000 = exclude inheritance 标记
 *     u32 id         uid / gid；owner 与 everyone 为 0xffffffff
 *     u32 reserved
 */
#include "xl.h"

#include <grp.h>
#include <pwd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#define A_ALLOW 0x01
#define A_DENY  0x02
#define A_USER  0x04
#define A_GROUP 0x08
#define A_EVERY 0x10
#define A_OWNER 0x20
#define A_X     0x01
#define A_R     0x04
#define A_INH_DIR 0x04
#define A_INH_EXCL 0x4000

/* 判断「目标 uid」是否属于 gid —— 不能用进程自身的组，
 * 否则 root 进程会把所有 root:root 目录判成组可访问。 */
static int uid_in_group(uid_t uid, gid_t gid) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static uid_t cached_uid = (uid_t)-1;
    static gid_t list[64];
    static int cnt = 0;

    pthread_mutex_lock(&mu);
    if (cached_uid != uid) {
        struct passwd *pw = getpwuid(uid);
        cnt = 0;
        cached_uid = uid;
        if (pw) {
            gid_t gl[64];
            int n = 64;
            if (getgrouplist(pw->pw_name, pw->pw_gid, gl, &n) >= 0) {
                if (n > 64) n = 64;
                memcpy(list, gl, sizeof(gid_t) * (size_t)n);
                cnt = n;
            } else {
                list[0] = pw->pw_gid;
                cnt = 1;
            }
        }
    }
    int hit = 0;
    for (int i = 0; i < cnt; i++) if (list[i] == gid) { hit = 1; break; }
    pthread_mutex_unlock(&mu);
    return hit;
}

/* 1 = 允许列目录，0 = 拒绝，-1 = 该级没有可用 ACL（继续向上找） */
static int posix_bit(const struct stat *st, uid_t uid, int need) {
    int ok;
    if (st->st_uid == uid)      ok = (st->st_mode >> 6) & 7;
    else if (uid_in_group(uid, st->st_gid)) ok = (st->st_mode >> 3) & 7;
    else                        ok = st->st_mode & 7;
    return (ok & need) ? 1 : 0;
}

/* need: A_R 或 A_X；self=1 只看本级自己的 ACL 条目 */
static int eval_one(uid_t uid, const char *dir, int self, int need) {
    struct stat st;
    if (stat(dir, &st) != 0) return -1;

    unsigned char buf[4096];
    ssize_t n = getxattr(dir, "system.trim_acl", buf, sizeof buf);
    if (n < 4) return -1;
    uint32_t ver;
    memcpy(&ver, buf, 4);
    if (ver != 1) return -1;

    uint32_t granted = 0, denied = 0;
    int matched = 0;
    size_t nent = ((size_t)n - 4) / 20;
    for (size_t i = 0; i < nent; i++) {
        const unsigned char *p = buf + 4 + i * 20;
        uint32_t type_tag, perms, inherit, id;
        memcpy(&type_tag, p + 0, 4);
        memcpy(&perms, p + 4, 4);
        memcpy(&inherit, p + 8, 4);
        memcpy(&id, p + 12, 4);
        if (inherit & A_INH_EXCL) continue;              /* 边界标记 */
        if (!self && !(inherit & A_INH_DIR)) continue;   /* 只认可继承到子目录的条目 */

        uint32_t tag = type_tag & 0x3c;
        int hit = 0;
        if (tag == A_EVERY) hit = 1;
        else if (tag == A_OWNER) hit = (st.st_uid == uid);
        else if (tag == A_USER) hit = (id == (uint32_t)uid);
        else if (tag == A_GROUP) hit = uid_in_group(uid, (gid_t)id);
        if (!hit) continue;
        matched = 1;
        if (type_tag & A_DENY) denied |= perms;
        else if (type_tag & A_ALLOW) granted |= perms;
    }
    if (!matched) return 0;   /* 本级有 ACL 但没有匹配项 = 拒绝，不再向上找 */
    if (denied & need) return 0;
    return (granted & need) ? 1 : 0;
}

static int eval_ancestors(uid_t uid, const char *dir, int need) {
    char buf[8192];
    size_t n = strlen(dir);
    if (n == 0 || n >= sizeof buf) return -1;
    memcpy(buf, dir, n + 1);
    for (int depth = 0; depth < 64; depth++) {
        char *sl = strrchr(buf, '/');
        if (!sl) return -1;
        if (sl == buf) { if (buf[1] == 0) return -1; buf[1] = 0; }
        else *sl = 0;
        int r = eval_one(uid, buf, 0, need);
        if (r >= 0) return r;
    }
    return -1;
}

/* 单级判定：本级 ACL 优先，其次最近祖先的 ACL，最后 POSIX */
static int level_ok(uid_t uid, const char *dir, int need) {
    /* 目录属主即本人：飞牛的家目录 mode 常为 0000 且 ACL 只授权给管理员，
       属主本人仍应可访问（文件管理器里也是这么表现的）。 */
    {
        struct stat st;
        if (stat(dir, &st) == 0 && st.st_uid == uid) return 1;
    }
    int r = eval_one(uid, dir, 1, need);
    if (r >= 0) return r;
    r = eval_ancestors(uid, dir, need);
    if (r >= 0) return r;
    struct stat st;
    if (stat(dir, &st) != 0) return 0;
    r = posix_bit(&st, uid, need);
    return r;
}

/* 列目录：路径上每一级都要可穿过(x)，目标目录本身要可读(r)。
 * 只判最后一级会漏掉 /vol1/docker(0710) 这类祖先拒绝。 */
int perm_dir_listable(uid_t uid, const char *dir) {
    if (uid == 0) return 1;
    char buf[8192];
    size_t n = strlen(dir);
    if (n == 0 || n >= sizeof buf) return 0;
    memcpy(buf, dir, n + 1);

    if (!level_ok(uid, buf, A_X)) return 0;   /* 目标目录自身可穿过 */
    for (;;) {
        char *sl = strrchr(buf, '/');
        if (!sl || sl == buf) break;
        *sl = 0;
        /* 卷根 /volN 在飞牛里 mode 0，但内核允许穿越（trimacl 之外的特例），不参与判定 */
        if (sl - buf >= 4 && strncmp(buf, "/vol", 4) == 0) {
            size_t k = 4;
            while (buf[k] >= '0' && buf[k] <= '9') k++;
            if (k == (size_t)(sl - buf)) break;
        }
            if (!level_ok(uid, buf, A_X)) return 0;
    }
    return level_ok(uid, dir, A_R);
}
