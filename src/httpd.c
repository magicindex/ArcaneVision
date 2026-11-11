/* 玄览 / Arcane Vision — 常驻服务
 * 每个挂载点一份快照；查询时各卷并行求交后按 mtime 归并。
 * 同时监听 TCP 端口与 unix socket（fnOS 应用网关的 app.sock）。
 */
#include "xl.h"
#include "fsops.h"
#include "markdown.h"
#include "version.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <limits.h>
#include <pwd.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <iconv.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_VOL   8
#define SCOPE_MAX 512   /* 每个用户的检索范围条目上限（含排除项） */
#define N_WORKERS 4
#define Q_CAP     128
#define REQ_MAX   131072   /* 头 + body；设置里的检索范围可能较大 */

/* ---------- 动态缓冲 ---------- */
typedef struct { char *p; size_t n, cap; } sbuf;
static void send_simple(int fd, int code, const char *json);

/* 轻量 JSON 语法校验：顶层必须是对象/数组，检查括号配对、字符串闭合与尾部垃圾。
 * 只用于拒绝明显截断/畸形的请求，不做类型与字段校验。 */
static int json_ok(const char *s) {
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '{' && *p != '[') return 0;
    int stack[64], sp = 0, in_str = 0, esc = 0;
    for (; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (in_str) {
            if (esc) { esc = 0; continue; }
            if (c == '\\') { esc = 1; continue; }
            if (c == '"') { in_str = 0; continue; }
            if (c < 0x20) return 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == '{' || c == '[') {
            if (sp >= (int)(sizeof stack / sizeof stack[0])) return 0;
            stack[sp++] = c;
        } else if (c == '}' || c == ']') {
            if (!sp) return 0;
            char o = (char)stack[--sp];
            if ((c == '}' && o != '{') || (c == ']' && o != '[')) return 0;
            if (!sp) {
                p++;
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                return *p == 0;
            }
        }
    }
    return 0;
}

static void sb_reserve(sbuf *b, size_t extra) {
    if (b->n + extra + 1 <= b->cap) return;
    size_t want = b->cap ? b->cap : 4096;
    while (want < b->n + extra + 1) want *= 2;
    b->p = xrealloc(b->p, want);
    b->cap = want;
}

static void sb_add(sbuf *b, const char *s, size_t n) {
    sb_reserve(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sb_puts(sbuf *b, const char *s) { sb_add(b, s, strlen(s)); }

static void sb_printf(sbuf *b, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) { sb_add(b, tmp, (size_t)n); return; }
    char *big = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb_add(b, big, (size_t)n);
    free(big);
}

static void sb_json(sbuf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') sb_printf(b, "\\%c", c);
        else if (c == '\n') sb_puts(b, "\\n");
        else if (c == '\r') sb_puts(b, "\\r");
        else if (c == '\t') sb_puts(b, "\\t");
        else if (c < 0x20) sb_printf(b, "\\u%04x", c);
        else sb_add(b, (const char *)&c, 1);
    }
}

/* ---------- 全局状态 ---------- */
typedef struct {
    char     root[256];
    char     file[1024];
    xl_snap  sn;
    int      ok;
    int      building;
    int      usb;                 /* 1 = USB 外挂卷，独立快照 */
    char     usb_id[128];         /* 文件系统 UUID（挂载名变化时用来认同一设备） */
    uint64_t records;
    uint64_t built_at;
    uint64_t build_ms;
    /* 目录子项索引：parent -> 子记录链表，避免每次列目录扫全量记录 */
    uint32_t *dir_head;
    uint32_t *dir_next;
    uint32_t root_id;
    int      dir_ready;
} vol_t;

static void dir_index_free(vol_t *v);
static void dir_index_build(vol_t *v);

static vol_t g_vols[MAX_VOL];
static int   g_nvols;
static int   g_nvols_local;       /* 前 g_nvols_local 个是 /volN 本地卷 */

/* USB 外挂设备检索（默认关闭，单独索引，/vol00 挂载） */
static int  g_usb_enabled;
static int  g_usb_watch = 1;
static int  g_usb_had;
static char g_usb_msg[1024] = "USB 检索未开启";
static pthread_mutex_t g_usb_msg_mu = PTHREAD_MUTEX_INITIALIZER;
static void usb_msg_set(const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_usb_msg_mu);
    memcpy(g_usb_msg, tmp, sizeof g_usb_msg);
    g_usb_msg[sizeof g_usb_msg - 1] = 0;
    pthread_mutex_unlock(&g_usb_msg_mu);
}
static void usb_msg_get(char *out, size_t cap) {
    pthread_mutex_lock(&g_usb_msg_mu);
    snprintf(out, cap, "%s", g_usb_msg);
    pthread_mutex_unlock(&g_usb_msg_mu);
}
static char g_usb_sig[4096];
static uint64_t g_usb_last_tick;
static int  g_usb_missing_ticks;
static pthread_rwlock_t g_rw = PTHREAD_RWLOCK_INITIALIZER;

static volatile sig_atomic_t g_stop;
/* 网关不一定转发 x-trim-isadmin，这里补一个兜底：
 * 飞牛把首个普通用户（最小的 uid>=1000）视为管理员。
 * 可用 XL_ADMIN_FALLBACK=0 关闭兜底，只信网关头。 */
static int admin_fallback_enabled(void) {
    const char *e = getenv("XL_ADMIN_FALLBACK");
    if (!e) return 1;
    return !(e[0] == '0' || e[0] == 'n' || e[0] == 'N' || e[0] == 'f' || e[0] == 'F');
}
static int admin_by_uid(int uid) {
    if (uid < 1000) return 0;
    static int cached_first = -2;
    static time_t cached_at;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    time_t now = time(NULL);
    pthread_mutex_lock(&mu);
    if (cached_first == -2 || now - cached_at > 60) {
        int first = -1;
        struct passwd *pw;
        setpwent();
        while ((pw = getpwent()) != NULL) {
            if ((int)pw->pw_uid >= 1000 && (first < 0 || (int)pw->pw_uid < first))
                first = (int)pw->pw_uid;
        }
        endpwent();
        cached_first = first;
        cached_at = now;
    }
    pthread_mutex_unlock(&mu);
    return uid == cached_first;
}

/* 当前请求的 admin 结论（由 handle_client 按网关头/兜底算好后写入 TLS） */
static _Thread_local int g_req_admin = -1;

/* USB 外挂盘（/vol00）默认只对管理员账号开放 */
static int uid_is_admin(int uid) {
    if (uid == 0) return 1;
    if (g_req_admin >= 0) return g_req_admin;
    return admin_by_uid(uid);
}
static int usb_path_blocked(int uid, const char *p) {
    return !uid_is_admin(uid) && p && !strncmp(p, "/vol00/", 7);
}
static volatile uint64_t g_last_rebuild;
/* 自动全盘扫描计划：mode 0=关 1=每天 2=每 N 小时 */
typedef struct { int mode; int hour; int minute; int hours; } rebuild_cfg;
static rebuild_cfg g_rcfg = {1, 3, 0, 6};   /* 默认：每天 03:00 */
static pthread_mutex_t g_rcfg_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_last_run_yday = -1;
static int g_acl_uid = -1;      /* >=0 时按该 uid 过滤不可见目录 */
static volatile int g_building;
static volatile int g_failed;
static volatile int g_cur_vol = -1;
static volatile uint64_t g_build_start;
static volatile uint64_t g_build_ms;
static volatile uint64_t g_floor_after;   /* 单卷记录数，用于进度下限 */
static char g_err[256];
static char g_idx_dir[1024];
static char g_trash[1100];

/* ---------- 设备解析 ---------- */
static int mount_dev_for(const char *root, char *dev, size_t cap) {
    FILE *f = fopen("/proc/self/mounts", "r");
    if (!f) return 0;
    char line[4096];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char d[2048], m[2048];
        if (sscanf(line, "%2047s %2047s", d, m) != 2) continue;
        char *out = m, *in = m;
        while (*in) {
            if (in[0] == '\\' && in[1] >= '0' && in[1] <= '7' && in[2] && in[3]) {
                *out++ = (char)(((in[1] - '0') << 6) | ((in[2] - '0') << 3) | (in[3] - '0'));
                in += 4;
            } else *out++ = *in++;
        }
        *out = 0;
        if (strcmp(m, root) == 0) {
            size_t dl = strlen(d);
            if (dl >= cap) break;
            memcpy(dev, d, dl + 1);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}


/* /proc/self/mounts 里的设备名可能是 /dev/mapper/xxx，容器内不存在；
 * 用根目录的 st_dev 到 /sys/dev/block 反查真实节点（如 /dev/dm-1）。 */
static int resolve_dev(const char *root, char *dev, size_t cap) {
    if (mount_dev_for(root, dev, cap) && access(dev, F_OK) == 0)
        return 1;
    struct stat st;
    if (stat(root, &st) != 0) return 0;
    char link[128], path[256];
    snprintf(link, sizeof(link), "/sys/dev/block/%u:%u", major(st.st_dev), minor(st.st_dev));
    ssize_t n = readlink(link, path, sizeof(path) - 1);
    if (n <= 0) return 0;
    path[n] = 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char out[320];
    int m = snprintf(out, sizeof(out), "/dev/%s", base);
    if (m <= 0 || (size_t)m >= cap) return 0;
    memcpy(dev, out, (size_t)m + 1);
    return access(dev, F_OK) == 0;
}

/* 根目录下是否存在"真正"的子挂载（排除 docker overlay）：
 * 有子挂载时裸设备扫描读不到它们，改用目录遍历。 */
static int has_submounts(const char *root) {
    FILE *f = fopen("/proc/self/mounts", "r");
    if (!f) return 0;
    char line[4096];
    size_t rl = strlen(root);
    int hit = 0;
    while (fgets(line, sizeof line, f)) {
        char mnt[1024];
        if (sscanf(line, "%*s %1023s", mnt) != 1) continue;
        if (strncmp(mnt, root, rl) != 0 || mnt[rl] != '/') continue;
        if (strstr(mnt, "/overlay2/") || strstr(mnt, "/merged")) continue;
        hit = 1;
        break;
    }
    fclose(f);
    return hit;
}

/* ---------- 构建 ---------- */
static int build_one(int vi, int verbose) {
    vol_t *v = &g_vols[vi];
    xl_store st;
    store_init(&st);
    xl_scan_records = 0;
    uint64_t t0 = now_ms();
    int rc;
    char dev[256];
    int no_raw = getenv("XL_NO_RAW") != NULL;
    if (!no_raw && has_submounts(v->root)) {
        if (verbose) fprintf(stderr, "xuanlan: %s 含子挂载，改用目录遍历\n", v->root);
        no_raw = 1;
    }
    /* ext4 裸扫不经过 scan.c 的排除表；有排除项时退回目录遍历以保证语义正确 */
    if (!no_raw && scan_has_exclude()) {
        if (verbose) fprintf(stderr, "xuanlan: 存在排除项，改用目录遍历\n");
        no_raw = 1;
    }
    if (!no_raw && resolve_dev(v->root, dev, sizeof(dev)) && strncmp(dev, "/dev/", 5) == 0) {
        if (verbose) fprintf(stderr, "xuanlan: %s <- %s\n", v->root, dev);
        rc = fsreader_ext4_scan(dev, v->root, &st, verbose);
        if (rc < 0 || st.n < 2) {
            store_free(&st);
            store_init(&st);
            rc = scan_tree(v->root, &st, 1, verbose);
        }
    } else {
        rc = scan_tree(v->root, &st, 1, verbose);
    }
    if (rc < 0 && st.n < 2) {
        snprintf(g_err, sizeof(g_err), "%s: 无法读取", v->root);
        store_free(&st);
        return -1;
    }
    store_shrink(&st);
    uint64_t scan_ms = now_ms() - t0;

    xl_index ix;
    index_init(&ix);
    index_build(&ix, &st, verbose);

    char tmp[1100];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", v->file) >= (int)sizeof(tmp)) {
        store_free(&st); index_free(&ix); return -1;
    }
    if (snapshot_save(tmp, &st, &ix, v->root, scan_ms) < 0) {
        snprintf(g_err, sizeof(g_err), "%s: 写快照失败", v->root);
        unlink(tmp);
        store_free(&st); index_free(&ix);
        return -1;
    }
    if (rename(tmp, v->file) != 0) {
        snprintf(g_err, sizeof(g_err), "%s: 替换快照失败: %s", v->root, strerror(errno));
        unlink(tmp);
        store_free(&st); index_free(&ix);
        return -1;
    }
    store_free(&st);
    index_free(&ix);

    xl_snap ns;
    if (snapshot_open(v->file, &ns) < 0) {
        snprintf(g_err, sizeof(g_err), "%s: 快照打开失败", v->root);
        return -1;
    }
    pthread_rwlock_wrlock(&g_rw);
    if (v->ok) { dir_index_free(v); snapshot_close(&v->sn); }
    v->sn = ns;
    v->ok = 1;
    v->records = ns.h.rec_count;
    v->built_at = (uint64_t)time(NULL);
    v->build_ms = now_ms() - t0;
    dir_index_build(v);
    pthread_rwlock_unlock(&g_rw);
    return 0;
}

static void *build_worker(void *arg) {
    (void)arg;
    scan_set_progress(&xl_scan_records);
    g_building = 1;
    g_failed = 0;
    g_err[0] = 0;
    g_build_start = now_ms();
    live_clear();                      /* 基线即将重建，旧 delta 作废 */
    for (int i = 0; i < g_nvols_local && !g_stop; i++) {
        g_cur_vol = i;
        g_floor_after = 0;
        xl_scan_records = 0;
        if (build_one(i, 0) < 0) g_failed = 1;
        g_floor_after = 0;
    }
    g_cur_vol = -1;
    g_build_ms = now_ms() - g_build_start;
    g_last_rebuild = now_ms();
    g_building = 0;
    return NULL;
}

static void start_build(void) {
    if (g_building) return;
    pthread_t th;
    g_building = 1;
    if (pthread_create(&th, NULL, build_worker, NULL) != 0) {
        g_building = 0;
        g_failed = 1;
        snprintf(g_err, sizeof(g_err), "无法创建构建线程");
        return;
    }
    pthread_detach(th);
}


/* ---------- USB 外挂设备（fnOS 外接存储挂载在 /vol00/ 下） ---------- */
#define MAX_USB 8

typedef struct { char root[512]; char dev[256]; char id[128]; } usb_mount;

static int usb_json_bool(const char *b, const char *key, int def) {
    if (!b) return def;
    const char *k = strstr(b, key);
    if (!k) return def;
    const char *c = strchr(k + strlen(key), ':');
    if (!c) return def;
    c++;
    while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') c++;
    if (!strncmp(c, "true", 4)) return 1;
    if (!strncmp(c, "false", 5)) return 0;
    return def;
}

static void usb_unescape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < cap; i++) {
        if (in[i] == '\\' && in[i+1] >= '0' && in[i+1] <= '7' &&
            in[i+2] >= '0' && in[i+2] <= '7' && in[i+3] >= '0' && in[i+3] <= '7') {
            out[o++] = (char)(((in[i+1]-'0') << 6) | ((in[i+2]-'0') << 3) | (in[i+3]-'0'));
            i += 3;
        } else out[o++] = in[i];
    }
    out[o] = 0;
}

static void usb_safe_id(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        out[o++] = (isalnum(c) || c == '-' || c == '_' || c == '.') ? (char)c : '-';
    }
    out[o] = 0;
    if (!o) snprintf(out, cap, "usbdisk");
}

static void usb_cfg_path(char *out, size_t cap) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", g_idx_dir);
    char *sl = strrchr(tmp, '/');
    if (sl) *sl = 0;
    snprintf(out, cap, "%s/usb.conf", tmp);
}

static void usb_cfg_save(void) {
    char p[1300], t[1400];
    usb_cfg_path(p, sizeof p);
    snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "w");
    if (!f) return;
    fprintf(f, "{\"enabled\":%s,\"watch\":%s}\n",
            g_usb_enabled ? "true" : "false", g_usb_watch ? "true" : "false");
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(t, p);
}

static void usb_cfg_load(void) {
    char p[1300];
    usb_cfg_path(p, sizeof p);
    g_usb_enabled = 0;
    g_usb_watch = 1;
    FILE *f = fopen(p, "r");
    if (!f) return;
    char b[1024];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    g_usb_enabled = usb_json_bool(b, "\"enabled\"", 0);
    g_usb_watch = usb_json_bool(b, "\"watch\"", 1);
}

/* 用 /dev/disk/by-uuid 认文件系统，挂载名变化时仍能认出同一设备 */
static void usb_id_for(const char *dev, char *out, size_t cap) {
    const char *base = strrchr(dev, '/');
    base = base ? base + 1 : dev;
    snprintf(out, cap, "%.*s", (int)(cap - 1), base);
    DIR *d = opendir("/dev/disk/by-uuid");
    if (!d) return;
    struct dirent *de;
    char link[600], tgt[600];
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        snprintf(link, sizeof link, "/dev/disk/by-uuid/%s", de->d_name);
        ssize_t l = readlink(link, tgt, sizeof tgt - 1);
        if (l <= 0) continue;
        tgt[l] = 0;
        const char *tb = strrchr(tgt, '/');
        tb = tb ? tb + 1 : tgt;
        if (!strcmp(tb, base)) { snprintf(out, cap, "%.*s", (int)(cap - 1), de->d_name); break; }
    }
    closedir(d);
}

/* 判断 /dev/sdX / mmcblk / nvme 是否真的来自 USB/可移动介质；
 * 无法确认（sysfs 不存在、mapper 等）时保守接受，避免误伤 fnOS 的其他挂载方式。 */
static int usb_dev_external(const char *dev) {
    if (strncmp(dev, "/dev/", 5) != 0) return 1;
    const char *base = dev + 5;
    char disk[64];
    if (snprintf(disk, sizeof disk, "%s", base) >= (int)sizeof disk) return 1;
    char *pp = strrchr(disk, 'p');
    if (pp && pp > disk && pp[1] >= '0' && pp[1] <= '9' &&
        pp[-1] >= '0' && pp[-1] <= '9') {
        *pp = 0;                                  /* mmcblk0p1 / nvme0n1p1 */
    } else {
        char *e = disk + strlen(disk);
        while (e > disk && e[-1] >= '0' && e[-1] <= '9') e--;
        *e = 0;                                   /* sdj1 -> sdj */
    }
    char path[256], buf[16], link[PATH_MAX];
    snprintf(path, sizeof path, "/sys/block/%s/removable", disk);
    FILE *f = fopen(path, "r");
    int has_sys = f != NULL, removable = 0;
    if (f) {
        if (fgets(buf, sizeof buf, f) && buf[0] == '1') removable = 1;
        fclose(f);
    }
    snprintf(path, sizeof path, "/sys/block/%s", disk);
    int usb = realpath(path, link) && strstr(link, "/usb");
    if (usb || removable) return 1;
    if (!has_sys) return 1;                       /* 无法判断时不要误伤 */
    return 0;
}

static int usb_collect(usb_mount *out, int cap) {
    FILE *f = fopen("/proc/self/mounts", "r");
    if (!f) return 0;
    char line[4096];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < cap) {
        char dev[1024], mnt[2048], fst[128];
        if (sscanf(line, "%1023s %2047s %127s", dev, mnt, fst) != 3) continue;
        char m[2048];
        usb_unescape(mnt, m, sizeof m);
        if (strncmp(m, "/vol00/", 7) != 0) continue;
        struct stat st;
        if (stat(m, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) if (!strcmp(out[i].root, m)) dup = 1;
        if (dup) continue;
        snprintf(out[n].root, sizeof out[n].root, "%.*s", (int)(sizeof out[n].root - 1), m);
        char dv[1024];
        usb_unescape(dev, dv, sizeof dv);
        if (!usb_dev_external(dv)) continue;
        snprintf(out[n].dev, sizeof out[n].dev, "%.*s", (int)(sizeof out[n].dev - 1), dv);
        usb_id_for(dv, out[n].id, sizeof out[n].id);
        n++;
    }
    fclose(f);
    return n;
}

static void usb_sig_of(const usb_mount *m, int n, char *out, size_t cap) {
    usb_mount tmp[MAX_USB];
    int k = n < MAX_USB ? n : MAX_USB;
    for (int i = 0; i < k; i++) tmp[i] = m[i];
    for (int i = 0; i < k; i++)
        for (int j = i + 1; j < k; j++)
            if (strcmp(tmp[j].id, tmp[i].id) < 0) { usb_mount t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t; }
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < k; i++) {
        int w = snprintf(out + o, cap - o, "%s|%s;", tmp[i].id, tmp[i].root);
        if (w < 0 || (size_t)w >= cap - o) break;
        o += (size_t)w;
    }
}

static void usb_idx_file(const char *id, char *out, size_t cap) {
    char safe[160], buf[1300];
    usb_safe_id(id, safe, sizeof safe);
    int w = snprintf(buf, sizeof buf, "%s/usb-%s.idx", g_idx_dir, safe);
    if (w < 0) { out[0] = 0; return; }
    if ((size_t)w >= cap) w = (int)cap - 1;
    memcpy(out, buf, (size_t)w);
    out[w] = 0;
}

static int usb_find_locked(const char *id) {
    for (int i = g_nvols_local; i < g_nvols; i++)
        if (g_vols[i].usb && !strcmp(g_vols[i].usb_id, id)) return i;
    return -1;
}

static void usb_remove_locked(int i) {
    if (i < g_nvols_local || i >= g_nvols) return;
    vol_t *v = &g_vols[i];
    if (v->ok) { dir_index_free(v); snapshot_close(&v->sn); v->ok = 0; }
    unlink(v->file);                     /* 旧路径索引一并清理，避免残留 */
    memmove(&g_vols[i], &g_vols[i+1], (size_t)(g_nvols - i - 1) * sizeof(vol_t));
    g_nvols--;
}

static int usb_add_locked(const usb_mount *m) {
    if (g_nvols >= MAX_VOL) return -1;
    vol_t *v = &g_vols[g_nvols];
    memset(v, 0, sizeof *v);
    snprintf(v->root, sizeof v->root, "%.*s", (int)(sizeof v->root - 1), m->root);
    snprintf(v->usb_id, sizeof v->usb_id, "%.*s", (int)(sizeof v->usb_id - 1), m->id);
    usb_idx_file(m->id, v->file, sizeof v->file);
    v->usb = 1;
    /* 每次挂载/重启都重建：外接盘可能在别处改过内容，旧快照不可信 */
    unlink(v->file);
    return g_nvols++;
}

typedef struct { char root[512]; char file[1100]; char id[128]; } usb_job;

static void usb_build_finish(usb_job *j, int ok) {
    pthread_rwlock_wrlock(&g_rw);
    int i = usb_find_locked(j->id);
    if (i >= 0 && !strcmp(g_vols[i].root, j->root)) {
        g_vols[i].building = 0;
        if (!ok) {
            g_vols[i].ok = 0;
            usb_msg_set( "USB 索引构建失败");
        }
    }
    pthread_rwlock_unlock(&g_rw);
    free(j);
}

static void *usb_build_worker(void *arg) {
    usb_job *j = arg;
    xl_store st;
    volatile uint64_t usb_progress = 0;
    scan_set_progress(&usb_progress);
    store_init(&st);
    uint64_t t0 = now_ms();
    int rc = scan_tree(j->root, &st, 1, 0);
    if (rc < 0 && st.n < 2) { store_free(&st); usb_build_finish(j, 0); return NULL; }
    store_shrink(&st);
    xl_index ix;
    index_init(&ix);
    index_build(&ix, &st, 0);
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", j->file);
    int ok = snapshot_save(tmp, &st, &ix, j->root, 0) == 0;
    if (ok && rename(tmp, j->file) != 0) ok = 0;
    if (!ok) unlink(tmp);
    uint64_t ms = now_ms() - t0;
    store_free(&st);
    index_free(&ix);
    if (!ok) { usb_build_finish(j, 0); return NULL; }
    xl_snap ns;
    if (snapshot_open(j->file, &ns) < 0) { usb_build_finish(j, 0); return NULL; }
    int installed = 0;
    pthread_rwlock_wrlock(&g_rw);
    int i = usb_find_locked(j->id);
    if (i >= 0 && !strcmp(g_vols[i].root, j->root)) {
        vol_t *v = &g_vols[i];
        if (v->ok) { dir_index_free(v); snapshot_close(&v->sn); }
        v->sn = ns;
        v->ok = 1;
        v->records = ns.h.rec_count;
        v->built_at = (uint64_t)time(NULL);
        v->build_ms = ms;
        v->building = 0;
        dir_index_build(v);
        installed = 1;
        usb_msg_set( "USB 设备在线（已索引 %llu 条）",
                 (unsigned long long)v->records);
    }
    pthread_rwlock_unlock(&g_rw);
    if (!installed) snapshot_close(&ns);
    free(j);
    return NULL;
}

static void usb_spawn_build_locked(int i) {
    usb_job *j = xmalloc(sizeof *j);
    memset(j, 0, sizeof *j);
    snprintf(j->root, sizeof j->root, "%.*s", (int)(sizeof j->root - 1), g_vols[i].root);
    snprintf(j->file, sizeof j->file, "%.*s", (int)(sizeof j->file - 1), g_vols[i].file);
    snprintf(j->id, sizeof j->id, "%.*s", (int)(sizeof j->id - 1), g_vols[i].usb_id);
    g_vols[i].building = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, usb_build_worker, j) != 0) {
        g_vols[i].building = 0;
        free(j);
    } else pthread_detach(th);
}

static void usb_drop_locked(void) {
    for (int i = g_nvols - 1; i >= g_nvols_local; i--)
        if (g_vols[i].usb) usb_remove_locked(i);
}

/* 对齐当前挂载：新增/移除/路径变化；掉线时按开关自动关闭检索 */
static void usb_sync(void) {
    usb_mount cur[MAX_USB];
    int cn = g_usb_enabled ? usb_collect(cur, MAX_USB) : 0;
    int path_changed = 0, path_ignored = 0, added = 0;
    pthread_rwlock_wrlock(&g_rw);
    for (int i = g_nvols - 1; i >= g_nvols_local; i--) {
        vol_t *v = &g_vols[i];
        if (!v->usb) continue;
        int k = -1;
        for (int j = 0; j < cn; j++) if (!strcmp(cur[j].id, v->usb_id)) { k = j; break; }
        if (k < 0) {
            /* 路径检测关闭时保留旧索引，由用户手动处理；关闭检索时照常清理 */
            if (g_usb_enabled && !g_usb_watch) { path_ignored = 1; continue; }
            usb_remove_locked(i);
            continue;
        }
        if (strcmp(cur[k].root, v->root) != 0) {
            if (g_usb_enabled && !g_usb_watch) { path_ignored = 1; continue; }
            path_changed = 1;
            usb_remove_locked(i);        /* 旧挂载路径索引作废 */
        }
    }
    for (int k = 0; k < cn; k++) {
        int i = usb_find_locked(cur[k].id);
        if (i < 0) {
            i = usb_add_locked(&cur[k]);
            if (i < 0) continue;
            added = 1;
        }
        if (!g_vols[i].ok && !g_vols[i].building) usb_spawn_build_locked(i);
    }
    int any = 0;
    for (int i = g_nvols_local; i < g_nvols; i++) if (g_vols[i].usb) any = 1;
    if (any) g_usb_had = 1;
    if (cn > 0) g_usb_missing_ticks = 0;
    if (g_usb_enabled && cn == 0 && g_usb_watch && g_usb_had) {
        /* 连续两次（约 4 秒）都检测不到才关闭，避免 U 盘重新挂载瞬间被误判 */
        if (++g_usb_missing_ticks >= 2) {
            usb_drop_locked();
            g_usb_enabled = 0;
            g_usb_had = 0;
            g_usb_missing_ticks = 0;
            usb_msg_set(
                     "USB 设备已断开，旧索引已清理，已自动关闭 USB 检索");
            usb_cfg_save();
        } else {
            usb_msg_set(
                     "USB 设备暂时不可用，等待恢复（%d/2）", g_usb_missing_ticks);
        }
    } else if (path_changed) {
        usb_msg_set( "USB 挂载点已变化，正在按新路径重建索引");
    } else if (path_ignored) {
        usb_msg_set(
                 "USB 状态已变化；路径检测已关闭，未自动清理或重建索引");
    } else if (added) {
        usb_msg_set( "已检测到 USB 设备，正在建索引");
    } else if (!g_usb_enabled) {
        usb_msg_set( "USB 检索未开启");
    } else if (cn == 0) {
        usb_msg_set( "未检测到 USB 设备（插上后自动索引）");
    }
    usb_sig_of(cur, cn, g_usb_sig, sizeof g_usb_sig);
    pthread_rwlock_unlock(&g_rw);
}

static void usb_tick(void) {
    uint64_t now = now_ms();
    if (now - g_usb_last_tick < 2000) return;
    g_usb_last_tick = now;
    if (!g_usb_enabled) return;
    usb_mount cur[MAX_USB];
    int cn = usb_collect(cur, MAX_USB);
    char sig[4096];
    usb_sig_of(cur, cn, sig, sizeof sig);
    if (strcmp(sig, g_usb_sig) != 0) { usb_sync(); return; }
    if (g_usb_missing_ticks > 0) { usb_sync(); return; }
    int retry = 0;
    pthread_rwlock_rdlock(&g_rw);
    for (int i = g_nvols_local; i < g_nvols; i++)
        if (g_vols[i].usb && !g_vols[i].ok && !g_vols[i].building) retry = 1;
    pthread_rwlock_unlock(&g_rw);
    if (retry) usb_sync();
}

static void usb_enable(int on) {
    if ((on ? 1 : 0) == g_usb_enabled) return;
    g_usb_enabled = on ? 1 : 0;
    if (!on) {
        pthread_rwlock_wrlock(&g_rw);
        usb_drop_locked();
        g_usb_had = 0;
        g_usb_sig[0] = 0;
        g_usb_missing_ticks = 0;
        usb_msg_set( "USB 检索已关闭，旧索引已清理");
        pthread_rwlock_unlock(&g_rw);
    } else {
        g_usb_had = 0;
        g_usb_sig[0] = 0;
        g_usb_missing_ticks = 0;
        usb_sync();
    }
    usb_cfg_save();
}

/* 隐藏项规则变化后，USB 索引也需要按新规则重建 */
static void usb_rebuild_all(void) {
    if (!g_usb_enabled) return;
    pthread_rwlock_wrlock(&g_rw);
    usb_drop_locked();
    g_usb_sig[0] = 0;
    pthread_rwlock_unlock(&g_rw);
    usb_sync();
}

/* ---------- 查询 ---------- */
static void handle_acl_list(sbuf *out, int uid, int admin, int all);
static void settings_path(char *out, size_t cap);
static int  scope_load(int uid, char paths[][512], int cap);
static int  scope_default(int uid, char paths[][512], int cap);
static int  scope_allows(char paths[][512], int n, const char *p);

typedef struct { uint16_t vol; uint32_t id; } ref_t;

static int cmp_ref(const void *A, const void *B, void *arg) {
    (void)arg;
    const ref_t *a = A, *b = B;
    const xl_rec *ra = &g_vols[a->vol].sn.recs[a->id];
    const xl_rec *rb = &g_vols[b->vol].sn.recs[b->id];
    if (ra->mtime != rb->mtime) return ra->mtime < rb->mtime ? 1 : -1;
    return (int)ra->name_len - (int)rb->name_len;
}

static void url_decode(const char *src, size_t n, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        if (src[i] == '%' && i + 2 < n && isxdigit((unsigned char)src[i + 1]) &&
            isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[o++] = ' ';
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = 0;
}

static int param(const char *qs, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    const char *p = qs;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            url_decode(p + kl + 1, seg - kl - 1, out, cap);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

static int cmp_live(const void *A, const void *B) {
    const xl_live_hit *a = A, *b = B;
    return (a->mtime < b->mtime) - (a->mtime > b->mtime);
}


/* ---- /api/file：预览/下载单个文件 ---- */
static const char *mime_of(const char *path) {
    const char *e = strrchr(path, '.');
    if (!e) return "application/octet-stream";
    static const struct { const char *ext, *mime; } T[] = {
        {".png","image/png"}, {".jpg","image/jpeg"}, {".jpeg","image/jpeg"},
        {".gif","image/gif"}, {".webp","image/webp"}, {".bmp","image/bmp"},
        {".svg","image/svg+xml"}, {".ico","image/x-icon"},
        {".mp4","video/mp4"}, {".webm","video/webm"}, {".mov","video/quicktime"},
        {".mkv","video/x-matroska"}, {".m4v","video/x-m4v"}, {".avi","video/x-msvideo"},
        {".wmv","video/x-ms-wmv"}, {".flv","video/x-flv"}, {".mpg","video/mpeg"},
        {".mpeg","video/mpeg"}, {".ts","video/mp2t"}, {".m2ts","video/mp2t"},
        {".3gp","video/3gpp"}, {".rmvb","video/vnd.rn-realvideo"}, {".ogv","video/ogg"},
        {".mp3","audio/mpeg"}, {".flac","audio/flac"}, {".wav","audio/wav"},
        {".m4a","audio/mp4"}, {".ogg","audio/ogg"}, {".oga","audio/ogg"},
        {".opus","audio/ogg"}, {".aac","audio/aac"}, {".wma","audio/x-ms-wma"},
        {".aiff","audio/aiff"}, {".ape","audio/x-ape"},
        {".pdf","application/pdf"},
        {".txt","text/plain; charset=utf-8"}, {".md","text/plain; charset=utf-8"},
        {".log","text/plain; charset=utf-8"}, {".json","application/json; charset=utf-8"},
        {".csv","text/csv; charset=utf-8"}, {".sh","text/plain; charset=utf-8"},
        {".conf","text/plain; charset=utf-8"}, {".ini","text/plain; charset=utf-8"},
        {".yml","text/plain; charset=utf-8"}, {".yaml","text/plain; charset=utf-8"},
        {".html","text/html; charset=utf-8"}, {".css","text/css; charset=utf-8"},
        {".js","text/javascript; charset=utf-8"},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcasecmp(e, T[i].ext)) return T[i].mime;
    return "application/octet-stream";
}

#define STREAM_CHUNK (256u << 10)

/* 每个工作线程一次请求的附加响应头（下载时写 Content-Disposition） */
static _Thread_local char g_extra_hdr[1400];
static _Thread_local char g_range_hdr[256];
static _Thread_local int g_no_store = 1;
static _Thread_local int g_fs_fd = -1;
static _Thread_local off_t g_fs_off = 0, g_fs_len = 0;
static _Thread_local int g_fs_status = 0;


/* ---------- 媒体探测（ffprobe） ---------- */
static int run_capture(char *const argv[], char *out, size_t cap) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) dup2(nul, 2);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    size_t got = 0;
    uint64_t deadline = now_ms() + 5000;   /* ffprobe 不允许无限挂住 worker */
    struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN, .revents = 0 };
    for (;;) {
        if (got + 1 >= cap) break;
        int64_t remain = (int64_t)deadline - (int64_t)now_ms();
        if (remain <= 0) { kill(pid, SIGKILL); break; }
        int pr = poll(&pfd, 1, (int)remain);
        if (pr < 0) { if (errno == EINTR) continue; kill(pid, SIGKILL); break; }
        if (pr == 0) { kill(pid, SIGKILL); break; }
        ssize_t r = read(pipefd[0], out + got, cap - 1 - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    out[got] = 0;
    close(pipefd[0]);
    int status = 0;
    for (int i = 0; i < 20; i++) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        usleep(50000);
    }
    if (waitpid(pid, &status, WNOHANG) == 0) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int jstr_in(const char *ob, const char *end, const char *key, char *out, size_t cap) {
    if (!out || !cap) return 0;
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = ob; (p = strstr(p, key)) != NULL && p < end; p += kl) {
        const char *q = p + kl;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q >= end || *q != ':') continue;
        q++;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q >= end || *q != '"') continue;
        const char *a = q + 1, *b = memchr(a, '"', (size_t)(end - a));
        if (!b) return 0;
        size_t n = (size_t)(b - a);
        if (n >= cap) n = cap - 1;
        memcpy(out, a, n); out[n] = 0;
        return 1;
    }
    return 0;
}

static int jint_in(const char *ob, const char *end, const char *key, int *out) {
    size_t kl = strlen(key);
    for (const char *p = ob; (p = strstr(p, key)) != NULL && p < end; p += kl) {
        const char *q = p + kl;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q >= end || *q != ':') continue;
        q++;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        char *ep = NULL;
        long v = strtol(q, &ep, 10);
        if (ep != q && ep <= end) { *out = (int)v; return 1; }
    }
    return 0;
}

/* 单次 ffprobe 取编码/分辨率/像素格式/传输特性，避免预览时启动两个进程 */
static int media_probe(const char *real, char *vcodec, size_t vcap, char *acodec, size_t acap,
                       int *w, int *h, char *pix, size_t pcap, char *transfer, size_t tcap) {
    static const char *paths[] = { "/usr/bin/ffprobe", "/bin/ffprobe", "/usr/local/bin/ffprobe" };
    char buf[8192];
    if (vcodec && vcap) vcodec[0] = 0;
    if (acodec && acap) acodec[0] = 0;
    if (pix && pcap) pix[0] = 0;
    if (transfer && tcap) transfer[0] = 0;
    *w = *h = 0;
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        if (access(paths[i], X_OK) != 0) continue;
        char *argv[] = { (char *)paths[i], "-v", "error",
                         "-show_entries", "stream=codec_type,codec_name,width,height,pix_fmt,color_transfer",
                         "-of", "json", (char *)real, NULL };
        if (run_capture(argv, buf, sizeof buf) != 0) return 0;
        int have_v = 0, have_a = 0;
        const char *p = buf;
        while ((p = strstr(p, "\"codec_type\"")) != NULL) {
            const char *ob = p;
            while (ob > buf && *ob != '{') ob--;
            const char *end = strchr(p, '}');
            if (!end) break;
            char type[16] = "";
            jstr_in(ob, end, "\"codec_type\"", type, sizeof type);
            if (!strcmp(type, "video") && !have_v) {
                have_v = 1;
                jstr_in(ob, end, "\"codec_name\"", vcodec, vcap);
                jstr_in(ob, end, "\"pix_fmt\"", pix, pcap);
                jstr_in(ob, end, "\"color_transfer\"", transfer, tcap);
                jint_in(ob, end, "\"width\"", w);
                jint_in(ob, end, "\"height\"", h);
            } else if (!strcmp(type, "audio") && !have_a) {
                have_a = 1;
                jstr_in(ob, end, "\"codec_name\"", acodec, acap);
            }
            p = end + 1;
        }
        return 1;
    }
    return 0;
}

static void handle_probe(sbuf *out, const char *qs, int uid) {
    char raw[4096];
    if (!param(qs, "path", raw, sizeof raw)) { sb_puts(out, "{\"ok\":false,\"error\":\"missing path\"}"); return; }
    if (strncmp(raw, "/vol", 4) || strstr(raw, "..")) { sb_puts(out, "{\"ok\":false,\"error\":\"path not allowed\"}"); return; }
    char real[PATH_MAX];
    if (!realpath(raw, real) || strncmp(real, "/vol", 4)) { sb_puts(out, "{\"ok\":false,\"error\":\"not found\"}"); return; }
    if (uid >= 0) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", real);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = 0;
            if (!perm_dir_listable((uid_t)uid, dir)) { sb_puts(out, "{\"ok\":false,\"error\":\"forbidden\"}"); return; }
        }
        if (!perm_file_readable((uid_t)uid, real)) { sb_puts(out, "{\"ok\":false,\"error\":\"forbidden\"}"); return; }
    }
    struct stat pst;
    if (stat(real, &pst) != 0 || !S_ISREG(pst.st_mode)) { sb_puts(out, "{\"ok\":false,\"error\":\"not a regular file\"}"); return; }
    char vc[32] = "", ac[32] = "", pix[40] = "", tr[40] = "";
    int vw = 0, vh = 0;
    int ok = media_probe(real, vc, sizeof vc, ac, sizeof ac, &vw, &vh, pix, sizeof pix, tr, sizeof tr);
    int is_hdr = !strcmp(tr, "smpte2084") || !strcmp(tr, "arib-std-b67");
    sb_printf(out, "{\"ok\":%s,\"video\":\"%s\",\"audio\":\"%s\",\"width\":%d,\"height\":%d,\"pix\":\"%s\",\"hdr\":%s}",
              ok ? "true" : "false", vc, ac, vw, vh, pix, is_hdr ? "true" : "false");
}


/* ---------- 纯文本阅读页（自带深色样式，避免浏览器自动反色/低对比） ---------- */
static int utf8_valid(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }
        size_t need;
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return 0;
        if (i + need >= n) return 0;
        for (size_t k = 1; k <= need; k++) if ((s[i + k] & 0xC0) != 0x80) return 0;
        i += need + 1;
    }
    return 1;
}

static void html_escape_to(sbuf *out, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '&') sb_puts(out, "&amp;");
        else if (c == '<') sb_puts(out, "&lt;");
        else if (c == '>') sb_puts(out, "&gt;");
        else sb_add(out, &c, 1);
    }
}

/* 读取文本文件（UTF-8 优先，非法则按 GB18030 转换）；失败时输出 JSON 错误并返回 NULL */
static char *read_text_file(const char *qs, int uid, size_t *len_out, int *trunc_out) {
    static const size_t CAP = 8u << 20;
    char raw[4096];
    if (!param(qs, "path", raw, sizeof raw)) { *len_out = (size_t)-1; return NULL; }
    if (strncmp(raw, "/vol", 4) || strstr(raw, "..")) { *len_out = (size_t)-1; return NULL; }
    char real[PATH_MAX];
    if (!realpath(raw, real) || strncmp(real, "/vol", 4)) { *len_out = (size_t)-1; return NULL; }
    if (usb_path_blocked(uid, real)) { *len_out = (size_t)-1; return NULL; }
    if (uid >= 0) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", real);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = 0;
            if (!perm_dir_listable((uid_t)uid, dir)) { *len_out = (size_t)-1; return NULL; }
        }
        if (!perm_file_readable((uid_t)uid, real)) { *len_out = (size_t)-1; return NULL; }
    }
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISREG(st.st_mode)) { *len_out = (size_t)-1; return NULL; }
    int fd = open(real, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { *len_out = (size_t)-1; return NULL; }
    { struct stat fst; if (fstat(fd, &fst) != 0 || !S_ISREG(fst.st_mode)) { close(fd); *len_out = (size_t)-1; return NULL; } }
    size_t want = (size_t)st.st_size < CAP ? (size_t)st.st_size : CAP;
    char *buf = malloc(want + 1);
    if (!buf) { close(fd); *len_out = (size_t)-1; return NULL; }
    size_t got = 0;
    while (got < want) {
        ssize_t r = read(fd, buf + got, want - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = 0;
    *trunc_out = (size_t)st.st_size > got;

    if (!utf8_valid((const unsigned char *)buf, got)) {
        iconv_t cd = iconv_open("UTF-8", "GB18030");
        if (cd != (iconv_t)-1) {
            size_t outcap = got * 2 + 64;
            char *conv = malloc(outcap);
            if (conv) {
                char *inp = buf, *outp = conv;
                size_t inl = got, outl = outcap - 1;
                iconv(cd, &inp, &inl, &outp, &outl);
                *outp = 0;
                free(buf);
                iconv_close(cd);
                *len_out = strlen(conv);
                return conv;
            }
            iconv_close(cd);
        }
    }
    *len_out = got;
    return buf;
}

static void sb_js_string(sbuf *out, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { sb_puts(out, "\\"); sb_add(out, (const char *)&c, 1); }
        else if (c == '\n') sb_puts(out, "\\n");
        else if (c == '<') sb_puts(out, "\\x3c");   /* 防止文件名闭合 <script> */
        else if (c < 0x20) sb_puts(out, " ");
        else sb_add(out, (const char *)&c, 1);
    }
}

static void handle_text(sbuf *out, const char *qs, int uid, const char **ctype) {
    size_t tlen = 0;
    int truncated = 0;
    char *text = read_text_file(qs, uid, &tlen, &truncated);
    if (!text) {
        if (tlen == (size_t)-1) {
            char raw[4096];
            if (!param(qs, "path", raw, sizeof raw)) sb_puts(out, "{\"error\":\"missing path\"}");
            else if (strncmp(raw, "/vol", 4) || strstr(raw, "..")) sb_puts(out, "{\"error\":\"path not allowed\"}");
            else sb_puts(out, "{\"error\":\"not found or forbidden\"}");
            *ctype = "application/json; charset=utf-8";
            return;
        }
        sb_puts(out, "{\"error\":\"not a file\"}");
        *ctype = "application/json; charset=utf-8";
        return;
    }
    char pbuf[4096];
    param(qs, "path", pbuf, sizeof pbuf);

    char th[16];
    param(qs, "theme", th, sizeof th);
    int light = !strcmp(th, "light");
    const char *c_bg   = light ? "#f7f9fc" : "#0d1524";
    const char *c_fg   = light ? "#1b2740" : "#dbe6f6";
    const char *c_bar  = light ? "#eef2f8" : "#111c2e";
    const char *c_line = light ? "#dbe2ee" : "#21314a";
    const char *c_dim  = light ? "#5a6880" : "#8fa5c4";
    const char *c_head = light ? "#101a2c" : "#eaf1fb";
    const char *c_h4   = light ? "#0b6b60" : "#7fe6d6";
    const char *c_link = light ? "#0b6fb8" : "#7fd7ff";
    const char *c_quo  = light ? "#4b5a72" : "#a9bad6";
    const char *c_qbg  = light ? "rgba(15,23,42,.035)" : "rgba(255,255,255,.02)";
    const char *c_code = light ? "#eef2f8" : "#111c2e";
    const char *c_btnb = light ? "rgba(15,156,140,.14)" : "rgba(20,50,54,.92)";
    const char *c_btnf = light ? "#0b4a44" : "#9ff0e4";
    sb_printf(out,
        "<!doctype html><meta charset=\"utf-8\"><meta name=\"color-scheme\" content=\"%s\">"
        "<title></title><style>"
        "html,body{margin:0;background:%s;color:%s;overscroll-behavior:contain}"
        "pre{margin:0;padding:18px 20px;font:13px/1.7 ui-monospace,Menlo,Consolas,monospace;"
        "white-space:pre-wrap;word-break:break-word;tab-size:4}"
        ".bar{position:sticky;top:0;background:%s;border-bottom:1px solid %s;"
        "padding:6px 14px;font:12px/1.6 -apple-system,sans-serif;color:%s}"
        "#dbtn{position:fixed;right:16px;bottom:16px;display:none;cursor:pointer;"
        "border:1px solid rgba(94,234,212,.45);background:%s;color:%s;"
        "border-radius:999px;padding:8px 16px;font:13px/1 -apple-system,sans-serif;"
        "box-shadow:0 6px 20px rgba(0,0,0,.18)}"
        "#dbtn:hover{filter:brightness(1.08)}"
        "article{padding:24px 28px 60px;max-width:820px;margin:0 auto;"
        "font:15px/1.85 -apple-system,\"PingFang SC\",\"Microsoft YaHei\",sans-serif;color:%s}"
        "article h1,article h2,article h3,article h4,article h5,article h6{line-height:1.4;margin:1.4em 0 .6em;color:%s}"
        "article h1{font-size:24px}article h2{font-size:21px}article h3{font-size:18px}"
        "article h4{font-size:16px;color:%s}article h5,article h6{font-size:15px}"
        "article p{margin:0 0 14px}article ul,article ol{margin:0 0 14px;padding-left:1.5em}"
        "article li{margin:4px 0}article blockquote{margin:0 0 14px;padding:8px 14px;"
        "border-left:3px solid %s;color:%s;background:%s}"
        "article img{max-width:100%;height:auto;border-radius:8px;margin:8px 0}"
        "article a{color:%s}article hr{border:0;border-top:1px solid %s;margin:22px 0}"
        "article table{border-collapse:collapse;margin:0 0 14px;width:100%%}"
        "article td,article th{border:1px solid %s;padding:6px 10px;font-size:14px}"
        "article pre,article code{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:13px}"
        "article pre{background:%s;padding:12px 14px;border-radius:8px;overflow:auto}"
        "</style>",
        light ? "light" : "dark", c_bg, c_fg, c_bar, c_line, c_dim,
        c_btnb, c_btnf, c_fg, c_head, c_h4, c_line, c_quo, c_qbg, c_link, c_line, c_line, c_code);
    if (truncated) sb_puts(out, "<div class=\"bar\">文件较大，仅显示前 8 MB</div>");
    sb_puts(out, "<pre id=\"src\">");
    html_escape_to(out, text, tlen);
    sb_puts(out, "</pre><article id=\"doc\" style=\"display:none\"></article>"
                 "<button id=\"dbtn\">按文档查看</button><script>const P=\"");
    sb_js_string(out, pbuf);
    sb_puts(out,
        "\";const btn=document.getElementById('dbtn'),src=document.getElementById('src'),doc=document.getElementById('doc');"
        "let on=false;"
        "function san(html){"
        "const ok=new Set(['P','DIV','H1','H2','H3','H4','H5','H6','UL','OL','LI','STRONG','B','EM','I','U','S','BLOCKQUOTE','PRE','CODE','BR','HR','TABLE','THEAD','TBODY','TR','TD','TH','IMG','A','FIGURE','FIGCAPTION','SPAN','SECTION','ARTICLE']);"
        "const d=new DOMParser().parseFromString('<body>'+html+'</body>','text/html');"
        "const bad=new Set(['SCRIPT','STYLE','IFRAME','OBJECT','EMBED','LINK','META','BASE','FORM','INPUT','BUTTON','TEXTAREA','SELECT','VIDEO','AUDIO','SVG']);"
        "const walk=n=>{[...n.childNodes].forEach(c=>{"
        "if(c.nodeType===8){c.remove();return;}"
        "if(c.nodeType!==1)return;"
        "const t=c.tagName;"
        "if(bad.has(t)){c.remove();return;}"
        "if(!ok.has(t)){const f=document.createDocumentFragment();while(c.firstChild)f.appendChild(c.firstChild);c.replaceWith(f);return;}"
        "[...c.attributes].forEach(a=>{const n=a.name.toLowerCase();"
        "if(n.startsWith('on')){c.removeAttribute(a.name);return;}"
        "if(t==='IMG'&&n==='src'){if(!/^(https?:|data:image\\/)/i.test(a.value))c.removeAttribute('src');return;}"
        "if(t==='A'&&n==='href'){if(!/^(https?:|mailto:)/i.test(a.value))c.removeAttribute('href');return;}"
        "if(n==='alt'||n==='title'||n==='colspan'||n==='rowspan'||n==='align')return;"
        "c.removeAttribute(a.name);});"
        "if(t==='A'){c.setAttribute('target','_blank');c.setAttribute('rel','noopener noreferrer');}"
        "walk(c);});};"
        "walk(d.body);return d.body.innerHTML;}"
        "fetch(new URL('raw?path='+encodeURIComponent(P),location.href))"
        ".then(r=>r.ok?r.text():Promise.reject(r.status)).then(t=>{"
        "let h=null;const s=t.trim();"
        "if(s.charAt(0)==='{'){try{const o=JSON.parse(s);h=o&&(o.content||o.html||o.body);}catch(e){}}"
        "if(!h&&/\\.html?$/i.test(P))h=t;"
        "if(typeof h==='string'&&/<[a-z][^>]*>/i.test(h)){doc.innerHTML=san(h);btn.style.display='block';}"
        "}).catch(()=>{});"
        "btn.onclick=()=>{on=!on;src.style.display=on?'none':'block';doc.style.display=on?'block':'none';"
        "btn.textContent=on?'查看源码':'按文档查看';};"
        "</script>");
    free(text);
    *ctype = "text/html; charset=utf-8";
}

static void handle_markdown(sbuf *out, const char *qs, int uid, const char **ctype) {
    size_t tlen = 0;
    int truncated = 0;
    char *text = read_text_file(qs, uid, &tlen, &truncated);
    if (!text) {
        if (tlen == (size_t)-1) {
            char raw[4096];
            if (!param(qs, "path", raw, sizeof raw)) sb_puts(out, "{\"error\":\"missing path\"}");
            else if (strncmp(raw, "/vol", 4) || strstr(raw, "..")) sb_puts(out, "{\"error\":\"path not allowed\"}");
            else sb_puts(out, "{\"error\":\"not found or forbidden\"}");
        } else {
            sb_puts(out, "{\"error\":\"not a file\"}");
        }
        *ctype = "application/json; charset=utf-8";
        return;
    }

    size_t hlen = 0;
    char *html = md_render_html(text, tlen, &hlen);
    if (!html) {
        free(text);
        sb_puts(out, "{\"error\":\"render failed\"}");
        *ctype = "application/json; charset=utf-8";
        return;
    }

    char fmt[16] = {0};
    param(qs, "format", fmt, sizeof fmt);
    if (!strcmp(fmt, "json")) {
        sb_puts(out, "{\"ok\":true,\"html\":\"");
        sb_json(out, html, hlen);
        sb_printf(out, "\",\"truncated\":%s}", truncated ? "true" : "false");
        *ctype = "application/json; charset=utf-8";
        free(html);
        free(text);
        return;
    }

    char pbuf[4096] = {0};
    param(qs, "path", pbuf, sizeof pbuf);
    const char *base = strrchr(pbuf, '/');
    base = base ? base + 1 : pbuf;

    char th[16] = {0};
    param(qs, "theme", th, sizeof th);
    int light = !strcmp(th, "light");
    const char *bg   = light ? "#f7f9fc" : "#0d1524";
    const char *fg   = light ? "#1b2740" : "#dbe6f6";
    const char *dim  = light ? "#5a6880" : "#8fa5c4";
    const char *line = light ? "#dbe2ee" : "#21314a";
    const char *head = light ? "#101a2c" : "#eaf1fb";
    const char *acc  = light ? "#0b6b96" : "#7fd7ff";
    const char *code = light ? "#edf2f8" : "#111c2e";
    const char *quote= light ? "#4b5a72" : "#a9bad6";

    sb_printf(out,
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta name=\"color-scheme\" content=\"%s\">"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'; img-src http: https:; base-uri 'none'; form-action 'none'\">"
        "<title>",
        light ? "light" : "dark");
    html_escape_to(out, base, strlen(base));
    sb_puts(out, "</title><style>");
    sb_printf(out,
        ":root{--bg:%s;--fg:%s;--dim:%s;--line:%s;--head:%s;--accent:%s;--code:%s;--quote:%s}",
        bg, fg, dim, line, head, acc, code, quote);
    sb_puts(out,
        "*{box-sizing:border-box}"
        "html{background:var(--bg)}"
        "body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.78 -apple-system,BlinkMacSystemFont,\"Segoe UI\",\"PingFang SC\",\"Microsoft YaHei\",sans-serif;overflow-wrap:break-word}"
        ".md{max-width:860px;margin:0 auto;padding:34px 30px 72px}"
        ".md>:first-child{margin-top:0}"
        "h1,h2,h3,h4,h5,h6{line-height:1.35;color:var(--head);margin:1.55em 0 .62em;font-weight:650}"
        "h1{font-size:28px;border-bottom:1px solid var(--line);padding-bottom:.3em}"
        "h2{font-size:22px;border-bottom:1px solid var(--line);padding-bottom:.22em}"
        "h3{font-size:19px}h4{font-size:17px}h5,h6{font-size:16px}"
        "p,ul,ol,blockquote,pre,table{margin:0 0 16px}"
        "ul,ol{padding-left:1.45em}li{margin:5px 0}"
        "a{color:var(--accent);text-decoration:none}a:hover{text-decoration:underline}"
        "strong{color:var(--head)}del{color:var(--dim)}"
        "code{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:.92em;background:var(--code);padding:2px 5px;border-radius:5px}"
        "pre{background:var(--code);padding:14px 16px;border-radius:10px;overflow:auto;line-height:1.6}"
        "pre code{display:block;background:none;padding:0;font-size:13px;white-space:pre}"
        "blockquote{margin-left:0;padding:8px 16px;border-left:3px solid var(--accent);color:var(--quote);background:rgba(127,215,255,.055)}"
        "hr{border:0;border-top:1px solid var(--line);margin:26px 0}"
        "img{max-width:100%;height:auto;border-radius:8px;margin:8px 0}"
        "table{border-collapse:collapse;width:100%;display:block;overflow-x:auto}"
        "th,td{border:1px solid var(--line);padding:7px 10px;text-align:left;font-size:14px}"
        "th{background:var(--code);color:var(--head)}"
        ".note{padding:8px 14px;border-bottom:1px solid var(--line);color:var(--dim);font-size:13px}"
        "@media(max-width:620px){.md{padding:22px 18px 56px}h1{font-size:24px}h2{font-size:20px}body{font-size:15px}}"
        "</style></head><body>");
    if (truncated) sb_puts(out, "<div class=\"note\">文件较大，仅渲染前 8 MB</div>");
    sb_puts(out, "<article class=\"md\">");
    sb_add(out, html, hlen);
    sb_puts(out, "</article></body></html>");

    free(html);
    free(text);
    *ctype = "text/html; charset=utf-8";
}

static void handle_raw(sbuf *out, const char *qs, int uid, const char **ctype) {
    size_t tlen = 0;
    int truncated = 0;
    char *text = read_text_file(qs, uid, &tlen, &truncated);
    *ctype = "text/plain; charset=utf-8";
    if (!text) {
        if (tlen != (size_t)-1) free(text);
        sb_puts(out, "");
        return;
    }
    sb_add(out, text, tlen);
    free(text);
}

static int header_str(const char *req, const char *key, char *out, size_t cap);

static void handle_file(sbuf *out, const char *req, const char *qs, int uid, int *code, const char **ctype) {
    char raw[4096];
    *code = 400;
    *ctype = "application/json; charset=utf-8";
    if (!param(qs, "path", raw, sizeof raw)) {
        sb_puts(out, "{\"error\":\"missing path\"}");
        return;
    }
    if (strncmp(raw, "/vol", 4) || strstr(raw, "..")) {
        *code = 403;
        sb_puts(out, "{\"error\":\"path not allowed\"}");
        return;
    }
    char real[PATH_MAX];
    if (!realpath(raw, real)) {
        *code = 404;
        sb_puts(out, "{\"error\":\"not found\"}");
        return;
    }
    if (strncmp(real, "/vol", 4) || !strstr(real, "/") || usb_path_blocked(uid, real)) {
        *code = 403;
        sb_puts(out, "{\"error\":\"path not allowed\"}");
        return;
    }
    if (uid >= 0) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", real);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = 0;
            if (!perm_dir_listable((uid_t)uid, dir)) {
                *code = 403;
                sb_puts(out, "{\"error\":\"forbidden\"}");
                return;
            }
        }
        if (!perm_file_readable((uid_t)uid, real)) {
            *code = 403;
            sb_puts(out, "{\"error\":\"forbidden\"}");
            return;
        }
    }
    int fd = open(real, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        *code = 404;
        sb_puts(out, "{\"error\":\"not found\"}");
        return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        *code = 403;
        sb_puts(out, "{\"error\":\"not a regular file\"}");
        return;
    }
    /* 大文件走流式 + Range（视频拖动进度条必需），不再整体读入内存 */
    off_t size = (off_t)st.st_size, off = 0, len = size;
    int scode = 200;
    char range[128];
    if (header_str(req, "range:", range, sizeof range)) {
        if (!strncmp(range, "bytes=", 6)) {
            const char *p = range + 6, *dash = strchr(p, '-');
            if (dash) {
                off_t a = 0, b = size - 1;
                int has_a = dash > p, has_b = dash[1] != 0;
                if (has_a) a = (off_t)strtoll(p, NULL, 10);
                if (has_b) b = (off_t)strtoll(dash + 1, NULL, 10);
                if (!has_a && has_b) { a = size - b; b = size - 1; }
                if (a < 0) a = 0;
                if (b >= size) b = size - 1;
                if (a <= b) {
                    off = a; len = b - a + 1; scode = 206;
                    snprintf(g_range_hdr, sizeof g_range_hdr,
                             "Content-Range: bytes %lld-%lld/%lld\r\nAccept-Ranges: bytes\r\n",
                             (long long)off, (long long)(off + len - 1), (long long)size);
                } else {
                    close(fd);
                    *code = 416;
                    snprintf(g_range_hdr, sizeof g_range_hdr, "Content-Range: bytes */%lld\r\n", (long long)size);
                    sb_puts(out, "{\"error\":\"range not satisfiable\"}");
                    return;
                }
            }
        }
    }
    if (!g_range_hdr[0])
        snprintf(g_range_hdr, sizeof g_range_hdr, "Accept-Ranges: bytes\r\n");
    g_fs_fd = fd; g_fs_off = off; g_fs_len = len; g_fs_status = scode;
    *code = scode;
    *ctype = mime_of(real);

    char dl[8];
    if (param(qs, "dl", dl, sizeof dl)) {
        const char *base = strrchr(real, '/');
        base = base ? base + 1 : real;
        char enc[1024];
        size_t o = 0;
        for (const unsigned char *p = (const unsigned char *)base; *p && o + 3 < sizeof enc; p++) {
            if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
                enc[o++] = (char)*p;
            } else {
                static const char hx[] = "0123456789ABCDEF";
                enc[o++] = '%';
                enc[o++] = hx[*p >> 4];
                enc[o++] = hx[*p & 15];
            }
        }
        enc[o] = 0;
        /* 老浏览器不看 filename* 时的回退：保留扩展名，主体用 download */
        char plain[256];
        const char *dot = strrchr(base, '.');
        int ext_ok = dot && dot != base && strlen(dot) <= 12;
        if (ext_ok) {
            for (const char *q = dot; *q; q++)
                if ((unsigned char)*q >= 0x80 || *q == '"') { ext_ok = 0; break; }
        }
        snprintf(plain, sizeof plain, "download%s", ext_ok ? dot : "");
        snprintf(g_extra_hdr, sizeof g_extra_hdr,
                 "Content-Disposition: attachment; filename=\"%s\"; filename*=UTF-8''%s\r\n",
                 plain, enc);
        *ctype = "application/octet-stream";
    }
}


/* 应用系统目录对普通用户隐藏（它们 POSIX 755，ACL 不拦，但不是用户数据） */
static int sysnoise_name(const char *name, uint16_t len) {
    static const char *const noise[] = {
        "thumb", "lost+found", "trim.media", "mediasrv.transcode", "appcenter-downloads", "vm"
    };
    for (size_t i = 0; i < sizeof noise / sizeof noise[0]; i++) {
        size_t n = strlen(noise[i]);
        if (n == len && memcmp(name, noise[i], n) == 0) return 1;
    }
    return 0;
}

static int system_path(const char *path) {
    static const char *const sysdirs[] = {
        "/@appmeta/", "/@sysappmeta/", "/@appcenter/", "/@apphome/",
        "/@appdata/", "/@appconf/", "/@apptemp/"
    };
    const char *p = strchr(path + 1, '/');
    if (!p) return 0;
    for (size_t i = 0; i < sizeof sysdirs / sizeof sysdirs[0]; i++) {
        size_t n = strlen(sysdirs[i]) - 1;          /* 不含结尾斜杠 */
        if (strncmp(p, sysdirs[i], n) == 0 && (p[n] == '/' || p[n] == 0)) return 1;
    }
    return 0;
}


/* ---------- 结果分类 ---------- */
#define CAT_PDF   0x001
#define CAT_DOC   0x002
#define CAT_XLS   0x004
#define CAT_PPT   0x008
#define CAT_TXT   0x010
#define CAT_CODE  0x020
#define CAT_IMG   0x040
#define CAT_VID   0x080
#define CAT_AUD   0x100
#define CAT_ARC   0x200
#define CAT_OTHER 0x400

static int cat_of_name(const char *name, size_t len) {
    const char *dot = memrchr(name, '.', len);
    if (!dot || dot == name) return CAT_OTHER;
    size_t el = len - (size_t)(dot - name) - 1;
    if (el == 0 || el >= 12) return CAT_OTHER;
    char e[12];
    for (size_t i = 0; i < el; i++) {
        char c = dot[1 + i];
        e[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    e[el] = 0;
    static const struct { const char *ext; int cat; } tab[] = {
        {"pdf",CAT_PDF},
        {"doc",CAT_DOC},{"docx",CAT_DOC},{"odt",CAT_DOC},{"rtf",CAT_DOC},{"wps",CAT_DOC},
        {"xls",CAT_XLS},{"xlsx",CAT_XLS},{"xlsm",CAT_XLS},{"csv",CAT_XLS},{"ods",CAT_XLS},{"et",CAT_XLS},
        {"ppt",CAT_PPT},{"pptx",CAT_PPT},{"odp",CAT_PPT},{"dps",CAT_PPT},
        {"txt",CAT_TXT},{"md",CAT_TXT},{"markdown",CAT_TXT},{"log",CAT_TXT},
        {"ini",CAT_TXT},{"conf",CAT_TXT},{"cfg",CAT_TXT},{"toml",CAT_TXT},
        {"c",CAT_CODE},{"h",CAT_CODE},{"cc",CAT_CODE},{"cpp",CAT_CODE},{"hpp",CAT_CODE},
        {"py",CAT_CODE},{"js",CAT_CODE},{"mjs",CAT_CODE},{"tsx",CAT_CODE},
        {"jsx",CAT_CODE},{"go",CAT_CODE},{"rs",CAT_CODE},{"java",CAT_CODE},{"kt",CAT_CODE},
        {"sh",CAT_CODE},{"bash",CAT_CODE},{"zsh",CAT_CODE},{"ps1",CAT_CODE},
        {"html",CAT_CODE},{"css",CAT_CODE},{"scss",CAT_CODE},{"vue",CAT_CODE},
        {"sql",CAT_CODE},{"json",CAT_CODE},{"yaml",CAT_CODE},{"yml",CAT_CODE},{"xml",CAT_CODE},
        {"jpg",CAT_IMG},{"jpeg",CAT_IMG},{"png",CAT_IMG},{"gif",CAT_IMG},{"bmp",CAT_IMG},
        {"webp",CAT_IMG},{"svg",CAT_IMG},{"heic",CAT_IMG},{"tif",CAT_IMG},{"tiff",CAT_IMG},
        {"mp4",CAT_VID},{"mkv",CAT_VID},{"avi",CAT_VID},{"mov",CAT_VID},{"wmv",CAT_VID},
        {"flv",CAT_VID},{"webm",CAT_VID},{"m4v",CAT_VID},{"mpg",CAT_VID},{"mpeg",CAT_VID},{"ts",CAT_VID},
        {"mp3",CAT_AUD},{"flac",CAT_AUD},{"wav",CAT_AUD},{"aac",CAT_AUD},{"m4a",CAT_AUD},
        {"ogg",CAT_AUD},{"ape",CAT_AUD},{"wma",CAT_AUD},{"opus",CAT_AUD},
        {"zip",CAT_ARC},{"rar",CAT_ARC},{"7z",CAT_ARC},{"tar",CAT_ARC},{"gz",CAT_ARC},
        {"bz2",CAT_ARC},{"xz",CAT_ARC},{"zst",CAT_ARC},{"iso",CAT_ARC},
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strcmp(e, tab[i].ext) == 0) return tab[i].cat;
    return CAT_OTHER;
}


static const char *cat_key(int c) {
    switch (c) {
    case CAT_PDF: return "pdf";
    case CAT_DOC: return "doc";
    case CAT_XLS: return "xls";
    case CAT_PPT: return "ppt";
    case CAT_TXT: return "txt";
    case CAT_CODE: return "code";
    case CAT_IMG: return "img";
    case CAT_VID: return "vid";
    case CAT_AUD: return "aud";
    case CAT_ARC: return "arc";
    default: return "other";
    }
}

static int cat_parse(const char *v) {
    if (!v || !*v) return 0;
    if (!strcmp(v, "pdf")) return CAT_PDF;
    if (!strcmp(v, "doc")) return CAT_DOC;
    if (!strcmp(v, "xls")) return CAT_XLS;
    if (!strcmp(v, "ppt")) return CAT_PPT;
    if (!strcmp(v, "txt")) return CAT_TXT;
    if (!strcmp(v, "code")) return CAT_CODE;
    if (!strcmp(v, "img")) return CAT_IMG;
    if (!strcmp(v, "vid")) return CAT_VID;
    if (!strcmp(v, "aud")) return CAT_AUD;
    if (!strcmp(v, "arc")) return CAT_ARC;
    if (!strcmp(v, "other")) return CAT_OTHER;
    return 0;
}

/* 搜索结果里大量文件共享同一父目录；按目录缓存可避免逐条走 ACL 祖先链。 */
#define PCACHE_N 4096
typedef struct { char *dir; int ok; } perm_cache_ent;
static uint32_t perm_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}
static int perm_cache_check(perm_cache_ent *c, const char *dir, uid_t uid) {
    perm_cache_ent *e = &c[perm_hash(dir) & (PCACHE_N - 1)];
    if (e->dir && !strcmp(e->dir, dir)) return e->ok;
    int ok = perm_dir_listable(uid, dir);
    char *cp = strdup(dir);
    if (cp) { free(e->dir); e->dir = cp; e->ok = ok; }
    return ok;
}
static void perm_cache_free(perm_cache_ent *c) {
    for (int i = 0; i < PCACHE_N; i++) free(c[i].dir);
}

/* 搜索限流：每 uid 每秒 20 次，其中 2 字符查询每秒 5 次，避免拖慢 4 个 worker */
#define RL_MAX 64
static int rate_allow(int uid, int is_short) {
    static struct { int key; uint64_t win; int total, shortn; } t[RL_MAX];
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static unsigned rr;
    uint64_t win = now_ms() / 1000;
    int key = uid + 1, slot = -1, ok = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < RL_MAX; i++) if (t[i].key == key) { slot = i; break; }
    if (slot < 0) {
        slot = (int)(rr++ % RL_MAX);
        t[slot].key = key;
        t[slot].win = 0;
        t[slot].total = t[slot].shortn = 0;
    }
    if (t[slot].win != win) {
        t[slot].win = win;
        t[slot].total = t[slot].shortn = 0;
    }
    t[slot].total++;
    if (is_short) t[slot].shortn++;
    ok = t[slot].total <= 20 && (!is_short || t[slot].shortn <= 5);
    pthread_mutex_unlock(&mu);
    return ok;
}

static void handle_search(sbuf *out, const char *qs, int uid, int admin) {
    char q[512] = "", s_limit[32] = "", s_off[32] = "", type[16] = "", s_cat[24] = "";
    param(qs, "q", q, sizeof(q));
    param(qs, "limit", s_limit, sizeof(s_limit));
    param(qs, "offset", s_off, sizeof(s_off));
    param(qs, "type", type, sizeof(type));
    param(qs, "cat", s_cat, sizeof(s_cat));
    int catmask = cat_parse(s_cat);
    if (!q[0]) {
        sb_puts(out, "{\"error\":\"missing q\"}");
        return;
    }
    /* 单字符查询会退化成全量 names 池扫描（实测 2.6 秒），直接拒绝。 */
    {
        int chars = 0;
        for (const unsigned char *p = (const unsigned char *)q; *p; p++)
            if ((*p & 0xC0) != 0x80) chars++;
        if (chars < 2) {
            sb_puts(out, "{\"error\":\"query too short\"}");
            return;
        }
        if (!rate_allow(uid, chars == 2)) {
            sb_puts(out, "{\"error\":\"rate limited\"}");
            return;
        }
    }
    uint32_t limit = s_limit[0] ? (uint32_t)atoi(s_limit) : 50;
    uint32_t off = s_off[0] ? (uint32_t)atoi(s_off) : 0;
    if (limit == 0 || limit > 500) limit = 50;

    xl_query_opts o;
    memset(&o, 0, sizeof(o));
    /* 只取当前页所需的候选（含权限过滤余量），不再物化全部命中。 */
    {
        uint64_t want = (uint64_t)(limit + off) * 20;
        if (want > 5000) want = 5000;
        if (want < 200) want = 200;
        o.max_results = (uint32_t)want;
    }
    if (!strcmp(type, "dir")) o.dirs_only = 1;
    else if (!strcmp(type, "file")) o.files_only = 1;

    char (*sc)[512] = xmalloc(sizeof(char[512]) * SCOPE_MAX);
    int nsc = -1;
    if (uid >= 0) nsc = scope_load(uid, sc, SCOPE_MAX);

    uint64_t t0 = now_ms();
    ref_t *refs = NULL;
    size_t nref = 0, cref = 0;
    uint64_t total_hits = 0;
    int trunc = 0;

    pthread_rwlock_rdlock(&g_rw);
    for (int i = 0; i < g_nvols; i++) {
        if (!g_vols[i].ok) continue;
        if (g_vols[i].usb && !admin) continue;   /* 外挂盘默认只对管理员可见 */
        uint32_t n = 0;
        int t = 0;
        uint64_t vtotal = 0;
        uint32_t *hits = query_hits(&g_vols[i].sn, q, &o, &n, &t, NULL, &vtotal);
        total_hits += vtotal;
        if (t) trunc = 1;
        if (n) {
            if (nref + n > cref) {
                while (nref + n > cref) cref = cref ? cref * 2 : 4096;
                refs = xrealloc(refs, cref * sizeof(ref_t));
            }
            for (uint32_t k = 0; k < n; k++) {
                refs[nref].vol = (uint16_t)i;
                refs[nref].id = hits[k];
                nref++;
            }
        }
        free(hits);
    }
    if (nref > 1) qsort_r(refs, nref, sizeof(ref_t), cmp_ref, NULL);
    if (catmask) {
        size_t w = 0;
        for (size_t k = 0; k < nref; k++) {
            vol_t *v = &g_vols[refs[k].vol];
            const xl_rec *r = &v->sn.recs[refs[k].id];
            if (r->flags & XL_F_DIR) continue;
            if (!(cat_of_name(v->sn.names + r->name_off, r->name_len) & catmask)) continue;
            refs[w++] = refs[k];
        }
        nref = w;
    }

    xl_live_hit *lh = NULL;
    size_t nlh = live_query(q, &lh);
    if (nlh > 1) qsort(lh, nlh, sizeof(*lh), cmp_live);
    if (catmask && nlh) {
        size_t w = 0;
        for (size_t k = 0; k < nlh; k++) {
            if (lh[k].flags & XL_F_DIR) continue;
            const char *b = strrchr(lh[k].path, '/');
            const char *nm = b ? b + 1 : lh[k].path;
            if (!(cat_of_name(nm, strlen(nm)) & catmask)) continue;
            lh[w++] = lh[k];
        }
        nlh = w;
    }

    sb_printf(out, "{\"query\":\"");
    sb_json(out, q, strlen(q));
    size_t live_dels = 0;
    live_delta_count(&live_dels);
    sb_printf(out, "\",\"total\":%zu,\"approx\":\"true\",\"truncated\":%s,\"offset\":%u,\"limit\":%u,\"items\":[",
              total_hits + nlh, trunc ? "true" : "false", off, limit);
    (void)live_dels;

    char path[8192];
    perm_cache_ent *pcache = calloc(PCACHE_N, sizeof(*pcache));
    if (!pcache) {
        pthread_rwlock_unlock(&g_rw);
        for (size_t k = 0; k < nlh; k++) free(lh[k].path);
        free(lh); free(refs); free(sc);
        sb_puts(out, "],\"ms\":0}");
        return;
    }
    uint32_t shown = 0;
    size_t i = 0, j = 0, skipped = 0;
    while ((i < nref || j < nlh) && shown < limit) {
        int take_live;
        if (i >= nref) take_live = 1;
        else if (j >= nlh) take_live = 0;
        else take_live = lh[j].mtime > g_vols[refs[i].vol].sn.recs[refs[i].id].mtime;

        int is_dir; uint32_t size, mtime; const char *name; size_t nlen;
        int is_usb = 0;
        if (take_live) {
            path[0] = 0;
            snprintf(path, sizeof(path), "%s", lh[j].path);
            const char *b = strrchr(path, '/');
            name = b ? b + 1 : path;
            nlen = strlen(name);
            is_dir = (lh[j].flags & XL_F_DIR) != 0;
            is_usb = strncmp(path, "/vol00/", 7) == 0;
            if (is_usb && !admin) { j++; continue; }
            size = lh[j].size; mtime = lh[j].mtime;
            j++;
        } else {
            vol_t *v = &g_vols[refs[i].vol];
            const xl_rec *r = &v->sn.recs[refs[i].id];
            snap_path(&v->sn, refs[i].id, path, sizeof(path));
            i++;
            if (live_is_deleted(path)) continue;
            name = v->sn.names + r->name_off;
            nlen = r->name_len;
            is_dir = (r->flags & XL_F_DIR) != 0;
            is_usb = v->usb;
            size = r->size; mtime = r->mtime;
        }
        if (uid >= 0) {
            char pd[8192];
            size_t pl = strlen(path);
            if (pl >= sizeof pd) continue;
            memcpy(pd, path, pl + 1);
            char *sl = strrchr(pd, '/');
            if (!sl) continue;
            if (sl == pd) sl[1] = 0; else *sl = 0;
            if (!admin && system_path(path)) continue;
            if (!perm_cache_check(pcache, pd, (uid_t)uid)) continue;
            if (nsc >= 0 && strncmp(path, "/vol00/", 7) != 0 && !scope_allows(sc, nsc, path)) continue;
        }
        {
            struct stat lst;
            int have = stat(path, &lst) == 0;
            /* 索引可能短时间保留已删除/已卸载路径；不要把它们当搜索结果返回 */
            if (!have) continue;
            if (is_dir && !S_ISDIR(lst.st_mode)) continue;
            if (!is_dir && uid >= 0 && !perm_mode_readable((uid_t)uid, path, &lst)) continue;
            if (!is_dir) { size = (uint32_t)lst.st_size; mtime = (uint32_t)lst.st_mtime; }
        }
        if (skipped < off) { skipped++; continue; }
        if (shown) sb_puts(out, ",");
        sb_puts(out, "{\"path\":\"");
        sb_json(out, path, strlen(path));
        sb_puts(out, "\",\"name\":\"");
        sb_json(out, name, nlen);
        sb_printf(out, "\",\"dir\":%s,\"size\":%u,\"mtime\":%u,\"cat\":\"%s\",\"usb\":%s}",
                  is_dir ? "true" : "false", size, mtime,
                  is_dir ? "dir" : cat_key(cat_of_name(name, nlen)),
                  is_usb ? "true" : "false");
        shown++;
    }
    sb_printf(out, "],\"ms\":%.1f}", (double)(now_ms() - t0));
    pthread_rwlock_unlock(&g_rw);
    perm_cache_free(pcache); free(pcache);
    for (size_t k = 0; k < nlh; k++) free(lh[k].path);
    free(lh);
    free(refs);
    free(sc);
}



static int dir_find_id(const xl_snap *sn, const char *dir, uint32_t *id_out);

/* ---- 目录子项索引 ----
 * 启动/重建后一次性按 parent 建 child 链表，列目录只遍历直接子项；
 * 子项按记录号升序（DFS 顺序），与原来的全量扫描顺序一致。 */
static void dir_index_free(vol_t *v) {
    free(v->dir_head);
    free(v->dir_next);
    v->dir_head = v->dir_next = NULL;
    v->root_id = UINT32_MAX;
    v->dir_ready = 0;
}

static void dir_index_build(vol_t *v) {
    dir_index_free(v);
    if (!v->ok) return;
    uint32_t n = (uint32_t)v->sn.h.rec_count;
    if (!n) return;
    v->dir_head = xmalloc((size_t)n * sizeof(uint32_t));
    v->dir_next = xmalloc((size_t)n * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) {
        v->dir_head[i] = UINT32_MAX;
        v->dir_next[i] = UINT32_MAX;
    }
    v->root_id = UINT32_MAX;
    for (uint32_t i = n; i-- > 0;) {
        const xl_rec *r = &v->sn.recs[i];
        if (r->parent == XL_ROOT) {
            if (r->name_len == 0) v->root_id = i;
        } else if (r->parent < n) {
            v->dir_next[i] = v->dir_head[r->parent];
            v->dir_head[r->parent] = i;
        }
    }
    v->dir_ready = (v->root_id != UINT32_MAX);
}

/* 从卷根按路径分量查找目录；未建索引时回退到线性扫描 */
static int dir_lookup(const vol_t *v, const char *dir, uint32_t *id_out) {
    if (!v->dir_ready) return dir_find_id(&v->sn, dir, id_out);
    if (!strcmp(dir, v->root)) { *id_out = v->root_id; return 1; }
    size_t rl = strlen(v->root);
    if (strncmp(dir, v->root, rl) != 0 || dir[rl] != '/') return 0;
    const char *p = dir + rl;
    uint32_t cur = v->root_id;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t len = (size_t)(e - p);
        uint32_t next = UINT32_MAX;
        for (uint32_t i = v->dir_head[cur]; i != UINT32_MAX; i = v->dir_next[i]) {
            const xl_rec *r = &v->sn.recs[i];
            if (!(r->flags & XL_F_DIR) || r->name_len != len) continue;
            if (memcmp(v->sn.names + r->name_off, p, len) == 0) { next = i; break; }
        }
        if (next == UINT32_MAX) return 0;
        cur = next;
        p = e;
    }
    *id_out = cur;
    return 1;
}

/* ---- 目录列表：用于"定位到文件夹"的站内视图 ---- */
static int dir_find_id(const xl_snap *sn, const char *dir, uint32_t *id_out) {
    const char *slash = strrchr(dir, '/');
    const char *bn = slash ? slash + 1 : dir;
    size_t bl = strlen(bn);
    char p[8192];
    for (uint32_t i = 0; i < (uint32_t)sn->h.rec_count; i++) {
        const xl_rec *r = &sn->recs[i];
        if (!(r->flags & XL_F_DIR)) continue;
        if (r->name_len != bl) continue;
        if (bl && memcmp(sn->names + r->name_off, bn, bl)) continue;
        snap_path(sn, i, p, sizeof p);
        if (!strcmp(p, dir)) { *id_out = i; return 1; }
    }
    return 0;
}


static int is_volroot(const char *d) {
    if (d[0] != '/' || strncmp(d + 1, "vol", 3) != 0) return 0;
    const char *q = d + 4;
    if (!*q) return 0;
    while (*q >= '0' && *q <= '9') q++;
    return *q == 0;
}

static void handle_dir(sbuf *out, const char *qs, int uid, int admin) {
    char dir[4096];
    if (!param(qs, "path", dir, sizeof dir)) {
        sb_puts(out, "{\"error\":\"path required\"}");
        return;
    }
    size_t dl = strlen(dir);
    while (dl > 1 && dir[dl - 1] == '/') dir[--dl] = 0;
    {
        /* 规范化后再匹配索引路径，避免 .. / 重复斜杠 / 符号链接造成空列表或越界 */
        char rp[PATH_MAX];
        struct stat dst;
        if (!realpath(dir, rp)) {
            sb_puts(out, "{\"error\":\"not found\",\"entries\":[]}");
            return;
        }
        if (strncmp(rp, "/vol", 4) != 0 || (!admin && !strncmp(rp, "/vol00/", 7))) {
            sb_puts(out, "{\"error\":\"forbidden\",\"entries\":[]}");
            return;
        }
        if (stat(rp, &dst) != 0 || !S_ISDIR(dst.st_mode)) {
            sb_puts(out, "{\"error\":\"not found\",\"entries\":[]}");
            return;
        }
        snprintf(dir, sizeof dir, "%s", rp);
        dl = strlen(dir);
    }
    /* 卷根 /volN 的 mode 是 0000，但飞牛允许浏览后按子目录授权；放行并在下面逐项判权。
       管理员也按当前账号的 ACL 过滤，避免列出别人的家目录与系统目录。 */
    int volroot = is_volroot(dir);
    int all = 0;
    { char tmp[8]; all = (admin && param(qs, "all", tmp, sizeof tmp)); }
    if (uid >= 0 && !all) {
        if (system_path(dir) || (!volroot && !perm_dir_listable((uid_t)uid, dir))) {
            sb_puts(out, "{\"error\":\"forbidden\",\"entries\":[]}");
            return;
        }
    }
    const int MAX_ENT = 4000;
    pthread_rwlock_rdlock(&g_rw);
    sb_puts(out, "{\"path\":\"");
    sb_json(out, dir, dl);
    sb_puts(out, "\",\"entries\":[");
    int shown = 0, total = 0;
    char p[8192];
    for (int pass = 0; pass < 2; pass++) {
        for (int vi = 0; vi < g_nvols && shown < MAX_ENT; vi++) {
            vol_t *v = &g_vols[vi];
            if (!v->ok) continue;
            if (v->usb && !admin) continue;
            uint32_t did;
            if (!dir_lookup(v, dir, &did)) continue;
            uint32_t ci = v->dir_ready ? v->dir_head[did] : 0;
            while (shown < MAX_ENT) {
                uint32_t i;
                if (v->dir_ready) {
                    i = ci;
                    if (i == UINT32_MAX) break;
                    ci = v->dir_next[i];
                } else {
                    while (ci < (uint32_t)v->sn.h.rec_count && v->sn.recs[ci].parent != did) ci++;
                    if (ci >= (uint32_t)v->sn.h.rec_count) break;
                    i = ci++;
                }
                const xl_rec *r = &v->sn.recs[i];
                int is_dir = (r->flags & XL_F_DIR) != 0;
                if (pass == 0 && !is_dir) continue;
                if (pass == 1 && is_dir) continue;
                snap_path(&v->sn, i, p, sizeof p);
                if (live_is_deleted(p)) continue;
                if (uid >= 0 && is_dir) {
                    if (!all && sysnoise_name(v->sn.names + r->name_off, r->name_len)) continue;
                    if (!all && (system_path(p) || !perm_dir_listable((uid_t)uid, p))) continue;
                }
                uint32_t esz = r->size, emt = r->mtime;
                if (!is_dir) {
                    struct stat lst;
                    if (stat(p, &lst) != 0) continue;
                    if (uid >= 0 && !perm_mode_readable((uid_t)uid, p, &lst)) continue;
                    if (lst.st_size > 0) esz = (uint32_t)lst.st_size;
                    if (lst.st_mtime > 0) emt = (uint32_t)lst.st_mtime;
                }
                total++;
                if (shown) sb_puts(out, ",");
                sb_puts(out, "{\"path\":\"");
                sb_json(out, p, strlen(p));
                sb_puts(out, "\",\"name\":\"");
                sb_json(out, v->sn.names + r->name_off, r->name_len);
                sb_printf(out, "\",\"dir\":%s,\"size\":%u,\"mtime\":%u,\"cat\":\"%s\"}",
                          is_dir ? "true" : "false", esz, emt,
                          is_dir ? "dir" : cat_key(cat_of_name(v->sn.names + r->name_off, r->name_len)));
                shown++;
            }
        }
    }
    /* 基线快照最多 6 小时，目录里刚创建的文件也必须可见 */
    {
        xl_live_hit *lh = NULL;
        size_t nlh = live_dir(dir, &lh);
        for (size_t li = 0; li < nlh && shown < MAX_ENT; li++) {
            const char *p = lh[li].path;
            if (!admin && strncmp(p, "/vol00/", 7) == 0) continue;
            const char *bn = strrchr(p, '/');
            bn = bn ? bn + 1 : p;
            int is_dir = (lh[li].flags & XL_F_DIR) != 0;
            if (uid >= 0 && !admin) {
                if (system_path(p)) continue;
                if (is_dir && sysnoise_name(bn, (uint16_t)strlen(bn))) continue;
                const char *sl = strrchr(p, '/');
                char pd[8192];
                if (!sl) continue;
                size_t pl = (size_t)(sl - p);
                if (pl >= sizeof pd) continue;
                memcpy(pd, p, pl); pd[pl] = 0;
                if (!perm_dir_listable((uid_t)uid, pd)) continue;
            }
            struct stat lst;
            int have = stat(p, &lst) == 0;
            if (!is_dir && uid >= 0 && (!have || !perm_mode_readable((uid_t)uid, p, &lst))) continue;
            if (shown) sb_puts(out, ",");
            sb_puts(out, "{\"path\":\"");
            sb_json(out, p, strlen(p));
            sb_puts(out, "\",\"name\":\"");
            sb_json(out, bn, strlen(bn));
            sb_printf(out, "\",\"dir\":%s,\"size\":%u,\"mtime\":%u,\"cat\":\"%s\"}",
                      is_dir ? "true" : "false",
                      have ? (unsigned)lh[li].size : 0,
                      have ? (unsigned)lst.st_mtime : (unsigned)lh[li].mtime,
                      is_dir ? "dir" : cat_key(cat_of_name(bn, strlen(bn))));
            shown++; total++;
        }
        for (size_t li = 0; li < nlh; li++) free(lh[li].path);
        free(lh);
    }
    sb_printf(out, "],\"count\":%d,\"truncated\":%s}", total, shown >= MAX_ENT ? "true" : "false");
    pthread_rwlock_unlock(&g_rw);
}


/* ---- 设置与授权目录 ---- */
static void settings_path(char *out, size_t cap) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", g_idx_dir);
    char *sl = strrchr(tmp, '/');
    if (sl) *sl = 0;
    size_t need = strlen(tmp);
    if (need + 14 >= cap) need = cap > 15 ? cap - 15 : 0;
    memcpy(out, tmp, need);
    memcpy(out + need, "/settings.json", 15);
}

/* 检测飞牛文档（trim.docs / OnlyOffice）是否安装并运行 */
static void onlyoffice_probe(sbuf *out) {
    const char *cfg = "/usr/local/apps/@appcenter/trim.docs/ui/config";
    int installed = (access(cfg, R_OK) == 0);
    int running = 0, port_open = 0;
    if (!installed) {
        const char *alt = "/vol1/@appcenter/trim.docs/ui/config";
        installed = (access(alt, R_OK) == 0);
    }
    if (installed) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET;
            sa.sin_port = htons(10500);
            sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            port_open = (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0);
            close(fd);
        }
        running = port_open;
    }
    sb_printf(out, "{\"installed\":%s,\"running\":%s,\"port\":10500,\"name\":\"飞牛文档\"}",
              installed ? "true" : "false", running ? "true" : "false");
}



static void preview_probe(sbuf *out) {
    const char *cfg = "/usr/local/apps/@appcenter/trim.preview/ui/config";
    const char *sock = "/usr/local/apps/@appcenter/trim.preview/app.sock";
    int installed = (access(cfg, R_OK) == 0);
    if (!installed) installed = (access("/vol1/@appcenter/trim.preview/ui/config", R_OK) == 0);
    int running = 0;
    if (installed) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_un sa;
            memset(&sa, 0, sizeof sa);
            sa.sun_family = AF_UNIX;
            snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
            running = (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0);
            close(fd);
        }
    }
    sb_printf(out, "{\"installed\":%s,\"running\":%s,\"name\":\"飞牛预览\"}",
              installed ? "true" : "false", running ? "true" : "false");
}

/* ---- 检索范围（用户在设置里勾选的目录） ---- */
static void scope_path(int uid, char *out, size_t cap) {
    char tmp[1024];
    settings_path(tmp, sizeof tmp);
    char *sl = strrchr(tmp, '/');
    if (sl) *sl = 0;
    snprintf(out, cap, "%.900s/scope-%d.txt", tmp, uid);
}

static int scope_load(int uid, char paths[][512], int cap) {
    char sp[1200];
    scope_path(uid, sp, sizeof sp);
    FILE *f = fopen(sp, "r");
    if (!f) return -1;                  /* -1 = 用户没设过 */
    int n = 0;
    char line[512];
    while (n < cap && fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
        if (!l) continue;
        snprintf(paths[n], 512, "%s", line);
        n++;
    }
    fclose(f);
    return n;
}

static void scope_save(int uid, char (*paths)[512], int n) {
    char sp[1200];
    scope_path(uid, sp, sizeof sp);
    FILE *f = fopen(sp, "w");
    if (!f) return;
    fchmod(fileno(f), 0600);
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", paths[i]);
    fclose(f);
}

/* 路径 p 是否在范围内：取最长匹配，排除项（前缀 '!'）优先 */
static int scope_allows(char (*paths)[512], int n, const char *p) {
    size_t best_in = 0, best_ex = 0;
    for (int i = 0; i < n; i++) {
        const char *s = paths[i];
        int ex = (*s == '!');
        if (ex) s++;
        size_t l = strlen(s);
        if (!l) continue;
        if (strncmp(p, s, l) != 0) continue;
        if (p[l] != 0 && p[l] != '/') continue;
        if (ex) { if (l >= best_ex) best_ex = l; }
        else    { if (l >  best_in) best_in = l; }
    }
    return best_in > best_ex;
}

/* 默认范围：家目录（/volN/<uid>）+ 单独授权的目录，排除 everyone 共享盘 */
static int scope_default(int uid, char (*paths)[512], int cap) {
    sbuf tmp;
    memset(&tmp, 0, sizeof tmp);
    handle_acl_list(&tmp, uid, 0, 0);
    int n = 0;
    const char *p = tmp.p ? tmp.p : "";
    const char *cur = p;
    while (n < cap) {
        const char *pp = strstr(cur, "\"path\":\"");
        if (!pp) break;
        pp += 8;
        const char *e = strchr(pp, '"');
        if (!e) break;
        char path[512];
        size_t pl = (size_t)(e - pp);
        if (pl >= sizeof path) pl = sizeof path - 1;
        memcpy(path, pp, pl); path[pl] = 0;
        const char *kp = strstr(e, "\"kind\":\"");
        char kind[16] = "";
        if (kp) { kp += 8; const char *ke = strchr(kp, '"'); if (ke) {
            size_t kl = (size_t)(ke - kp); if (kl >= sizeof kind) kl = sizeof kind - 1;
            memcpy(kind, kp, kl); kind[kl] = 0; } }
        const char *bn = strrchr(path, '/');
        bn = bn ? bn + 1 : path;
        int digits = *bn != 0;
        for (const char *q = bn; *q; q++) if (*q < '0' || *q > '9') { digits = 0; break; }
        if (!strcmp(kind, "granted") || digits) {
            snprintf(paths[n], 512, "%s", path);
            n++;
        }
        cur = kp ? kp : e;
    }
    free(tmp.p);
    return n;
}

/* 从 JSON 里抠出 key 对应的对象（返回 '{' 起始与长度） */
static const char *json_obj(const char *s, const char *key, size_t *len) {
    const char *p = strstr(s, key);
    if (!p) return NULL;
    p = strchr(p, '{');
    if (!p) return NULL;
    int depth = 0;
    const char *e = p;
    for (; *e; e++) {
        if (*e == '{') depth++;
        else if (*e == '}') { if (--depth == 0) { e++; break; } }
    }
    if (depth != 0) return NULL;
    *len = (size_t)(e - p);
    return p;
}

static int json_int(const char *s, const char *key, int def) {
    const char *p = strstr(s, key);
    if (!p) return def;
    p = strchr(p, ':');
    if (!p) return def;
    p++;
    while (*p == ' ' || *p == '"') p++;
    if (*p < '0' || *p > '9') return def;
    return atoi(p);
}

static void json_mode(const char *s, char *out, size_t cap) {
    const char *p = strstr(s, "\"mode\"");
    out[0] = 0;
    if (!p) return;
    p = strchr(p, ':');
    if (!p) return;
    p = strchr(p, '"');
    if (!p) return;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return;
    size_t n = (size_t)(e - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

static void json_str(const char *s, const char *key, char *out, size_t cap, const char *def) {
    snprintf(out, cap, "%s", def);
    const char *p = strstr(s, key);
    if (!p) return;
    p = strchr(p, ':');
    if (!p) return;
    p = strchr(p, '"');
    if (!p) return;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return;
    size_t n = (size_t)(e - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

static void rebuild_cfg_load(void) {
    char sp[1200];
    settings_path(sp, sizeof sp);
    FILE *f = fopen(sp, "r");
    if (!f) return;
    char buf[16384];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    size_t len = 0;
    const char *o = json_obj(buf, "\"rebuild\"", &len);
    if (!o) return;
    char tmp[256];
    if (len >= sizeof tmp) len = sizeof tmp - 1;
    memcpy(tmp, o, len);
    tmp[len] = 0;
    char mode[16];
    json_mode(tmp, mode, sizeof mode);
    int m = 0;
    if (!strcmp(mode, "daily")) m = 1;
    else if (!strcmp(mode, "interval")) m = 2;
    int hour = json_int(tmp, "\"hour\"", 3);
    int minute = json_int(tmp, "\"minute\"", 0);
    int hours = json_int(tmp, "\"hours\"", 6);
    if (hour < 0 || hour > 23) hour = 3;
    if (minute < 0 || minute > 59) minute = 0;
    if (hours < 1) hours = 1;
    if (hours > 168) hours = 168;
    pthread_mutex_lock(&g_rcfg_mu);
    g_rcfg.mode = m; g_rcfg.hour = hour; g_rcfg.minute = minute; g_rcfg.hours = hours;
    pthread_mutex_unlock(&g_rcfg_mu);
}

/* 是否到了该全盘扫描的时间 */
static int schedule_due(void) {
    rebuild_cfg c;
    pthread_mutex_lock(&g_rcfg_mu);
    c = g_rcfg;
    pthread_mutex_unlock(&g_rcfg_mu);
    if (c.mode == 0) return 0;
    time_t now = time(NULL);
    if (c.mode == 2)
        return g_last_rebuild && now_ms() - g_last_rebuild > (uint64_t)c.hours * 3600000ull;
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = c.hour; tm.tm_min = c.minute; tm.tm_sec = 0;
    time_t target = mktime(&tm);
    if (target < 0 || now < target) return 0;
    /* 今天目标时间之后已经扫过就不再扫（重启后 last=启动时刻，也不会立刻重扫） */
    time_t last_epoch = g_last_rebuild ? now - (time_t)((now_ms() - g_last_rebuild) / 1000) : 0;
    return last_epoch < target;
}


static void base_of(const char *p, char *out, size_t cap)
{
    const char *s = strrchr(p, '/');
    snprintf(out, cap, "%.*s", (int)(cap - 1), s ? s + 1 : p);
}

/* 源路径校验：/volN 之下、无 ..、非系统目录、回收站之外、ACL 可列 */
static int fs_guard(int uid, const char *p, int want_dir, char *err, size_t cap)
{
    if (!p || strlen(p) < 6 || strncmp(p, "/vol", 4) != 0) {
        snprintf(err, cap, "路径不在存储卷内"); return -1;
    }
    if (strstr(p, "/../") || (strlen(p) > 3 && !strcmp(p + strlen(p) - 3, "/.."))) {
        snprintf(err, cap, "路径非法"); return -1;
    }
    if (g_trash[0] && !strncmp(p, g_trash, strlen(g_trash))) {
        snprintf(err, cap, "回收站内的文件不可操作"); return -1;
    }
    if (usb_path_blocked(uid, p)) { snprintf(err, cap, "USB 外挂盘仅管理员可操作"); return -1; }
    if (system_path(p)) { snprintf(err, cap, "系统目录不可操作"); return -1; }

    struct stat st;
    if (lstat(p, &st) != 0) { snprintf(err, cap, "路径不存在"); return -1; }
    if (want_dir && !S_ISDIR(st.st_mode)) { snprintf(err, cap, "目标不是目录"); return -1; }
    {
        /* 解析符号链接后再确认仍在存储卷内，避免经软链访问卷外文件 */
        char real[PATH_MAX];
        if (!realpath(p, real) || strncmp(real, "/vol", 4) != 0) {
            snprintf(err, cap, "路径越界"); return -1;
        }
    }

    char check[4096];
    if (S_ISDIR(st.st_mode)) snprintf(check, sizeof check, "%s", p);
    else {
        snprintf(check, sizeof check, "%s", p);
        char *slash = strrchr(check, '/');
        if (!slash || slash == check) { snprintf(err, cap, "路径非法"); return -1; }
        *slash = 0;
    }
    if (uid > 0 && !perm_dir_listable((uid_t)uid, check)) {
        snprintf(err, cap, "没有权限"); return -1;
    }
    /* 目录项可列不代表文件本身可读：0600 他人文件必须再判目标 R */
    if (!want_dir && !S_ISDIR(st.st_mode) && uid > 0 && !perm_file_readable((uid_t)uid, p)) {
        snprintf(err, cap, "没有文件读权限"); return -1;
    }
    return 0;
}

/* 从 body 中取字符串数组，返回元素个数 */
static int json_str_array(const char *body, const char *key, char *out, size_t stride, int max)
{
    const char *p = strstr(body, key);
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;
    int n = 0;
    while (*p && *p != ']') {
        if (*p != '"') { p++; continue; }
        p++;
        if (n >= max) break;
        size_t k = 0;
        char *dst = out + (size_t)n * stride;
        while (*p && *p != '"' && k + 1 < stride) {
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'n') dst[k++] = '\n';
                else dst[k++] = *p;
            } else dst[k++] = *p;
            p++;
        }
        dst[k] = 0;
        if (*p == '"') p++;
        if (k) n++;
    }
    return n;
}

static void handle_fsop(sbuf *out, const char *body, int uid)
{
    if (!json_ok(body)) { sb_puts(out, "{\"ok\":false,\"error\":\"bad json\"}"); return; }
    char op[16] = {0}, dest[4200] = {0};
    json_str(body, "\"op\"", op, sizeof op, "");
    json_str(body, "\"dest\"", dest, sizeof dest, "");

    /* 每个请求独立分配，避免 4 个 worker 共用 static 缓冲互相串改路径 */
    char (*paths)[4200] = calloc(200, sizeof *paths);
    if (!paths) { sb_puts(out, "{\"ok\":false,\"error\":\"内存不足\"}"); return; }
    int n = json_str_array(body, "\"paths\"", &paths[0][0], sizeof paths[0], 200);
    if (!n) { free(paths); sb_puts(out, "{\"ok\":false,\"error\":\"未选择文件\"}"); return; }
    if (uid < 0) { free(paths); sb_puts(out, "{\"ok\":false,\"error\":\"未识别用户\"}"); return; }

    int is_trash = !strcmp(op, "trash");
    if (!is_trash && strcmp(op, "copy") && strcmp(op, "move")) {
        free(paths); sb_puts(out, "{\"ok\":false,\"error\":\"未知操作\"}"); return;
    }

    char derr[256] = {0};
    if (!is_trash) {
        if (!dest[0]) { free(paths); sb_puts(out, "{\"ok\":false,\"error\":\"目标目录为空\"}"); return; }
        if (fs_guard(uid, dest, 1, derr, sizeof derr) != 0) {
            free(paths);
            sb_printf(out, "{\"ok\":false,\"error\":\"目标：%s\"}", derr);
            return;
        }
        if (!perm_dir_writable((uid_t)uid, dest)) {
            free(paths); sb_puts(out, "{\"ok\":false,\"error\":\"目标目录不可写\"}"); return;
        }
    }

    int done = 0, failed = 0;
    sb_puts(out, "{\"ok\":true,\"failed\":[");
    for (int i = 0; i < n; i++) {
        char err[256] = {0};
        if (fs_guard(uid, paths[i], 0, err, sizeof err) != 0) {
            if (failed++) sb_puts(out, ",");
            sb_puts(out, "{\"path\":\"");
            sb_json(out, paths[i], strlen(paths[i]));
            sb_puts(out, "\",\"error\":\"");
            sb_json(out, err, strlen(err));
            sb_puts(out, "\"}");
            continue;
        }
        /* move/trash 会改变源目录内容，必须拥有源父目录写权限 */
        if ((is_trash || !strcmp(op, "move")) && !perm_parent_writable((uid_t)uid, paths[i])) {
            if (failed++) sb_puts(out, ",");
            sb_puts(out, "{\"path\":\"");
            sb_json(out, paths[i], strlen(paths[i]));
            sb_puts(out, "\",\"error\":\"源目录不可写\"}");
            continue;
        }
        char name[512];
        base_of(paths[i], name, sizeof name);
        if (is_trash) {
            char id[64];
            if (fs_trash_path(paths[i], g_trash, (uid_t)uid, id, sizeof id, err, sizeof err) == 0) done++;
            else {
                if (failed++) sb_puts(out, ",");
                sb_puts(out, "{\"path\":\"");
                sb_json(out, paths[i], strlen(paths[i]));
                sb_puts(out, "\",\"error\":\"");
                sb_json(out, err, strlen(err));
                sb_puts(out, "\"}");
            }
            continue;
        }
        char dst[4300];
        if (strlen(dest) + 1 + strlen(name) >= sizeof dst) {
            if (failed++) sb_puts(out, ",");
            sb_puts(out, "{\"path\":\"");
            sb_json(out, paths[i], strlen(paths[i]));
            sb_puts(out, "\",\"error\":\"路径过长\"}");
            continue;
        }
        {
            size_t dl = strlen(dest), nl = strlen(name);
            memcpy(dst, dest, dl);
            dst[dl] = '/';
            memcpy(dst + dl + 1, name, nl + 1);
        }
        struct stat st;
        if (lstat(dst, &st) == 0) {
            if (failed++) sb_puts(out, ",");
            sb_puts(out, "{\"path\":\"");
            sb_json(out, paths[i], strlen(paths[i]));
            sb_puts(out, "\",\"error\":\"目标已存在\"}");
            continue;
        }
        int rc = !strcmp(op, "copy") ? fs_copy_path(paths[i], dst, (uid_t)uid, err, sizeof err)
                                       : fs_move_path(paths[i], dst, err, sizeof err);
        if (rc == 0) done++;
        else {
            if (failed++) sb_puts(out, ",");
            sb_puts(out, "{\"path\":\"");
            sb_json(out, paths[i], strlen(paths[i]));
            sb_puts(out, "\",\"error\":\"");
            sb_json(out, err, strlen(err));
            sb_puts(out, "\"}");
        }
    }
    sb_printf(out, "],\"done\":%d,\"failedCount\":%d}", done, failed);
    free(paths);
}

static void handle_trash_get(sbuf *out, int uid)
{
    fs_trash_item *items = calloc(500, sizeof *items);
    if (!items) { sb_puts(out, "{\"error\":\"内存不足\"}"); return; }
    int n = fs_trash_scan(g_trash, (uid_t)uid, items, 500);
    sb_puts(out, "{\"items\":[");
    for (int i = 0; i < n; i++) {
        if (i) sb_puts(out, ",");
        sb_puts(out, "{\"id\":\"");
        sb_json(out, items[i].id, strlen(items[i].id));
        sb_puts(out, "\",\"name\":\"");
        sb_json(out, items[i].name, strlen(items[i].name));
        sb_puts(out, "\",\"orig\":\"");
        sb_json(out, items[i].orig, strlen(items[i].orig));
        sb_printf(out, "\",\"size\":%lld,\"mtime\":%lld,\"deleted\":%lld}",
                  items[i].size, items[i].mtime, items[i].deleted);
    }
    sb_puts(out, "]}");
    free(items);
}

static void handle_trash_post(sbuf *out, const char *body, int uid, int admin)
{
    if (!json_ok(body)) { sb_puts(out, "{\"ok\":false,\"error\":\"bad json\"}"); return; }
    char op[16] = {0};
    json_str(body, "\"op\"", op, sizeof op, "");
    if (!strcmp(op, "empty")) {
        if (!admin) { sb_puts(out, "{\"ok\":false,\"error\":\"需要管理员权限\"}"); return; }
        int removed = 0;
        char err[256] = {0};
        if (fs_empty_trash(g_trash, &removed, err, sizeof err) != 0) {
            sb_puts(out, "{\"ok\":false,\"error\":\"");
            sb_json(out, err, strlen(err));
            sb_puts(out, "\"}");
            return;
        }
        sb_printf(out, "{\"ok\":true,\"removed\":%d}", removed);
        return;
    }
    if (!strcmp(op, "restore")) {
        char ids[200][64];
        int n = json_str_array(body, "\"ids\"", &ids[0][0], sizeof ids[0], 200);
        int done = 0, failed = 0;
        sb_puts(out, "{\"ok\":true,\"failed\":[");
        for (int i = 0; i < n; i++) {
            char err[256] = {0};
            if (fs_restore(g_trash, ids[i], (uid_t)uid, err, sizeof err) == 0) done++;
            else {
                if (failed++) sb_puts(out, ",");
                sb_puts(out, "{\"id\":\"");
                sb_json(out, ids[i], strlen(ids[i]));
                sb_puts(out, "\",\"error\":\"");
                sb_json(out, err, strlen(err));
                sb_puts(out, "\"}");
            }
        }
        sb_printf(out, "],\"done\":%d,\"failedCount\":%d}", done, failed);
        return;
    }
    sb_puts(out, "{\"ok\":false,\"error\":\"未知操作\"}");
}

/* 从 JSON 文本里取布尔值：找 "key"，取其后的 true/false，未找到返回 def */
static int json_bool_def(const char *buf, const char *key, int def) {
    if (!buf) return def;
    const char *k = strstr(buf, key);
    if (!k) return def;
    const char *c = strchr(k + strlen(key), ':');
    if (!c) return def;
    c++;
    while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') c++;
    if (strncmp(c, "true", 4) == 0) return 1;
    if (strncmp(c, "false", 5) == 0) return 0;
    return def;
}

/* 扫描设置：skipHidden 默认开启（忽略 . / @ 开头的隐藏目录与文件） */
static void scan_cfg_load(void) {
    char sp[1200];
    settings_path(sp, sizeof sp);
    scan_set_skip_hidden(1);
    FILE *f = fopen(sp, "r");
    if (!f) return;
    char buf[16384];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    scan_set_skip_hidden(json_bool_def(buf, "\"skipHidden\"", 1));
}

static void handle_settings_get(sbuf *out, int uid, int admin, int all) {
    char sp[1200];
    settings_path(sp, sizeof sp);
    char *buf = NULL;
    long n = 0;
    FILE *f = fopen(sp, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 65536) {
            buf = malloc((size_t)n + 1);
            if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) buf[n] = 0;
        }
        fclose(f);
    }
    size_t owlen = 0;
    const char *ow = buf ? json_obj(buf, "\"openWith\"", &owlen) : NULL;
    sb_puts(out, "{\"openWith\":");
    if (ow && owlen) sb_add(out, ow, owlen);
    else sb_puts(out, "{}");
    sb_printf(out, ",\"settingsVersion\":%d",
              json_int(buf ? buf : "", "\"settingsVersion\"", 1));
    {
        char oi[16];
        json_str(buf ? buf : "", "\"openIn\"", oi, sizeof oi, "embed");
        sb_printf(out, ",\"openIn\":\"%s\"", oi);
    }
    sb_printf(out, ",\"strictHome\":%s",
              json_bool_def(buf, "\"strictHome\"", 1) ? "true" : "false");
    sb_printf(out, ",\"skipHidden\":%s", scan_skip_hidden() ? "true" : "false");
    sb_printf(out, ",\"usbEnabled\":%s,\"usbWatch\":%s,\"usbMessage\":\"",
              g_usb_enabled ? "true" : "false", g_usb_watch ? "true" : "false");
    char umsg[1024]; usb_msg_get(umsg, sizeof umsg); sb_json(out, umsg, strlen(umsg));
    sb_puts(out, "\",\"usbMounts\":[");
    {
        int ufirst = 0;
        pthread_rwlock_rdlock(&g_rw);
        for (int i = g_nvols_local; i < g_nvols; i++) {
            vol_t *v = &g_vols[i];
            if (!v->usb || !admin) continue;
            if (ufirst++) sb_puts(out, ",");
            sb_puts(out, "{\"path\":\"");
            sb_json(out, v->root, strlen(v->root));
            sb_printf(out, "\",\"records\":%llu,\"ok\":%s,\"building\":%s}",
                      (unsigned long long)v->records, v->ok ? "true" : "false",
                      v->building ? "true" : "false");
        }
        pthread_rwlock_unlock(&g_rw);
    }
    sb_puts(out, "]");
    sb_puts(out, ",\"onlyoffice\":");
    onlyoffice_probe(out);
    sb_puts(out, ",\"preview\":");
    preview_probe(out);
    {
        rebuild_cfg c;
        pthread_mutex_lock(&g_rcfg_mu);
        c = g_rcfg;
        pthread_mutex_unlock(&g_rcfg_mu);
        const char *m = c.mode == 1 ? "daily" : (c.mode == 2 ? "interval" : "off");
        sb_printf(out, ",\"rebuild\":{\"mode\":\"%s\",\"hour\":%d,\"minute\":%d,\"hours\":%d,"
                       "\"last\":%llu,\"building\":%s}",
                  m, c.hour, c.minute, c.hours,
                  (unsigned long long)(g_last_rebuild
                      ? time(NULL) - (now_ms() - g_last_rebuild) / 1000
                      : 0), g_building ? "true" : "false");
    }
    /* 当前用户的检索范围 */
    char (*sc)[512] = xmalloc(sizeof(char[512]) * SCOPE_MAX);
    int sn = -1;
    if (uid >= 0) {
        sn = scope_load(uid, sc, SCOPE_MAX);
        if (sn < 0) sn = scope_default(uid, sc, SCOPE_MAX);
    }
    sb_puts(out, ",\"scope\":[");
    for (int i = 0; i < sn; i++) {
        if (i) sb_puts(out, ",");
        sb_puts(out, "\"");
        sb_json(out, sc[i], strlen(sc[i]));
        sb_puts(out, "\"");
    }
    sb_puts(out, "],\"scopeAvailable\":");
    {
        sbuf tmp;
        memset(&tmp, 0, sizeof tmp);
        handle_acl_list(&tmp, uid, admin, all);
        sb_printf(out, "%s", tmp.p ? tmp.p : "{\"paths\":[]}");
        free(tmp.p);
    }
    sb_puts(out, "}");
    free(sc);
    free(buf);
}

/* 保存检索范围；返回 0=成功，-1=条目过多，-2=格式错误/被截断 */
static int save_scope_body(const char *body, int uid) {
    if (uid < 0) return 0;
    const char *sp = strstr(body, "\"scope\"");
    if (!sp) return 0;                     /* 没提交 scope：保持原值 */
    const char *lb = strchr(sp, '[');
    if (!lb) return -2;
    const char *rb = strchr(lb, ']');
    if (!rb) return -2;                    /* body 被截断 */
    char (*sc)[512] = xmalloc(sizeof(char[512]) * SCOPE_MAX);
    int n = 0, overflow = 0;
    const char *q = lb;
    while ((q = strchr(q, '"')) != NULL && q < rb) {
        q++;
        const char *e2 = strchr(q, '"');
        if (!e2 || e2 > rb) break;
        size_t pl = (size_t)(e2 - q);
        if (pl > 0 && pl < 512) {
            int valid = (q[0] == '/') || (q[0] == '!' && pl > 1 && q[1] == '/');
            if (valid) {
                if (n < SCOPE_MAX) {
                    memcpy(sc[n], q, pl);
                    sc[n][pl] = 0;
                    n++;
                } else {
                    overflow = 1;
                }
            }
        }
        q = e2 + 1;
    }
    int rc = overflow ? -1 : 0;
    if (!rc) scope_save(uid, sc, n);
    free(sc);
    return rc;
}

static void handle_settings_post(sbuf *out, const char *body, int uid, int admin) {
    static _Thread_local char ow_keep[8192];
    if (!json_ok(body)) { sb_puts(out, "{\"ok\":false,\"error\":\"bad json\"}"); return; }
    /* 全局设置只允许管理员修改；普通用户只能保存自己的检索范围 */
    /* 普通用户只保存自己的检索范围；全局字段忽略，不能因为 UI 一并提交就拒绝 */
    if (!admin) {
        int sr = save_scope_body(body, uid);
        if (sr == -1) { sb_puts(out, "{\"ok\":false,\"error\":\"检索范围最多 512 项\"}"); return; }
        if (sr == -2) { sb_puts(out, "{\"ok\":false,\"error\":\"设置内容格式错误或已被截断\"}"); return; }
        sb_puts(out, "{\"ok\":true}");
        return;
    }
    const char *b = NULL;
    size_t len = 0;
    const char *p = strstr(body, "\"openWith\"");
    if (p) {
        b = strchr(p, '{');
        if (!b) { sb_puts(out, "{\"ok\":false,\"error\":\"bad json\"}"); return; }
        int depth = 0; const char *e = b;
        for (; *e; e++) {
            if (*e == '{') depth++;
            else if (*e == '}') { if (--depth == 0) { e++; break; } }
        }
        if (depth != 0) { sb_puts(out, "{\"ok\":false,\"error\":\"bad json\"}"); return; }
        len = (size_t)(e - b);
        if (len > 8192) len = 8192;
    }
    if (!b) {
        /* 兼容只提交部分字段：保留现有 openWith */
        if (!body || !*body) { sb_puts(out, "{\"ok\":false,\"error\":\"empty body\"}"); return; }
        char sp0[1200];
        settings_path(sp0, sizeof sp0);
        ow_keep[0] = 0;
        FILE *f0 = fopen(sp0, "r");
        if (f0) { size_t r = fread(ow_keep, 1, sizeof(ow_keep) - 1, f0); ow_keep[r] = 0; fclose(f0); }
        size_t kl = 0;
        const char *ko = ow_keep[0] ? json_obj(ow_keep, "\"openWith\"", &kl) : NULL;
        if (ko && kl > 1 && kl < sizeof(ow_keep)) {
            memmove(ow_keep, ko, kl); ow_keep[kl] = 0; b = ow_keep; len = kl;
        } else {
            strcpy(ow_keep, "{}"); b = ow_keep; len = 2;
        }
        fprintf(stderr, "xuanlan serve: settings POST 缺少 openWith，按现有值保存 (body=%zu bytes)\n", strlen(body));
    }

    char open_in[16];
    json_str(body, "\"openIn\"", open_in, sizeof open_in, "embed");
    if (strcmp(open_in, "embed")) snprintf(open_in, sizeof open_in, "window");

    /* 自动扫描计划 */
    rebuild_cfg rc;
    pthread_mutex_lock(&g_rcfg_mu);
    rc = g_rcfg;
    pthread_mutex_unlock(&g_rcfg_mu);
    size_t rlen = 0;
    const char *ro = json_obj(body, "\"rebuild\"", &rlen);
    if (ro && rlen < 256) {
        char tmp[256];
        memcpy(tmp, ro, rlen);
        tmp[rlen] = 0;
        char mode[16];
        json_mode(tmp, mode, sizeof mode);
        rc.mode = !strcmp(mode, "daily") ? 1 : (!strcmp(mode, "interval") ? 2 : 0);
        rc.hour = json_int(tmp, "\"hour\"", rc.hour);
        rc.minute = json_int(tmp, "\"minute\"", rc.minute);
        rc.hours = json_int(tmp, "\"hours\"", rc.hours);
        if (rc.hour < 0 || rc.hour > 23) rc.hour = 3;
        if (rc.minute < 0 || rc.minute > 59) rc.minute = 0;
        if (rc.hours < 1) rc.hours = 1;
        if (rc.hours > 168) rc.hours = 168;
        pthread_mutex_lock(&g_rcfg_mu);
        g_rcfg = rc;
        pthread_mutex_unlock(&g_rcfg_mu);
    }

    {
        int sr = save_scope_body(body, uid);
        if (sr == -1) { sb_puts(out, "{\"ok\":false,\"error\":\"检索范围最多 512 项\"}"); return; }
        if (sr == -2) { sb_puts(out, "{\"ok\":false,\"error\":\"设置内容格式错误或已被截断\"}"); return; }
    }

    int strict_home = json_bool_def(body, "\"strictHome\"", perm_strict_home);
    perm_set_strict_home(strict_home);

    /* 隐藏目录开关：默认开启；改变后立即重建索引（旧快照里可能仍有 @eaDir 等条目） */
    int skip_hidden = json_bool_def(body, "\"skipHidden\"", scan_skip_hidden());

    char sp[1200], tmp[1300];
    settings_path(sp, sizeof sp);
    snprintf(tmp, sizeof tmp, "%s.tmp", sp);
    FILE *f = fopen(tmp, "w");
    if (!f) { sb_puts(out, "{\"ok\":false,\"error\":\"cannot write\"}"); return; }
    fchmod(fileno(f), 0600);
    const char *m = rc.mode == 1 ? "daily" : (rc.mode == 2 ? "interval" : "off");
    fprintf(f, "{\"settingsVersion\":2,\"openWith\":%.*s,\"openIn\":\"%s\",\"strictHome\":%s,"
               "\"skipHidden\":%s,"
               "\"rebuild\":{\"mode\":\"%s\",\"hour\":%d,\"minute\":%d,\"hours\":%d}}",
            (int)len, b, open_in, strict_home ? "true" : "false",
            skip_hidden ? "true" : "false",
            m, rc.hour, rc.minute, rc.hours);
    fflush(f);
    fsync(fileno(f));
    if (fclose(f) != 0 || rename(tmp, sp) != 0) {
        unlink(tmp);
        sb_puts(out, "{\"ok\":false,\"error\":\"cannot write\"}");
        return;
    }

    if (skip_hidden != scan_skip_hidden()) {
        scan_set_skip_hidden(skip_hidden);
        fprintf(stderr, "xuanlan serve: skipHidden=%d，触发全盘重建\n", skip_hidden);
        start_build();
        usb_rebuild_all();
    }
    {
        int usb_watch = usb_json_bool(body, "\"usbWatch\"", g_usb_watch);
        int usb_on = usb_json_bool(body, "\"usbEnabled\"", g_usb_enabled);
        if (usb_watch != g_usb_watch) { g_usb_watch = usb_watch; usb_cfg_save(); }
        usb_enable(usb_on);
    }
    sb_puts(out, "{\"ok\":true}");
}

static void handle_acl_list(sbuf *out, int uid, int admin, int all) {
    sb_printf(out, "{\"uid\":%d,\"admin\":%s,\"paths\":[", uid, admin ? "true" : "false");
    int shown = 0;
    if (all && admin) {
        /* 管理员全盘视角：直接给卷根，展开时不再过滤 */
        pthread_rwlock_rdlock(&g_rw);
        for (int vi = 0; vi < g_nvols; vi++) {
            if (!g_vols[vi].ok) continue;
            if (shown++) sb_puts(out, ",");
            sb_puts(out, "{\"path\":\"");
            sb_json(out, g_vols[vi].root, strlen(g_vols[vi].root));
            sb_puts(out, "\",\"kind\":\"admin\"}");
        }
        pthread_rwlock_unlock(&g_rw);
        sb_puts(out, "]}");
        return;
    }
    /* 默认：管理员也按当前账号的 ACL 列举，只显示真正可访问的目录；
       没有可访问子目录的卷不再出现。 */
    pthread_rwlock_rdlock(&g_rw);
    for (int vi = 0; vi < g_nvols && shown < 200; vi++) {
        vol_t *v = &g_vols[vi];
        if (!v->ok) continue;
        if (v->usb) {
            /* 外接分区作为单一根节点，子目录展开时再列，不要把分区内容平铺到顶层 */
            if (!admin) continue;
            if (shown++) sb_puts(out, ",");
            sb_puts(out, "{\"path\":\"");
            sb_json(out, v->root, strlen(v->root));
            sb_puts(out, "\",\"kind\":\"accessible\"}");
            continue;
        }
        uint32_t rootid = v->dir_ready ? v->root_id : XL_ROOT;
        if (rootid == XL_ROOT) continue;
        char p[4096];
        for (int pass = 0; pass < 2; pass++) {
            for (uint32_t i = v->dir_head[rootid]; i != UINT32_MAX && shown < 200; i = v->dir_next[i]) {
                const xl_rec *r = &v->sn.recs[i];
                if (!(r->flags & XL_F_DIR)) continue;
                snap_path(&v->sn, i, p, sizeof p);
                {
                    char probe[4200];
                    const char *bn = strrchr(p, '/');
                    bn = bn ? bn + 1 : p;
                    if (*bn == '.' || *bn == '@') continue;
                    if (sysnoise_name(bn, strlen(bn))) continue;
                    snprintf(probe, sizeof probe, "%s/", p);
                    if (system_path(probe)) continue;
                }
                int ok = perm_dir_listable((uid_t)uid, p);
                if (pass == 0 && !ok) continue;
                if (pass == 1 && ok) continue;
                if (pass == 1) {
                    /* 父目录不可见，再看下一层是否有显式授权 */
                    for (uint32_t j = v->dir_head[i]; j != UINT32_MAX && shown < 200; j = v->dir_next[j]) {
                        const xl_rec *c = &v->sn.recs[j];
                        if (!(c->flags & XL_F_DIR)) continue;
                        char cp[4096];
                        snap_path(&v->sn, j, cp, sizeof cp);
                        {
                            char probe[4200];
                            const char *bn2 = strrchr(cp, '/');
                            bn2 = bn2 ? bn2 + 1 : cp;
                            if (*bn2 == '.' || *bn2 == '@') continue;
                            if (sysnoise_name(bn2, strlen(bn2))) continue;
                            snprintf(probe, sizeof probe, "%s/", cp);
                            if (system_path(probe)) continue;
                        }
                        if (!perm_dir_listable((uid_t)uid, cp)) continue;
                        if (shown++) sb_puts(out, ",");
                        sb_puts(out, "{\"path\":\"");
                        sb_json(out, cp, strlen(cp));
                        sb_puts(out, "\",\"kind\":\"granted\"}");
                    }
                    continue;
                }
                if (shown++) sb_puts(out, ",");
                sb_puts(out, "{\"path\":\"");
                sb_json(out, p, strlen(p));
                sb_puts(out, "\",\"kind\":\"accessible\"}");
            }
        }
    }
    pthread_rwlock_unlock(&g_rw);
    sb_puts(out, "]}");
}

static void handle_me(sbuf *out, int uid, const char *via, const char *hdr_name, int hdr_admin) {
    char name[128] = "";
    if (hdr_name && *hdr_name) {
        snprintf(name, sizeof name, "%s", hdr_name);
    } else if (uid >= 0) {
        struct passwd pw, *res = NULL;
        char buf[4096];
        if (getpwuid_r((uid_t)uid, &pw, buf, sizeof(buf), &res) == 0 && res)
            snprintf(name, sizeof(name), "%s", res->pw_name);
    }
    sb_printf(out, "{\"uid\":%d,\"name\":\"", uid);
    sb_json(out, name, strlen(name));
    sb_printf(out, "\",\"filtered\":%s,\"admin\":%s,\"via\":\"", uid >= 0 ? "true" : "false",
              (hdr_admin || uid == 0) ? "true" : "false");
    sb_puts(out, via ? via : "none");
    sb_printf(out, "\"}");
}

static int g_have_ffmpeg;
static int g_have_nvenc;
static int g_have_zscale;
static int g_tc_active;
static pthread_mutex_t g_tc_mu = PTHREAD_MUTEX_INITIALIZER;
static void handle_transcode(int fd, const char *qs, int uid);
static void handle_thumb(sbuf *out, const char *qs, int uid, int *code, const char **ctype);

static void handle_status(sbuf *out) {
    uint64_t total = 0;
    int cur_vol = -1;
    char cur_root[256] = "";
    sb_puts(out, "{\"state\":\"");
    if (g_building) sb_puts(out, "building");
    else if (g_failed) sb_puts(out, "degraded");
    else sb_puts(out, "ready");
    sb_puts(out, "\",\"index_dir\":\"");
    sb_json(out, g_idx_dir, strlen(g_idx_dir));
    sb_puts(out, "\",\"volumes\":[");
    pthread_rwlock_rdlock(&g_rw);
    cur_vol = g_cur_vol;
    if (cur_vol >= 0 && cur_vol < g_nvols)
        snprintf(cur_root, sizeof cur_root, "%s", g_vols[cur_vol].root);
    for (int i = 0; i < g_nvols; i++) {
        vol_t *v = &g_vols[i];
        total += v->records;
        if (i) sb_puts(out, ",");
        sb_puts(out, "{\"root\":\"");
        sb_json(out, v->root, strlen(v->root));
        sb_printf(out, "\",\"ok\":%s,\"records\":%llu,\"built_at\":%llu,\"build_ms\":%llu,\"building\":%s,\"usb\":%s}",
                  v->ok ? "true" : "false",
                  (unsigned long long)v->records,
                  (unsigned long long)v->built_at,
                  (unsigned long long)v->build_ms,
                  v->usb ? (v->building ? "true" : "false")
                         : ((g_building && cur_vol == i) ? "true" : "false"),
                  v->usb ? "true" : "false");
    }
    pthread_rwlock_unlock(&g_rw);
    sb_printf(out, "],\"records\":%llu,\"building\":%s,\"progress\":{\"root\":\"",
              (unsigned long long)total, g_building ? "true" : "false");
    if (g_building && cur_root[0]) sb_json(out, cur_root, strlen(cur_root));
    sb_printf(out, "\",\"records\":%llu,\"elapsed_ms\":%llu},\"error\":\"",
              (unsigned long long)xl_scan_records,
              (unsigned long long)(g_building ? now_ms() - g_build_start : g_build_ms));
    sb_json(out, g_err, strlen(g_err));
    size_t dels = 0;
    size_t adds = live_delta_count(&dels);
    sb_printf(out, "\",\"live\":{\"adds\":%zu,\"dels\":%zu,\"overflow\":%s}",
              adds, dels, live_overflowed() ? "true" : "false");
    sb_printf(out, ",\"transcode\":{\"ok\":%s,\"nvenc\":%s}",
              g_have_ffmpeg ? "true" : "false", g_have_nvenc ? "true" : "false");
    {
        int nusb = 0;
        pthread_rwlock_rdlock(&g_rw);
        for (int i = g_nvols_local; i < g_nvols; i++) if (g_vols[i].usb) nusb++;
        pthread_rwlock_unlock(&g_rw);
        sb_printf(out, ",\"usb\":{\"enabled\":%s,\"watch\":%s,\"mounts\":%d,\"message\":\"",
                  g_usb_enabled ? "true" : "false", g_usb_watch ? "true" : "false", nusb);
        char umsg[1024]; usb_msg_get(umsg, sizeof umsg); sb_json(out, umsg, strlen(umsg));
        sb_puts(out, "\"}");
    }
    sb_puts(out, ",\"version\":\"");
    sb_puts(out, XL_APP_VERSION);
    sb_puts(out, "\"}");
}

/* ---------- HTTP ---------- */
static const char *mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".js")) return "text/javascript; charset=utf-8";
    if (!strcmp(dot, ".css")) return "text/css; charset=utf-8";
    if (!strcmp(dot, ".mjs") || !strcmp(dot, ".js")) return "text/javascript; charset=utf-8";
    if (!strcmp(dot, ".json")) return "application/json; charset=utf-8";
    if (!strcmp(dot, ".png")) return "image/png";
    if (!strcmp(dot, ".svg")) return "image/svg+xml";
    if (!strcmp(dot, ".ico")) return "image/x-icon";
    return "application/octet-stream";
}

static const char *g_ui_dir;

static int serve_file(sbuf *out, const char *rel, int *code) {
    if (strstr(rel, "..")) { *code = 400; return 0; }
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", g_ui_dir, rel);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { *code = 404; return 0; }
    struct stat sb;
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode)) { close(fd); *code = 404; return 0; }
    char *buf = xmalloc((size_t)sb.st_size + 1);
    size_t got = 0;
    while (got < (size_t)sb.st_size) {
        ssize_t r = read(fd, buf + got, (size_t)sb.st_size - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    sb_add(out, buf, got);
    free(buf);
    *code = 200;
    return mime_for(path) ? 1 : 1;
}

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int header_one_uid(const char *req, const char *key) {
    const char *p = req;
    size_t klen = strlen(key);
    while ((p = strcasestr(p, key))) {
        if (p == req || p[-1] == '\n' || p[-1] == '\r') {
            p += klen;
            while (*p == ' ' || *p == '\t') p++;
            if (*p >= '0' && *p <= '9') return atoi(p);
            return -1;
        }
        p += klen;
    }
    return -1;
}

/* 身份优先级由调用方决定：网关头 -> 页面自报（同源 /cgi/user/info）-> 会话 Cookie */
static int header_uid(const char *req) {
    int u = header_one_uid(req, "x-nas-uid:");
    if (u >= 0) return u;
    /* 飞牛 CGI 注入的账号头 */
    u = header_one_uid(req, "x-trim-userid:");
    if (u >= 0) return u;
    return -1;
}

static int header_str(const char *req, const char *key, char *out, size_t cap) {
    const char *p = req;
    size_t klen = strlen(key);
    while ((p = strcasestr(p, key))) {
        if (p == req || p[-1] == '\n' || p[-1] == '\r') {
            p += klen;
            while (*p == ' ' || *p == '\t') p++;
            const char *e = p;
            while (*e && *e != '\r' && *e != '\n') e++;
            size_t n = (size_t)(e - p);
            if (n >= cap) n = cap - 1;
            memcpy(out, p, n);
            out[n] = 0;
            return 1;
        }
        p += klen;
    }
    return 0;
}

static int header_client_uid(const char *req) {
    int u = header_one_uid(req, "x-auth-uid:");
    if (u >= 0 && u < 65536) return u;
    return -1;
}


/* 从请求头里取出 Cookie 原文（网关会把浏览器的 Cookie 一并转发过来）。 */
static size_t header_cookie(const char *req, char *out, size_t cap) {
    const char *p = req;
    const char *key = "cookie:";
    size_t klen = strlen(key);
    while ((p = strcasestr(p, key))) {
        if (p == req || p[-1] == '\n' || p[-1] == '\r') {
            const char *v = p + klen;
            while (*v == ' ' || *v == '\t') v++;
            const char *e = strchr(v, '\n');
            size_t len = e ? (size_t)(e - v) : strlen(v);
            while (len && (v[len - 1] == '\r' || v[len - 1] == '\n')) len--;
            if (len >= cap) len = cap - 1;
            memcpy(out, v, len);
            out[len] = 0;
            return len;
        }
        p += klen;
    }
    return 0;
}

/* 用飞牛会话 Cookie 向 trim_http_cgi 换取 uid，换取不到返回 -1。
 * 这是网关注入 x-nas-uid 之外的兜底路径：同一份 Cookie 直接回放给 CGI。 */

/* 会话 Cookie → uid 的小缓存：避免每次按键都打一次 CGI。 */
static pthread_mutex_t g_idm = PTHREAD_MUTEX_INITIALIZER;
static struct { char cookie[512]; int uid; time_t exp; } g_idc[8];

static int id_cache_get(const char *cookie) {
    int uid = -2;
    time_t now = time(NULL);
    pthread_mutex_lock(&g_idm);
    for (int i = 0; i < 8; i++)
        if (g_idc[i].exp > now && !strncmp(g_idc[i].cookie, cookie, sizeof g_idc[i].cookie - 1)) {
            uid = g_idc[i].uid;
            break;
        }
    pthread_mutex_unlock(&g_idm);
    return uid;
}

static void id_cache_put(const char *cookie, int uid) {
    time_t now = time(NULL);
    int slot = 0;
    pthread_mutex_lock(&g_idm);
    for (int i = 0; i < 8; i++) {
        if (g_idc[i].exp <= now) { slot = i; break; }
        if (g_idc[i].exp < g_idc[slot].exp) slot = i;
    }
    size_t cl = strlen(cookie);
    if (cl >= sizeof g_idc[slot].cookie) cl = sizeof g_idc[slot].cookie - 1;
    memcpy(g_idc[slot].cookie, cookie, cl);
    g_idc[slot].cookie[cl] = 0;
    g_idc[slot].uid = uid;
    g_idc[slot].exp = now + 60;
    pthread_mutex_unlock(&g_idm);
}

static int cgi_uid_from_cookie(const char *cookie) {
    const char *sock = getenv("XL_CGI_SOCK");
    if (!sock || !*sock) sock = "/var/run/trim_http_cgi.socket";
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    char req[8192];
    int n = snprintf(req, sizeof req,
        "GET /cgi/user/info HTTP/1.0\r\nHost: localhost\r\nAccept-Encoding: identity\r\n"
        "Connection: close\r\nCookie: %.4000s\r\n\r\n", cookie);
    if (n <= 0 || write(fd, req, (size_t)n) != n) { close(fd); return -1; }
    char buf[65536];
    size_t got = 0;
    ssize_t r;
    while (got + 1 < sizeof buf && (r = read(fd, buf + got, sizeof buf - 1 - got)) > 0)
        got += (size_t)r;
    close(fd);
    buf[got] = 0;
    char *body = strstr(buf, "\r\n\r\n");
    if (!body) return -1;
    body += 4;
    if (strstr(body, "invalid token")) return -1;
    char *p = strstr(body, "\"uid\"");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '"') p++;
    if (*p < '0' || *p > '9') return -1;
    return atoi(p);
}


/* 诊断用：只打印头名，不打印值，且每进程仅一次 */
static void dump_header_names_once(const char *req) {
    if (!getenv("XL_DEBUG_HEADERS")) return;
    static int done = 0;
    if (done) return;
    done = 1;
    const char *p = strstr(req, "\r\n");
    while (p) {
        p += 2;
        const char *e = strstr(p, "\r\n");
        if (!e || e == p) break;
        const char *colon = memchr(p, ':', (size_t)(e - p));
        if (colon) fprintf(stderr, "xuanlan hdr: %.*s\n", (int)(colon - p), p);
        p = e;
    }
}

static uid_t g_www_uid = (uid_t)-1;
static gid_t g_www_gid = (gid_t)-1;

static void handle_client(int fd) {
    int is_unix = 0;
    /* 空闲连接不能永久占住 worker */
    {
        struct timeval tv;
        tv.tv_sec = 5; tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        tv.tv_sec = 60;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    /* 仅允许 root 或 www-data（网关 CGI）连接 unix socket，避免本地用户伪造身份头 */
    {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0 && ss.ss_family == AF_UNIX) {
            is_unix = 1;
            struct ucred cr;
            socklen_t cl = sizeof cr;
            int peer_ok = 0;
            if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) == 0) {
                peer_ok = (cr.uid == 0) || (cr.uid == geteuid()) ||
                          (g_www_uid != (uid_t)-1 && cr.uid == g_www_uid);
            }
            if (!peer_ok) {
                const char *body = "{\"error\":\"peer not allowed\"}";
                char hdr[256];
                int n = snprintf(hdr, sizeof hdr,
                    "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n"
                    "Content-Length: %zu\r\nConnection: close\r\n\r\n", strlen(body));
                write(fd, hdr, (size_t)n);
                write(fd, body, strlen(body));
                close(fd);
                return;
            }
        }
    }
    g_extra_hdr[0] = 0;
    g_range_hdr[0] = 0;
    g_no_store = 1;
    g_req_admin = -1;
    g_fs_fd = -1;

    /* Accept-Ranges 之外的响应不带 Range/Cache 头 */

    char req[REQ_MAX];
    size_t n = 0;
    uint64_t rdl = now_ms() + 8000;   /* 防止 slowloris 一点点喂头占死 worker */
    while (n + 1 < sizeof(req)) {
        if (now_ms() > rdl) break;
        ssize_t r = recv(fd, req + n, sizeof(req) - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (!n) { close(fd); return; }
    req[n] = 0;
    if (!strstr(req, "\r\n\r\n")) {
        send_simple(fd, n + 1 >= sizeof(req) ? 431 : 400,
                    n + 1 >= sizeof(req) ? "{\"error\":\"headers too large\"}"
                                         : "{\"error\":\"bad request\"}");
        close(fd);
        return;
    }

    /* 头部可能先到、正文后到：按 Content-Length 继续读；chunked 则读到结束块 */
    size_t hdr_end = 0;
    int chunked_body = 0;
    {
        char *he = strstr(req, "\r\n\r\n");
        if (he) {
            hdr_end = (size_t)(he - req) + 4;
            size_t clen = 0;
            int chunked = 0;
            for (char *ln = req; ln < he && *ln; ) {
                char *eol = strstr(ln, "\r\n");
                size_t ll = eol ? (size_t)(eol - ln) : strlen(ln);
                if (ll > 15 && !strncasecmp(ln, "content-length:", 15)) {
                    clen = (size_t)strtoul(ln + 15, NULL, 10);
                } else if (ll > 17 && !strncasecmp(ln, "transfer-encoding:", 18)) {
                    if (memmem(ln, ll, "chunked", 7)) chunked = 1;
                }
                if (!eol) break;
                ln = eol + 2;
            }
            chunked_body = chunked;
            if (chunked) {
                while (n + 1 < sizeof(req) && !memmem(req + hdr_end, n - hdr_end, "\r\n0\r\n", 5)) {
                    if (now_ms() > rdl) break;
                    ssize_t r = recv(fd, req + n, sizeof(req) - 1 - n, 0);
                    if (r <= 0) break;
                    n += (size_t)r;
                    req[n] = 0;
                }
                if (n + 1 >= sizeof(req) && !memmem(req + hdr_end, n - hdr_end, "\r\n0\r\n", 5)) {
                    send_simple(fd, 413, "{\"error\":\"request too large\"}");
                    close(fd);
                    return;
                }
            } else if (clen) {
                if (clen > sizeof(req) - 1 - hdr_end) {
                    send_simple(fd, 413, "{\"error\":\"request too large\"}");
                    close(fd);
                    return;
                }
                size_t need = hdr_end + clen;
                while (n < need && n + 1 < sizeof(req)) {
                    if (now_ms() > rdl) break;
                    ssize_t r = recv(fd, req + n, sizeof(req) - 1 - n, 0);
                    if (r <= 0) break;
                    n += (size_t)r;
                    req[n] = 0;
                }
            }
        }
    }

    char method[8] = "", target[4096] = "";
    if (sscanf(req, "%7s %4095s", method, target) != 2) {
        close(fd);
        return;
    }

    /* 剥离网关前缀 */
    const char *path = target;
    if (!strncmp(path, "/app/xuanlan", 12)) path += 12;
    if (!*path) path = "/";
    char *qs = strchr(path, '?');
    char pathonly[4096];
    if (qs) {
        size_t pl = (size_t)(qs - path);
        if (pl >= sizeof(pathonly)) pl = sizeof(pathonly) - 1;
        memcpy(pathonly, path, pl);
        pathonly[pl] = 0;
        qs++;
    } else {
        snprintf(pathonly, sizeof(pathonly), "%s", path);
        qs = "";
    }

    sbuf body;
    memset(&body, 0, sizeof(body));
    int code = 200;
    const char *ctype = "application/json; charset=utf-8";

    static _Thread_local char bodybuf[REQ_MAX];
    const char *req_body = strstr(req, "\r\n\r\n");
    req_body = req_body ? req_body + 4 : "";
    if (chunked_body && hdr_end && hdr_end < n) {
        size_t o = 0;
        const char *q = req + hdr_end, *end = req + n;
        while (q < end) {
            char *e = NULL;
            unsigned long sz = strtoul(q, &e, 16);
            if (!e || e == q || e + 2 > end) break;
            q = e + 2;
            if (sz == 0) break;
            if (q + sz > end) sz = (size_t)(end - q);
            if (o + sz >= sizeof(bodybuf)) {
                send_simple(fd, 413, "{\"error\":\"request too large\"}");
                close(fd);
                return;
            }
            memcpy(bodybuf + o, q, sz);
            o += sz;
            q += sz;
            if (q + 2 <= end && q[0] == '\r' && q[1] == '\n') q += 2;
        }
        bodybuf[o] = 0;
        req_body = bodybuf;
    }
    dump_header_names_once(req);
    char hdr_user[128] = "";
    int hdr_admin = 0, hdr_admin_present = 0;
    int uid = -1;
    const char *uid_src = NULL;
    /* 只有 unix socket（网关/root）才可信身份头；TCP 回环任何本地用户都能伪造 */
    if (is_unix) {
        header_str(req, "x-trim-username:", hdr_user, sizeof hdr_user);
        {
            char a[16] = "";
            int has = header_str(req, "x-trim-isadmin:", a, sizeof a);
            if (has) {
                hdr_admin_present = 1;
                hdr_admin = (a[0] == '1' || a[0] == 't' || a[0] == 'T' || a[0] == 'y' || a[0] == 'Y');
            }
        }
        uid = header_uid(req);
        uid_src = uid >= 0 ? "trim" : NULL;
        if (uid < 0) {
            int cu = header_client_uid(req);
            if (cu >= 0) { uid = cu; uid_src = "client"; }
        }
    }
    if (uid < 0) {
        char cookie[4096];
        if (header_cookie(req, cookie, sizeof cookie) > 0) {
            int cu = id_cache_get(cookie);
            if (cu == -2) {
                cu = cgi_uid_from_cookie(cookie);
                if (cu >= 0) id_cache_put(cookie, cu);
            }
            if (cu >= 0) { uid = cu; uid_src = "cookie"; }
            else uid_src = "cookie-invalid";
        }
    }
    if (uid < 0 && is_unix) uid = g_acl_uid;
    int admin = (uid == 0);
    if (is_unix && !admin) {
        if (hdr_admin_present) admin = hdr_admin;
        else admin = admin_fallback_enabled() && uid >= 0 && admin_by_uid(uid);
    }
    g_req_admin = admin;
    char ct[128] = "";
    header_str(req, "content-type:", ct, sizeof ct);
    int json_ct = strcasestr(ct, "application/json") != NULL;
    char tmpall[16];
    memset(tmpall, 0, sizeof tmpall);
    {
        static int last_uid = -2;
        if (uid != last_uid) {
            fprintf(stderr, "xuanlan serve: 请求身份 uid=%d (%s) net=%s\n", uid,
                    uid < 0 ? (uid_src ? uid_src : "无身份头") : uid_src,
                    is_unix ? "unix" : "tcp");
            last_uid = uid;
        }
    }
    if (!strcmp(pathonly, "/api/settings") || (!strcmp(pathonly, "/api/dir") && strstr(qs, "all="))) {
        fprintf(stderr, "xuanlan serve: %s admin=%d qs=%s\n", pathonly, admin, qs);
    }

    /* fail-closed：/api 下没有可信身份一律 401（静态 UI 与 /healthz 除外） */
    if (!strncmp(pathonly, "/api/", 5) && uid < 0) {
        code = 401;
        sb_puts(&body, "{\"error\":\"authentication required\"}");
    } else if (!strcmp(pathonly, "/api/search")) {
        handle_search(&body, qs, uid, admin);
    } else if (!strcmp(pathonly, "/api/me")) {
        handle_me(&body, uid, uid_src ? uid_src : (uid >= 0 ? "env" : "none"), hdr_user, admin);
    } else if (!strcmp(pathonly, "/api/status")) {
        handle_status(&body);
    } else if (!strcmp(pathonly, "/api/rebuild")) {
        if (!admin) {
            code = 403;
            sb_puts(&body, "{\"error\":\"需要管理员权限\"}");
        } else if (strcmp(method, "POST") != 0) {
            code = 405;
            sb_puts(&body, "{\"error\":\"use POST\"}");
        } else if (!json_ct) {
            code = 415;
            sb_puts(&body, "{\"error\":\"use application/json\"}");
        } else {
            if (req_body && strstr(req_body, "\"usb\":true")) usb_rebuild_all();
            else start_build();
            sb_puts(&body, "{\"ok\":true}");
        }
    } else if (!strcmp(pathonly, "/api/clientlog")) {
        char msg[1024];
        if (param(qs, "msg", msg, sizeof msg))
            fprintf(stderr, "xuanlan client: %.900s\n", msg);
        sb_puts(&body, "{\"ok\":true}");
    } else if (!strcmp(pathonly, "/api/settings")) {
        if (strcmp(method, "POST") == 0) {
            if (!json_ct) { code = 415; sb_puts(&body, "{\"error\":\"use application/json\"}"); }
            else handle_settings_post(&body, req_body, uid, admin);
        } else {
            handle_settings_get(&body, uid, admin, param(qs, "all", tmpall, sizeof tmpall));
        }
    } else if (!strcmp(pathonly, "/api/acl")) {
        handle_acl_list(&body, uid, admin, param(qs, "all", tmpall, sizeof tmpall));
    } else if (!strcmp(pathonly, "/api/dir")) {
        handle_dir(&body, qs, uid, admin);
    } else if (!strcmp(pathonly, "/api/fsop")) {
        if (strcmp(method, "POST") != 0) { code = 405; sb_puts(&body, "{\"error\":\"use POST\"}"); }
        else if (!json_ct) { code = 415; sb_puts(&body, "{\"error\":\"use application/json\"}"); }
        else handle_fsop(&body, req_body, uid);
    } else if (!strcmp(pathonly, "/api/trash")) {
        if (strcmp(method, "POST") == 0) {
            if (!json_ct) { code = 415; sb_puts(&body, "{\"error\":\"use application/json\"}"); }
            else handle_trash_post(&body, req_body, uid, admin);
        } else {
            handle_trash_get(&body, uid);
        }
    } else if (!strcmp(pathonly, "/api/raw")) {
        handle_raw(&body, qs, uid, &ctype);
    } else if (!strcmp(pathonly, "/api/text")) {
        handle_text(&body, qs, uid, &ctype);
    } else if (!strcmp(pathonly, "/api/markdown") || !strcmp(pathonly, "/api/md")) {
        handle_markdown(&body, qs, uid, &ctype);
    } else if (!strcmp(pathonly, "/api/probe")) {
        handle_probe(&body, qs, uid);
    } else if (!strcmp(pathonly, "/api/transcode")) {
        handle_transcode(fd, qs, uid);
        free(body.p);
        close(fd);
        return;
    } else if (!strcmp(pathonly, "/api/thumb")) {
        handle_thumb(&body, qs, uid, &code, &ctype);
    } else if (!strcmp(pathonly, "/api/file")) {
        handle_file(&body, req, qs, uid, &code, &ctype);
    } else if (!strcmp(pathonly, "/healthz")) {
        ctype = "text/plain; charset=utf-8";
        sb_puts(&body, "ok");
    } else if (!strcmp(pathonly, "/") || !strcmp(pathonly, "/index.html")) {
        ctype = "text/html; charset=utf-8";
        if (!serve_file(&body, "index.html", &code)) {
            sb_puts(&body, "玄览 UI 缺失");
            ctype = "text/plain; charset=utf-8";
        }
    } else if (!strncmp(pathonly, "/vendor/", 8)) {
        if (!serve_file(&body, pathonly + 1, &code)) {
            ctype = "text/plain; charset=utf-8";
            sb_puts(&body, "not found");
        } else {
            ctype = mime_for(pathonly);
        }
    } else if (!strcmp(pathonly, "/favicon.ico")) {
        code = 204;
        ctype = "image/x-icon";
    } else {
        code = 404;
        ctype = "text/plain; charset=utf-8";
        sb_puts(&body, "not found");
    }

    if (g_fs_fd >= 0) {
        sbuf resp;
        memset(&resp, 0, sizeof(resp));
        sb_printf(&resp, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
                         "%s%s%sConnection: close\r\n\r\n",
                  g_fs_status, g_fs_status == 206 ? "Partial Content" : "OK",
                  ctype, (long long)g_fs_len, g_range_hdr, g_extra_hdr,
                  g_no_store ? "Cache-Control: no-store\r\n" : "");
        write_all(fd, resp.p, resp.n);
        free(resp.p);
        char *chunk = malloc(STREAM_CHUNK);
        if (chunk) {
            off_t pos = g_fs_off, left = g_fs_len;
            while (left > 0) {
                size_t want = left > (off_t)STREAM_CHUNK ? STREAM_CHUNK : (size_t)left;
                ssize_t r = pread(g_fs_fd, chunk, want, pos);
                if (r <= 0) break;
                if (write_all(fd, chunk, (size_t)r) != 0) break;
                pos += r; left -= r;
            }
            free(chunk);
        }
        close(g_fs_fd);
        g_fs_fd = -1;
        free(body.p);
        close(fd);
        return;
    }

    sbuf resp;
    memset(&resp, 0, sizeof(resp));
    sb_printf(&resp, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "%s%s%sConnection: close\r\n\r\n",
              code, code == 200 ? "OK" : (code == 404 ? "Not Found" : "Error"),
              ctype, body.n, g_range_hdr, g_extra_hdr,
              g_no_store ? "Cache-Control: no-store\r\n" : "");
    if (body.n) sb_add(&resp, body.p, body.n);
    write_all(fd, resp.p, resp.n);
    free(resp.p);
    free(body.p);
    close(fd);
}

/* ---------- 服务端转码（4K/HEVC → 1080p H.264，fMP4 边转边播） ---------- */
static int g_have_ffmpeg;
static int g_have_nvenc;
static int g_have_zscale;

static const char *ffmpeg_path(void) {
    static const char *cand[] = { "/usr/bin/ffmpeg", "/bin/ffmpeg", "/usr/local/bin/ffmpeg" };
    for (size_t i = 0; i < sizeof cand / sizeof cand[0]; i++)
        if (access(cand[i], X_OK) == 0) return cand[i];
    return NULL;
}

static void detect_transcode(void) {
    const char *p = ffmpeg_path();
    if (!p) return;
    g_have_ffmpeg = 1;
    char buf[8192];
    char *argv[] = { (char *)p, "-hide_banner", "-encoders", NULL };
    if (run_capture(argv, buf, sizeof buf) == 0 && strstr(buf, "h264_nvenc")) g_have_nvenc = 1;
    char fbuf[65536];
    char *fargv[] = { (char *)p, "-hide_banner", "-filters", NULL };
    if (run_capture(fargv, fbuf, sizeof fbuf) == 0 && strstr(fbuf, "zscale")) g_have_zscale = 1;
}

/* 图片缩略图：ffmpeg 生成 160px JPEG 并按 路径+mtime+size 缓存，供预览底部缩略图条使用 */
static void handle_thumb(sbuf *out, const char *qs, int uid, int *code, const char **ctype) {
    char raw[4096], sw[16] = "";
    *code = 400;
    *ctype = "application/json; charset=utf-8";
    if (!param(qs, "path", raw, sizeof raw) || strncmp(raw, "/vol", 4) || strstr(raw, "..")) {
        sb_puts(out, "{\"error\":\"bad path\"}");
        return;
    }
    int w = 160;
    if (param(qs, "w", sw, sizeof sw)) {
        int v = atoi(sw);
        if (v >= 64 && v <= 320) w = v;
    }
    char real[PATH_MAX];
    if (!realpath(raw, real)) { *code = 404; sb_puts(out, "{\"error\":\"not found\"}"); return; }
    if (strncmp(real, "/vol", 4) || usb_path_blocked(uid, real)) {
        *code = 403; sb_puts(out, "{\"error\":\"forbidden\"}"); return;
    }
    if (uid >= 0) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", real);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = 0;
            if (!perm_dir_listable((uid_t)uid, dir)) { *code = 403; sb_puts(out, "{\"error\":\"forbidden\"}"); return; }
        }
        if (!perm_file_readable((uid_t)uid, real)) { *code = 403; sb_puts(out, "{\"error\":\"forbidden\"}"); return; }
    }
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISREG(st.st_mode)) { *code = 404; sb_puts(out, "{\"error\":\"not found\"}"); return; }

    char seed[PATH_MAX + 64];
    snprintf(seed, sizeof seed, "%s|%lld|%lld|%d", real, (long long)st.st_size, (long long)st.st_mtime, w);
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)seed; *p; p++) { h ^= *p; h *= 1099511628211ull; }
    char tdir[PATH_MAX], cache[PATH_MAX + 80];
    snprintf(tdir, sizeof tdir, "%s/thumbs", g_idx_dir);
    mkdir(tdir, 0700);
    snprintf(cache, sizeof cache, "%s/%016llx.jpg", tdir, (unsigned long long)h);

    int fd = open(cache, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        struct stat cs;
        if (fstat(fd, &cs) == 0 && cs.st_size > 0 && cs.st_size < (2 << 20)) {
            char *buf = xmalloc((size_t)cs.st_size);
            ssize_t n = read(fd, buf, (size_t)cs.st_size);
            close(fd);
            if (n == cs.st_size) {
                sb_add(out, buf, (size_t)n);
                free(buf);
                *code = 200;
                *ctype = "image/jpeg";
                snprintf(g_extra_hdr, sizeof g_extra_hdr, "Cache-Control: private, max-age=86400\r\n");
                g_no_store = 0;
                return;
            }
            free(buf);
        } else close(fd);
    }
    const char *ff = ffmpeg_path();
    if (!g_have_ffmpeg || !ff) { *code = 404; sb_puts(out, "{\"error\":\"thumbnail unavailable\"}"); return; }

    char tmp[PATH_MAX + 96];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d.%lu", cache, (int)getpid(), (unsigned long)pthread_self());
    char vf[96];
    snprintf(vf, sizeof vf, "scale=%d:-2:flags=fast_bilinear", w);
    char *argv[] = {
        (char *)ff, "-nostdin", "-v", "error", "-y", "-threads", "1", "-i", real,
        "-vf", vf, "-frames:v", "1", "-c:v", "mjpeg", "-q:v", "5", "-f", "image2", tmp, NULL
    };
    char junk[256];
    int rc = run_capture(argv, junk, sizeof junk);
    if (rc != 0) { unlink(tmp); *code = 404; sb_puts(out, "{\"error\":\"thumbnail failed\"}"); return; }
    if (rename(tmp, cache) != 0) { unlink(tmp); *code = 500; sb_puts(out, "{\"error\":\"thumbnail cache failed\"}"); return; }
    fd = open(cache, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { *code = 500; sb_puts(out, "{\"error\":\"thumbnail read failed\"}"); return; }
    struct stat cs;
    if (fstat(fd, &cs) != 0 || cs.st_size <= 0 || cs.st_size >= (2 << 20)) {
        close(fd); *code = 500; sb_puts(out, "{\"error\":\"thumbnail invalid\"}"); return;
    }
    char *buf = xmalloc((size_t)cs.st_size);
    ssize_t n = read(fd, buf, (size_t)cs.st_size);
    close(fd);
    if (n != cs.st_size) { free(buf); *code = 500; sb_puts(out, "{\"error\":\"thumbnail read failed\"}"); return; }
    sb_add(out, buf, (size_t)n);
    free(buf);
    *code = 200;
    *ctype = "image/jpeg";
    snprintf(g_extra_hdr, sizeof g_extra_hdr, "Cache-Control: private, max-age=86400\r\n");
    g_no_store = 0;
}

static void tc_release(void) {
    pthread_mutex_lock(&g_tc_mu);
    if (g_tc_active > 0) g_tc_active--;
    pthread_mutex_unlock(&g_tc_mu);
}

static void send_simple(int fd, int code, const char *json) {
    char hdr[512];
    int hl = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        code, code == 200 ? "OK" : (code == 403 ? "Forbidden" : "Error"),
        strlen(json));
    write_all(fd, hdr, (size_t)hl);
    write_all(fd, json, strlen(json));
}

static void handle_transcode(int fd, const char *qs, int uid) {
    char raw[4096], qbuf[16], tbuf[32];
    if (!param(qs, "path", raw, sizeof raw)) { send_simple(fd, 400, "{\"error\":\"missing path\"}"); return; }
    if (strncmp(raw, "/vol", 4) || strstr(raw, "..")) { send_simple(fd, 403, "{\"error\":\"path not allowed\"}"); return; }
    char real[PATH_MAX];
    if (!realpath(raw, real) || strncmp(real, "/vol", 4)) { send_simple(fd, 404, "{\"error\":\"not found\"}"); return; }
    if (usb_path_blocked(uid, real)) { send_simple(fd, 403, "{\"error\":\"path not allowed\"}"); return; }
    struct stat tst;
    if (stat(real, &tst) != 0 || !S_ISREG(tst.st_mode)) { send_simple(fd, 403, "{\"error\":\"not a regular file\"}"); return; }
    if (uid >= 0) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", real);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = 0;
            if (!perm_dir_listable((uid_t)uid, dir)) { send_simple(fd, 403, "{\"error\":\"forbidden\"}"); return; }
        }
        if (!perm_file_readable((uid_t)uid, real)) { send_simple(fd, 403, "{\"error\":\"forbidden\"}"); return; }
    }
    if (!g_have_ffmpeg) { send_simple(fd, 503, "{\"error\":\"ffmpeg not available\"}"); return; }

    /* 每路转码都会长期占用一个 worker 和一个 ffmpeg，限制并发避免拖垮整个服务 */
    pthread_mutex_lock(&g_tc_mu);
    if (g_tc_active >= 2) { pthread_mutex_unlock(&g_tc_mu); send_simple(fd, 503, "{\"error\":\"too many transcodes\"}"); return; }
    g_tc_active++;
    pthread_mutex_unlock(&g_tc_mu);

    if (!param(qs, "q", qbuf, sizeof qbuf)) snprintf(qbuf, sizeof qbuf, "1080");
    int W = 1920, H = 1080, br = 4;
    if (!strcmp(qbuf, "720")) { W = 1280; H = 720; br = 3; }
    else if (!strcmp(qbuf, "480")) { W = 854; H = 480; br = 2; }

    double start = 0;
    if (param(qs, "t", tbuf, sizeof tbuf)) start = atof(tbuf);
    if (!(start >= 0) || start > 1e7) start = 0;
    char ss[32];
    snprintf(ss, sizeof ss, "%.3f", start);

    char vc[32] = "", ac[32] = "", pix[40] = "", tr[40] = "";
    int vw = 0, vh = 0;
    int probed = media_probe(real, vc, sizeof vc, ac, sizeof ac, &vw, &vh, pix, sizeof pix, tr, sizeof tr);
    int has_video = probed ? vc[0] != 0 : 1;   /* ffprobe 缺失时按旧行为尝试视频 */
    int is_hdr = !strcmp(tr, "smpte2084") || !strcmp(tr, "arib-std-b67");
    if (probed && !has_video && !ac[0]) { tc_release(); send_simple(fd, 415, "{\"error\":\"no decodable stream\"}"); return; }
    char vf[512];
    if (has_video && is_hdr && g_have_zscale)
        snprintf(vf, sizeof vf,
                 "zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,"
                 "tonemap=tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv,"
                 "format=yuv420p,scale=%d:%d:force_original_aspect_ratio=decrease:force_divisible_by=2", W, H);
    else if (has_video)
        snprintf(vf, sizeof vf,
                 "scale=%d:%d:force_original_aspect_ratio=decrease:force_divisible_by=2,format=yuv420p", W, H);
    else
        vf[0] = 0;

    char vb[16], vmax[16], vbuf[16];
    snprintf(vb, sizeof vb, "%dM", br);
    snprintf(vmax, sizeof vmax, "%dM", br * 3 / 2);
    snprintf(vbuf, sizeof vbuf, "%dM", br * 2);
    char *argv[48];
    int n = 0;
    argv[n++] = (char *)ffmpeg_path();
    argv[n++] = "-hide_banner"; argv[n++] = "-loglevel"; argv[n++] = "error";
    argv[n++] = "-nostdin";
    if (start > 0) { argv[n++] = "-ss"; argv[n++] = ss; }
    argv[n++] = "-i"; argv[n++] = real;
    argv[n++] = "-t"; argv[n++] = "14400";   /* 单次最多 4 小时 */
    if (has_video) {
        argv[n++] = "-map"; argv[n++] = "0:v:0";
        argv[n++] = "-map"; argv[n++] = "0:a:0?";
        argv[n++] = "-vf"; argv[n++] = vf;
        if (g_have_nvenc) {
            argv[n++] = "-c:v"; argv[n++] = "h264_nvenc";
            argv[n++] = "-preset"; argv[n++] = "p4";
            argv[n++] = "-rc"; argv[n++] = "vbr";
            argv[n++] = "-b:v"; argv[n++] = vb;
            argv[n++] = "-maxrate"; argv[n++] = vmax;
            argv[n++] = "-bufsize"; argv[n++] = vbuf;
        } else {
            argv[n++] = "-c:v"; argv[n++] = "libx264";
            argv[n++] = "-preset"; argv[n++] = "veryfast";
            argv[n++] = "-b:v"; argv[n++] = vb;
        }
    } else {
        argv[n++] = "-map"; argv[n++] = "0:a:0";
    }
    argv[n++] = "-c:a"; argv[n++] = "aac";
    argv[n++] = "-b:a"; argv[n++] = "128k";
    argv[n++] = "-ac"; argv[n++] = "2";
    argv[n++] = "-movflags"; argv[n++] = "frag_keyframe+empty_moov+default_base_moof";
    argv[n++] = "-f"; argv[n++] = "mp4";
    argv[n++] = "pipe:1";
    argv[n] = NULL;

    int pfd[2];
    if (pipe(pfd) != 0) { tc_release(); send_simple(fd, 500, "{\"error\":\"pipe failed\"}"); return; }
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); tc_release(); send_simple(fd, 500, "{\"error\":\"fork failed\"}"); return; }
    if (pid == 0) {
        dup2(pfd[1], 1);
        close(pfd[0]); close(pfd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 2); close(devnull); }
        execv(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);

    char rhdr[512];
    int hl = snprintf(rhdr, sizeof rhdr,
        "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\n"
        "Transfer-Encoding: chunked\r\nAccept-Ranges: none\r\n"
        "Cache-Control: no-store\r\nX-Transcode: %s\r\nConnection: close\r\n\r\n",
        g_have_nvenc ? "nvenc" : "libx264");
    if (write_all(fd, rhdr, (size_t)hl) != 0) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); close(pfd[0]); tc_release(); return; }

    char *buf = malloc(64 * 1024);
    int ok = buf != NULL;
    while (ok) {
        ssize_t r = read(pfd[0], buf, 64 * 1024);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        char ch[32];
        int cl = snprintf(ch, sizeof ch, "%zx\r\n", (size_t)r);
        if (write_all(fd, ch, (size_t)cl) != 0 ||
            write_all(fd, buf, (size_t)r) != 0 ||
            write_all(fd, "\r\n", 2) != 0) { ok = 0; break; }
    }
    free(buf);
    if (!ok) kill(pid, SIGTERM);
    close(pfd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (ok) write_all(fd, "0\r\n\r\n", 5);
    tc_release();
}

/* ---------- 线程池 ---------- */
static int g_q[Q_CAP];
static int g_qh, g_qt, g_qn;
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_qcond = PTHREAD_COND_INITIALIZER;

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_qlock);
        while (!g_qn && !g_stop) pthread_cond_wait(&g_qcond, &g_qlock);
        if (g_stop && !g_qn) { pthread_mutex_unlock(&g_qlock); return NULL; }
        int fd = g_q[g_qh];
        g_qh = (g_qh + 1) % Q_CAP;
        g_qn--;
        pthread_mutex_unlock(&g_qlock);
        handle_client(fd);
    }
}

static void enqueue(int fd) {
    pthread_mutex_lock(&g_qlock);
    if (g_qn == Q_CAP) {
        pthread_mutex_unlock(&g_qlock);
        send_simple(fd, 503, "{\"error\":\"server busy\"}");
        close(fd);
        return;
    }
    g_q[g_qt] = fd;
    g_qt = (g_qt + 1) % Q_CAP;
    g_qn++;
    pthread_cond_signal(&g_qcond);
    pthread_mutex_unlock(&g_qlock);
}

static int listen_tcp(const char *addr, int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) { close(fd); return -1; }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, 128) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}


/* 端口是否真的在监听：socket 有实例但 TCP 不通时视为坏实例，允许接管 */
static int tcp_alive(int port) {
    if (port <= 0) return 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 1;
    struct timeval tv = { 0, 300000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}

static int probe_existing(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    struct timeval tv = { 0, 400000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) { close(fd); return 0; }
    memcpy(sa.sun_path, path, strlen(path) + 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return 0; }
    const char *req = "GET /healthz HTTP/1.0\r\n\r\n";
    if (write(fd, req, strlen(req)) < 0) { close(fd); return 0; }
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, " 200 ") != NULL;
}

static int listen_unix(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) { close(fd); return -1; }
    memcpy(sa.sun_path, path, strlen(path) + 1);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, 128) != 0) {
        close(fd);
        return -1;
    }
    chmod(path, 0660);
    if (g_www_gid != (gid_t)-1) chown(path, 0, g_www_gid);
    return fd;
}

static void on_signal(int sig) { (void)sig; g_stop = 1; }

int httpd_serve(const xl_svc_opts *o) {
    detect_transcode();
    {
        struct passwd *pw = getpwnam("www-data");
        if (pw) { g_www_uid = pw->pw_uid; g_www_gid = pw->pw_gid; }
    }
    /* 严格模式：默认开启；settings.json 里可关闭 */
    {
        char sp[1200];
        settings_path(sp, sizeof sp);
        FILE *f = fopen(sp, "r");
        if (f) {
            char b[8192];
            size_t n = fread(b, 1, sizeof b - 1, f);
            b[n] = 0;
            fclose(f);
            perm_set_strict_home(json_bool_def(b, "\"strictHome\"", 1));
        }
    }
    const char *acl_env = getenv("XL_ACL_UID");
    g_acl_uid = acl_env ? atoi(acl_env) : -1;
    g_ui_dir = o->ui_dir;
    snprintf(g_idx_dir, sizeof(g_idx_dir), "%s", o->idx_dir);
    snprintf(g_trash, sizeof(g_trash), "%s/trash", g_idx_dir);
    mkdir(g_trash, 0755);
    mkdir(o->idx_dir, 0755);
    if (o->sock_path[0] && probe_existing(o->sock_path)) {
        if (o->no_tcp || tcp_alive(o->port)) {
            fprintf(stderr, "xuanlan serve: 已有实例在 %s 上响应，本次退出\n", o->sock_path);
            return 0;
        }
        fprintf(stderr, "xuanlan serve: %s 有实例但端口 %d 无响应，接管\n", o->sock_path, o->port);
        unlink(o->sock_path);
    }

    /* 挂载点列表 */
    g_nvols = 0;
    char roots[1024];
    snprintf(roots, sizeof(roots), "%s", o->roots);
    char *save = NULL;
    for (char *r = strtok_r(roots, ",", &save); r && g_nvols < MAX_VOL; r = strtok_r(NULL, ",", &save)) {
        while (*r == ' ') r++;
        size_t rl = strlen(r);
        while (rl && r[rl - 1] == '/') r[--rl] = 0;
        if (!rl) continue;
        struct stat rst;
        if (stat(r, &rst) != 0 || !S_ISDIR(rst.st_mode)) {
            fprintf(stderr, "xuanlan serve: 跳过不存在的挂载点 %s\n", r);
            continue;
        }
        vol_t *v = &g_vols[g_nvols];
        memset(v, 0, sizeof(*v));
        char rp[PATH_MAX];
        const char *norm = realpath(r, rp) ? rp : r;
        size_t nl = strlen(norm);
        if (nl >= sizeof(v->root)) {
            fprintf(stderr, "xuanlan serve: 挂载点路径过长: %s\n", norm);
            continue;
        }
        memcpy(v->root, norm, nl + 1);
        const char *base = strrchr(norm, '/');
        base = base ? base + 1 : norm;
        if (snprintf(v->file, sizeof(v->file), "%s/%s.idx", o->idx_dir, base) >= (int)sizeof(v->file)) {
            fprintf(stderr, "xuanlan serve: 索引路径过长: %s\n", r);
            continue;
        }
        if (snapshot_open(v->file, &v->sn) == 0) {
            v->ok = 1;
            v->records = v->sn.h.rec_count;
            v->built_at = v->sn.h.built_at;
            dir_index_build(v);
        }
        g_nvols++;
    }
    g_nvols_local = g_nvols;
    if (!g_nvols) {
        fprintf(stderr, "xuanlan serve: 没有挂载点\n");
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    for (int i = 0; i < N_WORKERS; i++) {
        pthread_t th;
        pthread_create(&th, NULL, worker, NULL);
        pthread_detach(th);
    }

    int ls[2];
    int nls = 0;
    if (!o->no_tcp) {
        ls[nls] = listen_tcp(o->bind_addr, o->port);
        if (ls[nls] < 0) fprintf(stderr, "xuanlan serve: TCP %s:%d 绑定失败: %s\n",
                                 o->bind_addr, o->port, strerror(errno));
        else nls++;
    }
    if (o->sock_path[0]) {
        ls[nls] = listen_unix(o->sock_path);
        if (ls[nls] < 0) fprintf(stderr, "xuanlan serve: socket %s 失败: %s\n",
                                 o->sock_path, strerror(errno));
        else nls++;
    }
    if (!nls) return 1;
    fprintf(stderr, "xuanlan serve: %d 个监听, %d 个卷, 索引目录 %s\n", nls, g_nvols, o->idx_dir);

    g_last_rebuild = now_ms();
    rebuild_cfg_load();
    scan_cfg_load();
    usb_cfg_load();
    if (g_usb_enabled) usb_sync();
    if (live_start(o->roots, 1) == 0)
        fprintf(stderr, "xuanlan serve: fanotify 实时增量已启用\n");
    else
        fprintf(stderr, "xuanlan serve: fanotify 不可用（需 root/CAP_SYS_ADMIN），仅周期对账\n");

    int need_build = 0;
    for (int i = 0; i < g_nvols_local; i++) {
        if (!g_vols[i].ok) { need_build = 1; continue; }
        /* 旧快照（或切换 skipHidden 后）与当前扫描设置不一致时重建一次 */
        int built_skip = (g_vols[i].sn.h.flags & XL_HDR_SKIP_HIDDEN) != 0;
        if (built_skip != scan_skip_hidden()) need_build = 1;
        /* 旧版本快照没有杂项过滤，需要按新规则重建一次 */
        if (!(g_vols[i].sn.h.flags & XL_HDR_SCAN_RULES)) need_build = 1;
    }
    if (need_build) start_build();

    struct pollfd pfd[2];
    for (int i = 0; i < nls; i++) { pfd[i].fd = ls[i]; pfd[i].events = POLLIN; }

    while (!g_stop) {
        int pr = poll(pfd, (nfds_t)nls, 1000);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < nls; i++) {
            if (!(pfd[i].revents & POLLIN)) continue;
            int c = accept4(pfd[i].fd, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0) enqueue(c);
        }
        if (!g_building) {
            if (schedule_due()) {
                time_t tnow = time(NULL);
                struct tm tm;
                localtime_r(&tnow, &tm);
                g_last_run_yday = tm.tm_yday + tm.tm_year * 1000;
                fprintf(stderr, "xuanlan serve: 定时全盘扫描开始\n");
                start_build();
            } else if (live_overflowed()) {
                fprintf(stderr, "xuanlan serve: 增量溢出，触发整卷重建\n");
                start_build();
            }
        }
        usb_tick();
    }
    for (int i = 0; i < nls; i++) close(ls[i]);
    if (o->sock_path[0]) unlink(o->sock_path);
    pthread_mutex_lock(&g_qlock);
    pthread_cond_broadcast(&g_qcond);
    pthread_mutex_unlock(&g_qlock);
    return 0;
}
