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

#include <errno.h>
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
#define A_W     0x02
#define A_R     0x04
#define A_INH_FILE 0x02
#define A_INH_DIR 0x04
#define A_INH_EXCL 0x4000

/* 判断「目标 uid」是否属于 gid —— 不能用进程自身的组，
 * 否则 root 进程会把所有 root:root 目录判成组可访问。 */
int perm_strict_home = 1;

void perm_set_strict_home(int on) { perm_strict_home = on ? 1 : 0; }

/* /volN/<数字>（家目录）及其子路径 */
static int under_home(const char *dir) {
    if (strncmp(dir, "/vol", 4) != 0) return 0;
    const char *p = dir + 4;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') p++;
    if (*p != '/') return 0;
    p++;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') p++;
    return (*p == '/' || *p == 0);
}

static int uid_in_group(uid_t uid, gid_t gid);

/* fnOS 内核对 Administrators 组（惯例 gid 1000）放行整卷读写，与 root 等价。
 * 不跟随这条规则，管理员在玄览里会看不到内核实际可读的目录（如 NextCloud
 * data 下由 www-data 持有、只授 owner 的子树）。 */
static gid_t admin_gid(void) {
    static gid_t cached = (gid_t)-2;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_lock(&mu);
    if (cached == (gid_t)-2) {
        struct group *gr = getgrnam("Administrators");
        cached = gr ? gr->gr_gid : (gid_t)-1;
    }
    pthread_mutex_unlock(&mu);
    return cached;
}

static int uid_is_admin(uid_t uid) {
    gid_t g = admin_gid();
    return g != (gid_t)-1 && uid_in_group(uid, g);
}

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

/* need: A_R / A_W / A_X 的组合；self=1 只看本级自己的 ACL 条目。
 * is_dir 决定祖先 ACL 是否按「继承到目录」还是「继承到文件」筛选。 */
static int eval_one(uid_t uid, const char *dir, int self, int need, int is_dir) {
    struct stat st;
    if (stat(dir, &st) != 0) return -1;

    unsigned char stackbuf[4096];
    unsigned char *buf = stackbuf;
    ssize_t n = getxattr(dir, "system.trim_acl", stackbuf, sizeof stackbuf);
    if (n < 0 && errno == ERANGE) {
        ssize_t sz = getxattr(dir, "system.trim_acl", NULL, 0);
        if (sz < 4 || sz > (1 << 20)) return -1;
        buf = malloc((size_t)sz);
        if (!buf) return -1;
        n = getxattr(dir, "system.trim_acl", buf, (size_t)sz);
    }
    if (n < 4) {
        if (buf != stackbuf) free(buf);
        return -1;
    }
    uint32_t ver;
    memcpy(&ver, buf, 4);
    if (ver != 1) {
        if (buf != stackbuf) free(buf);
        return -1;
    }

    uint32_t granted = 0, denied = 0;
    int matched = 0;
    int strict = perm_strict_home && under_home(dir);
    uint32_t inh_want = is_dir ? A_INH_DIR : A_INH_FILE;
    size_t nent = ((size_t)n - 4) / 20;
    for (size_t i = 0; i < nent; i++) {
        const unsigned char *p = buf + 4 + i * 20;
        uint32_t type_tag, perms, inherit, id;
        memcpy(&type_tag, p + 0, 4);
        memcpy(&perms, p + 4, 4);
        memcpy(&inherit, p + 8, 4);
        memcpy(&id, p + 12, 4);
        if (inherit & A_INH_EXCL) continue;              /* 边界标记 */
        if (!self && !(inherit & inh_want)) continue;     /* 只认可继承到同类目标的条目 */

        uint32_t tag = type_tag & 0x3c;
        int hit = 0;
        if (tag == A_EVERY) { if (strict && need != A_X && !(type_tag & A_DENY)) continue; hit = 1; }
        else if (tag == A_OWNER) hit = (st.st_uid == uid);
        else if (tag == A_USER) hit = (id == (uint32_t)uid);
        else if (tag == A_GROUP) hit = uid_in_group(uid, (gid_t)id);
        if (!hit) continue;
        matched = 1;
        if (type_tag & A_DENY) denied |= perms;
        else if (type_tag & A_ALLOW) granted |= perms;
    }
    int rc;
    if (!matched) rc = 0;     /* 本级有 ACL 但没有匹配项 = 拒绝，不再向上找 */
    else if (denied & need) rc = 0;
    else rc = (granted & need) ? 1 : 0;
    if (buf != stackbuf) free(buf);
    return rc;
}

static int eval_ancestors(uid_t uid, const char *dir, int need, int is_dir) {
    char buf[8192];
    size_t n = strlen(dir);
    if (n == 0 || n >= sizeof buf) return -1;
    memcpy(buf, dir, n + 1);
    for (int depth = 0; depth < 64; depth++) {
        char *sl = strrchr(buf, '/');
        if (!sl) return -1;
        if (sl == buf) { if (buf[1] == 0) return -1; buf[1] = 0; }
        else *sl = 0;
        /* 卷根 /volN 在飞牛里 mode 0，不算权限边界 */
        {
            size_t L = strlen(buf);
            if (L >= 4 && strncmp(buf, "/vol", 4) == 0) {
                size_t k = 4;
                while (buf[k] >= '0' && buf[k] <= '9') k++;
                if (k == L) return -1;
            }
        }
        int r = eval_one(uid, buf, 0, need, is_dir);
        if (r >= 0) return r;
    }
    return -1;
}

/* 单级判定：本级 ACL 优先，其次最近祖先的 ACL，最后 POSIX */
static int level_ok(uid_t uid, const char *path, int need, int is_dir) {
    struct stat st;
    /* 属主本人可访问自己的家目录（飞牛家目录常见 mode 0000 + ACL 只授管理员）。
       写权限不做这个放行，否则 0555 的目录也会被判定为可写。 */
    if (stat(path, &st) == 0 && st.st_uid == uid && !(need & A_W)) return 1;
    int r = eval_one(uid, path, 1, need, is_dir);
    if (r >= 0) return r;
    r = eval_ancestors(uid, path, need, is_dir);
    if (r >= 0) return r;
    if (stat(path, &st) != 0) return 0;
    if (perm_strict_home && under_home(path)) {
        /* 严格模式：家目录内不认 others 位 */
        if (st.st_uid == uid) return (((st.st_mode >> 6) & 7) & need) ? 1 : 0;
        if (uid_in_group(uid, st.st_gid)) return (((st.st_mode >> 3) & 7) & need) ? 1 : 0;
        /* 严格模式只收紧"列目录/读"，仍允许 others 的 x 用于穿越到被显式授权的子目录 */
        if (need == A_X) return ((st.st_mode & 7) & A_X) ? 1 : 0;
        return 0;
    }
    return posix_bit(&st, uid, need);
}

static int is_vol_root(const char *p) {
    if (strncmp(p, "/vol", 4) != 0) return 0;
    const char *q = p + 4;
    if (*q < '0' || *q > '9') return 0;
    while (*q >= '0' && *q <= '9') q++;
    return *q == 0;
}

/* 通用路径判定：祖先逐级 X，目标按 need 判定。
 * is_dir=1 用于目录（继承目录 ACL），0 用于普通文件（继承文件 ACL）。 */
int perm_check(uid_t uid, const char *path, int need, int is_dir) {
    if (uid == 0 || uid_is_admin(uid)) return 1;
    if (!path || !*path) return 0;
    char buf[8192];
    size_t n = strlen(path);
    if (n >= sizeof buf) return 0;
    memcpy(buf, path, n + 1);

    for (int depth = 0; depth < 64; depth++) {
        char *sl = strrchr(buf, '/');
        if (!sl || sl == buf) break;
        *sl = 0;
        /* 卷根 /volN 在飞牛里 mode 0，但内核允许穿越，不参与判定 */
        if (is_vol_root(buf)) break;
        if (!level_ok(uid, buf, A_X, 1)) return 0;
    }
    return level_ok(uid, path, need, is_dir);
}

/* 列目录：路径上每一级都要可穿过(x)，目标目录本身要可读(r)。 */
int perm_dir_listable(uid_t uid, const char *dir) {
    return perm_check(uid, dir, A_R | A_X, 1);
}

/* 普通文件可读：祖先只需 X，目标只需 R（缺 ACL 时回落 POSIX）。 */
int perm_file_readable(uid_t uid, const char *path) {
    return perm_check(uid, path, A_R, 0);
}

/* 目录可写（创建/删除/重命名子项需要父目录 W+X）。 */
int perm_dir_writable(uid_t uid, const char *dir) {
    return perm_check(uid, dir, A_W | A_X, 1);
}

int perm_parent_writable(uid_t uid, const char *path) {
    if (!path) return 0;
    char buf[8192];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) return 0;
    memcpy(buf, path, n + 1);
    char *sl = strrchr(buf, '/');
    if (!sl || sl == buf) return 0;
    *sl = 0;
    return perm_dir_writable(uid, buf);
}

/* 列表场景的轻量判定：只看目标文件自身 ACL/POSIX，不重复走祖先链。
 * 调用方应已确认父目录可列。 */
int perm_mode_readable(uid_t uid, const char *path, const struct stat *st) {
    if (uid == 0 || uid_is_admin(uid)) return 1;
    int r = eval_one(uid, path, 1, A_R, 0);
    if (r >= 0) return r;
    /* TRIM/ZFS 的 owner ACE 允许属主读取 mode 000 的文件（SMB 迁移数据常见）；
     * 与 level_ok() 的属主语义保持一致，否则这些文件会被目录列表误过滤。 */
    if (st && st->st_uid == uid) return 1;
    return posix_bit(st, uid, A_R);
}
