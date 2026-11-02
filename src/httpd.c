/* 玄览 / Arcane Vision — 常驻服务
 * 每个挂载点一份快照；查询时各卷并行求交后按 mtime 归并。
 * 同时监听 TCP 端口与 unix socket（fnOS 应用网关的 app.sock）。
 */
#include "xl.h"
#include "fsops.h"
#include "version.h"

#include <arpa/inet.h>
#include <ctype.h>
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
#define N_WORKERS 4
#define Q_CAP     128
#define REQ_MAX   16384

/* ---------- 动态缓冲 ---------- */
typedef struct { char *p; size_t n, cap; } sbuf;

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
    uint64_t records;
    uint64_t built_at;
    uint64_t build_ms;
} vol_t;

static vol_t g_vols[MAX_VOL];
static int   g_nvols;
static pthread_rwlock_t g_rw = PTHREAD_RWLOCK_INITIALIZER;

static volatile sig_atomic_t g_stop;
/* 网关不一定转发 x-trim-isadmin，这里补一个兜底：
 * 飞牛把首个普通用户（最小的 uid>=1000）视为管理员。 */
static int admin_by_uid(int uid) {
    if (uid < 1000) return 0;
    static int cached_first = -2;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (cached_first == -2) {
        int first = -1;
        struct passwd *pw;
        setpwent();
        while ((pw = getpwent()) != NULL) {
            if ((int)pw->pw_uid >= 1000 && (first < 0 || (int)pw->pw_uid < first))
                first = (int)pw->pw_uid;
        }
        endpwent();
        cached_first = first;
    }
    pthread_mutex_unlock(&mu);
    return uid == cached_first;
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
    if (!no_raw && resolve_dev(v->root, dev, sizeof(dev)) && strncmp(dev, "/dev/", 5) == 0) {
        if (verbose) fprintf(stderr, "xuanlan: %s <- %s\n", v->root, dev);
        rc = fsreader_ext4_scan(dev, v->root, &st, verbose);
        if (rc < 0 || st.n < 2) {
            store_free(&st);
            store_init(&st);
            rc = scan_tree(v->root, &st, 0, verbose);
        }
    } else {
        rc = scan_tree(v->root, &st, 0, verbose);
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
    if (v->ok) snapshot_close(&v->sn);
    v->sn = ns;
    v->ok = 1;
    v->records = ns.h.rec_count;
    v->built_at = (uint64_t)time(NULL);
    v->build_ms = now_ms() - t0;
    pthread_rwlock_unlock(&g_rw);
    return 0;
}

static void *build_worker(void *arg) {
    (void)arg;
    g_building = 1;
    g_failed = 0;
    g_err[0] = 0;
    g_build_start = now_ms();
    live_clear();                      /* 基线即将重建，旧 delta 作废 */
    for (int i = 0; i < g_nvols && !g_stop; i++) {
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
    for (;;) {
        if (got + 1 >= cap) break;
        ssize_t r = read(pipefd[0], out + got, cap - 1 - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    out[got] = 0;
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* 从 ffprobe json 里取第一个指定类型的 codec_name（不引第三方 json 库，简单扫描） */
static void probe_stream_codec(const char *json, const char *type, char *out, size_t cap) {
    out[0] = 0;
    const char *p = json;
    while ((p = strstr(p, "\"codec_type\"")) != NULL) {
        const char *a = strchr(p + 12, '"');
        const char *b = a ? strchr(a + 1, '"') : NULL;
        if (!a || !b) break;
        /* codec_name 在同一对象的 codec_type 之前，向前找对象起点 */
        const char *ob = p;
        while (ob > json && *ob != '{') ob--;
        const char *cn = strstr(ob, "\"codec_name\"");
        if (cn && cn < p) {
            const char *ca = strchr(cn + 12, '"');
            const char *cb = ca ? strchr(ca + 1, '"') : NULL;
            if (ca && cb && (size_t)(cb - ca - 1) < cap) {
                memcpy(out, ca + 1, (size_t)(cb - ca - 1));
                out[cb - ca - 1] = 0;
            }
        }
        size_t tl = (size_t)(b - a - 1);
        if (tl == strlen(type) && !strncmp(a + 1, type, tl)) return;
        p = b;
    }
}

static int media_probe(const char *real, char *vcodec, size_t vcap, char *acodec, size_t acap) {
    static const char *paths[] = { "/usr/bin/ffprobe", "/bin/ffprobe", "/usr/local/bin/ffprobe" };
    char buf[8192];
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        if (access(paths[i], X_OK) != 0) continue;
        char *argv[] = { (char *)paths[i], "-v", "error", "-show_entries", "stream=codec_type,codec_name",
                         "-of", "json", (char *)real, NULL };
        if (run_capture(argv, buf, sizeof buf) == 0) {
            probe_stream_codec(buf, "video", vcodec, vcap);
            probe_stream_codec(buf, "audio", acodec, acap);
            return 1;
        }
        return 0;
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
    }
    char vc[32] = "", ac[32] = "";
    int ok = media_probe(real, vc, sizeof vc, ac, sizeof ac);
    sb_printf(out, "{\"ok\":%s,\"video\":\"%s\",\"audio\":\"%s\"}", ok ? "true" : "false", vc, ac);
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
    if (uid >= 0) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", real);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = 0;
            if (!perm_dir_listable((uid_t)uid, dir)) { *len_out = (size_t)-1; return NULL; }
        }
    }
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISREG(st.st_mode)) { *len_out = (size_t)-1; return NULL; }
    int fd = open(real, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { *len_out = (size_t)-1; return NULL; }
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

    sb_puts(out,
        "<!doctype html><meta charset=\"utf-8\"><meta name=\"color-scheme\" content=\"dark\">"
        "<title></title><style>"
        "html,body{margin:0;background:#0d1524;color:#dbe6f6;overscroll-behavior:contain}"
        "pre{margin:0;padding:18px 20px;font:13px/1.7 ui-monospace,Menlo,Consolas,monospace;"
        "white-space:pre-wrap;word-break:break-word;tab-size:4}"
        ".bar{position:sticky;top:0;background:#111c2e;border-bottom:1px solid #21314a;"
        "padding:6px 14px;font:12px/1.6 -apple-system,sans-serif;color:#8fa5c4}"
        "#dbtn{position:fixed;right:16px;bottom:16px;display:none;cursor:pointer;"
        "border:1px solid rgba(94,234,212,.45);background:rgba(20,50,54,.92);color:#9ff0e4;"
        "border-radius:999px;padding:8px 16px;font:13px/1 -apple-system,sans-serif;"
        "box-shadow:0 6px 20px rgba(0,0,0,.45)}"
        "#dbtn:hover{background:rgba(26,66,70,.95)}"
        "article{padding:24px 28px 60px;max-width:820px;margin:0 auto;"
        "font:15px/1.85 -apple-system,\"PingFang SC\",\"Microsoft YaHei\",sans-serif;color:#dbe6f6}"
        "article h1,article h2,article h3,article h4,article h5,article h6{line-height:1.4;margin:1.4em 0 .6em;color:#eaf1fb}"
        "article h1{font-size:24px}article h2{font-size:21px}article h3{font-size:18px}"
        "article h4{font-size:16px;color:#7fe6d6}article h5,article h6{font-size:15px}"
        "article p{margin:0 0 14px}article ul,article ol{margin:0 0 14px;padding-left:1.5em}"
        "article li{margin:4px 0}article blockquote{margin:0 0 14px;padding:8px 14px;"
        "border-left:3px solid #2a4a6b;color:#a9bad6;background:rgba(255,255,255,.02)}"
        "article img{max-width:100%;height:auto;border-radius:8px;margin:8px 0}"
        "article a{color:#7fd7ff}article hr{border:0;border-top:1px solid #21314a;margin:22px 0}"
        "article table{border-collapse:collapse;margin:0 0 14px;width:100%}"
        "article td,article th{border:1px solid #21314a;padding:6px 10px;font-size:14px}"
        "article pre,article code{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:13px}"
        "article pre{background:#111c2e;padding:12px 14px;border-radius:8px;overflow:auto}"
        "</style>");
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
    if (strncmp(real, "/vol", 4) || !strstr(real, "/")) {
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
    }
    int fd = open(real, O_RDONLY | O_CLOEXEC);
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
        snprintf(g_extra_hdr, sizeof g_extra_hdr,
                 "Content-Disposition: attachment; filename*=UTF-8''%s\r\n", enc);
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
    uint32_t limit = s_limit[0] ? (uint32_t)atoi(s_limit) : 50;
    uint32_t off = s_off[0] ? (uint32_t)atoi(s_off) : 0;
    if (limit == 0 || limit > 500) limit = 50;

    xl_query_opts o;
    memset(&o, 0, sizeof(o));
    o.max_results = 0;               /* 收集全部，再切片 */
    if (!strcmp(type, "dir")) o.dirs_only = 1;
    else if (!strcmp(type, "file")) o.files_only = 1;

    char (*sc)[512] = xmalloc(sizeof(char[512]) * 64);
    int nsc = -1;
    if (uid >= 0) nsc = scope_load(uid, sc, 64);

    uint64_t t0 = now_ms();
    ref_t *refs = NULL;
    size_t nref = 0, cref = 0;
    int trunc = 0;

    pthread_rwlock_rdlock(&g_rw);
    for (int i = 0; i < g_nvols; i++) {
        if (!g_vols[i].ok) continue;
        uint32_t n = 0;
        int t = 0;
        uint32_t *hits = query_hits(&g_vols[i].sn, q, &o, &n, &t, NULL);
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
    sb_printf(out, "\",\"total\":%zu,\"approx\":%s,\"truncated\":%s,\"offset\":%u,\"limit\":%u,\"ms\":%.1f,\"items\":[",
              nref + nlh, (live_dels || nlh || uid >= 0) ? "true" : "false",
              trunc ? "true" : "false", off, limit, (double)(now_ms() - t0));

    char path[8192];
    uint32_t shown = 0;
    size_t i = 0, j = 0, skipped = 0;
    while ((i < nref || j < nlh) && shown < limit) {
        int take_live;
        if (i >= nref) take_live = 1;
        else if (j >= nlh) take_live = 0;
        else take_live = lh[j].mtime > g_vols[refs[i].vol].sn.recs[refs[i].id].mtime;

        int is_dir; uint32_t size, mtime; const char *name; size_t nlen;
        if (take_live) {
            path[0] = 0;
            snprintf(path, sizeof(path), "%s", lh[j].path);
            const char *b = strrchr(path, '/');
            name = b ? b + 1 : path;
            nlen = strlen(name);
            is_dir = (lh[j].flags & XL_F_DIR) != 0;
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
            if (!perm_dir_listable((uid_t)uid, pd)) continue;
            if (nsc >= 0 && !scope_allows(sc, nsc, path)) continue;
        }
        {
            struct stat lst;
            if (stat(path, &lst) == 0) {
                if (!is_dir) { size = (uint32_t)lst.st_size; mtime = (uint32_t)lst.st_mtime; }
            }
        }
        if (skipped < off) { skipped++; continue; }
        if (shown) sb_puts(out, ",");
        sb_puts(out, "{\"path\":\"");
        sb_json(out, path, strlen(path));
        sb_puts(out, "\",\"name\":\"");
        sb_json(out, name, nlen);
        sb_printf(out, "\",\"dir\":%s,\"size\":%u,\"mtime\":%u,\"cat\":\"%s\"}",
                  is_dir ? "true" : "false", size, mtime,
                  is_dir ? "dir" : cat_key(cat_of_name(name, nlen)));
        shown++;
    }
    sb_puts(out, "]}");
    pthread_rwlock_unlock(&g_rw);
    for (size_t k = 0; k < nlh; k++) free(lh[k].path);
    free(lh);
    free(refs);
    free(sc);
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

static int snap_root_id(const xl_snap *sn, uint32_t *id_out) {
    for (uint32_t i = 0; i < (uint32_t)sn->h.rec_count; i++) {
        const xl_rec *r = &sn->recs[i];
        if (r->name_len == 0 && r->parent == XL_ROOT) { *id_out = i; return 1; }
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
            uint32_t did;
            int found = !strcmp(dir, v->root) ? snap_root_id(&v->sn, &did)
                                              : dir_find_id(&v->sn, dir, &did);
            if (!found) continue;
            for (uint32_t i = 0; i < (uint32_t)v->sn.h.rec_count && shown < MAX_ENT; i++) {
                const xl_rec *r = &v->sn.recs[i];
                if (r->parent != did) continue;
                int is_dir = (r->flags & XL_F_DIR) != 0;
                if (pass == 0 && !is_dir) continue;
                if (pass == 1 && is_dir) continue;
                total++;
                snap_path(&v->sn, i, p, sizeof p);
                if (live_is_deleted(p)) continue;
                if (uid >= 0 && is_dir) {
                    if (!all && sysnoise_name(v->sn.names + r->name_off, r->name_len)) continue;
                    if (!all && (system_path(p) || !perm_dir_listable((uid_t)uid, p))) continue;
                }
                uint32_t esz = r->size, emt = r->mtime;
                if (!is_dir) {
                    struct stat lst;
                    if (stat(p, &lst) == 0) {
                        if (lst.st_size > 0) esz = (uint32_t)lst.st_size;
                        if (lst.st_mtime > 0) emt = (uint32_t)lst.st_mtime;
                    }
                }
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
    sb_printf(out, "],\"count\":%d,\"truncated\":%s}", total, total > MAX_ENT ? "true" : "false");
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
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", paths[i]);
    fclose(f);
}

static int scope_allows(char (*paths)[512], int n, const char *p) {
    for (int i = 0; i < n; i++) {
        size_t l = strlen(paths[i]);
        if (l == 0) continue;
        if (strncmp(p, paths[i], l) == 0 && (p[l] == 0 || p[l] == '/'))
            return 1;
    }
    return 0;
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
    if (system_path(p)) { snprintf(err, cap, "系统目录不可操作"); return -1; }

    struct stat st;
    if (lstat(p, &st) != 0) { snprintf(err, cap, "路径不存在"); return -1; }
    if (want_dir && !S_ISDIR(st.st_mode)) { snprintf(err, cap, "目标不是目录"); return -1; }

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
    char op[16] = {0}, dest[4200] = {0};
    json_str(body, "\"op\"", op, sizeof op, "");
    json_str(body, "\"dest\"", dest, sizeof dest, "");

    static char paths[200][4200];
    int n = json_str_array(body, "\"paths\"", &paths[0][0], sizeof paths[0], 200);
    if (!n) { sb_puts(out, "{\"ok\":false,\"error\":\"未选择文件\"}"); return; }
    if (uid < 0) { sb_puts(out, "{\"ok\":false,\"error\":\"未识别用户\"}"); return; }

    int is_trash = !strcmp(op, "trash");
    if (!is_trash && strcmp(op, "copy") && strcmp(op, "move")) {
        sb_puts(out, "{\"ok\":false,\"error\":\"未知操作\"}"); return;
    }

    char derr[256] = {0};
    if (!is_trash) {
        if (!dest[0]) { sb_puts(out, "{\"ok\":false,\"error\":\"目标目录为空\"}"); return; }
        if (fs_guard(uid, dest, 1, derr, sizeof derr) != 0) {
            sb_printf(out, "{\"ok\":false,\"error\":\"目标：%s\"}", derr);
            return;
        }
    }

    int done = 0, failed = 0;
    sb_puts(out, "{\"ok\":true,\"failed\":[");
    for (int i = 0; i < n; i++) {
        char err[256] = {0};
        if (fs_guard(uid, paths[i], 0, err, sizeof err) != 0) {
            if (failed++) sb_puts(out, ",");
            sb_printf(out, "{\"path\":\"%s\",\"error\":\"%s\"}", paths[i], err);
            continue;
        }
        char name[512];
        base_of(paths[i], name, sizeof name);
        if (is_trash) {
            char id[64];
            if (fs_trash_path(paths[i], g_trash, id, sizeof id, err, sizeof err) == 0) done++;
            else {
                if (failed++) sb_puts(out, ",");
                sb_printf(out, "{\"path\":\"%s\",\"error\":\"%s\"}", paths[i], err);
            }
            continue;
        }
        char dst[4300];
        snprintf(dst, sizeof dst, "%s/%s", dest, name);
        struct stat st;
        if (lstat(dst, &st) == 0) {
            if (failed++) sb_puts(out, ",");
            sb_printf(out, "{\"path\":\"%s\",\"error\":\"目标已存在\"}", paths[i]);
            continue;
        }
        int rc = !strcmp(op, "copy") ? fs_copy_path(paths[i], dst, err, sizeof err)
                                       : fs_move_path(paths[i], dst, err, sizeof err);
        if (rc == 0) done++;
        else {
            if (failed++) sb_puts(out, ",");
            sb_printf(out, "{\"path\":\"%s\",\"error\":\"%s\"}", paths[i], err);
        }
    }
    sb_printf(out, "],\"done\":%d,\"failedCount\":%d}", done, failed);
}

static void handle_trash_get(sbuf *out, int uid)
{
    static fs_trash_item items[500];
    int n = fs_trash_scan(g_trash, items, 500);
    sb_puts(out, "{\"items\":[");
    for (int i = 0; i < n; i++) {
        if (i) sb_puts(out, ",");
        sb_printf(out, "{\"id\":\"%s\",\"name\":\"%s\",\"orig\":\"%s\",\"size\":%lld,\"mtime\":%lld,\"deleted\":%lld}",
                  items[i].id, items[i].name, items[i].orig, items[i].size, items[i].mtime, items[i].deleted);
    }
    sb_puts(out, "]}");
}

static void handle_trash_post(sbuf *out, const char *body, int uid)
{
    char op[16] = {0};
    json_str(body, "\"op\"", op, sizeof op, "");
    if (!strcmp(op, "empty")) {
        int removed = 0;
        char err[256] = {0};
        if (fs_empty_trash(g_trash, &removed, err, sizeof err) != 0) {
            sb_printf(out, "{\"ok\":false,\"error\":\"%s\"}", err);
            return;
        }
        sb_printf(out, "{\"ok\":true,\"removed\":%d}", removed);
        return;
    }
    if (!strcmp(op, "restore")) {
        static char ids[200][64];
        int n = json_str_array(body, "\"ids\"", &ids[0][0], sizeof ids[0], 200);
        int done = 0, failed = 0;
        sb_puts(out, "{\"ok\":true,\"failed\":[");
        for (int i = 0; i < n; i++) {
            char err[256] = {0};
            if (fs_restore(g_trash, ids[i], err, sizeof err) == 0) done++;
            else {
                if (failed++) sb_puts(out, ",");
                sb_printf(out, "{\"id\":\"%s\",\"error\":\"%s\"}", ids[i], err);
            }
        }
        sb_printf(out, "],\"done\":%d,\"failedCount\":%d}", done, failed);
        return;
    }
    sb_puts(out, "{\"ok\":false,\"error\":\"未知操作\"}");
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
    {
        char oi[16];
        json_str(buf ? buf : "", "\"openIn\"", oi, sizeof oi, "window");
        sb_printf(out, ",\"openIn\":\"%s\"", oi);
    }
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
    char (*sc)[512] = xmalloc(sizeof(char[512]) * 64);
    int sn = -1;
    if (uid >= 0) {
        sn = scope_load(uid, sc, 64);
        if (sn < 0) sn = scope_default(uid, sc, 64);
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

static void handle_settings_post(sbuf *out, const char *body, int uid) {
    const char *p = strstr(body, "\"openWith\"");
    if (!p) { sb_puts(out, "{\"ok\":false,\"error\":\"openWith required\"}"); return; }
    const char *b = strchr(p, '{');
    if (!b) { sb_puts(out, "{\"ok\":false,\"error\":\"bad json\"}"); return; }
    int depth = 0; const char *e = b;
    for (; *e; e++) {
        if (*e == '{') depth++;
        else if (*e == '}') { if (--depth == 0) { e++; break; } }
    }
    if (depth != 0) { sb_puts(out, "{\"ok\":false,\"error\":\"bad json\"}"); return; }
    size_t len = (size_t)(e - b);
    if (len > 8192) len = 8192;

    char open_in[16];
    json_str(body, "\"openIn\"", open_in, sizeof open_in, "window");
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

    char sp[1200];
    settings_path(sp, sizeof sp);
    FILE *f = fopen(sp, "w");
    if (!f) { sb_puts(out, "{\"ok\":false,\"error\":\"cannot write\"}"); return; }
    const char *m = rc.mode == 1 ? "daily" : (rc.mode == 2 ? "interval" : "off");
    fprintf(f, "{\"openWith\":%.*s,\"openIn\":\"%s\",\"rebuild\":{\"mode\":\"%s\",\"hour\":%d,\"minute\":%d,\"hours\":%d}}",
            (int)len, b, open_in, m, rc.hour, rc.minute, rc.hours);
    fclose(f);

    /* 检索范围：解析 "scope":["/vol1/1000", ...] */
    if (uid >= 0) {
        const char *sp = strstr(body, "\"scope\"");
        if (sp) {
            const char *lb = strchr(sp, '[');
            const char *rb = lb ? strchr(lb, ']') : NULL;
            if (lb && rb) {
                char (*sc)[512] = xmalloc(sizeof(char[512]) * 64);
                int n = 0;
                const char *q = lb;
                while (n < 64 && (q = strchr(q, '"')) != NULL && q < rb) {
                    q++;
                    const char *e2 = strchr(q, '"');
                    if (!e2 || e2 > rb) break;
                    size_t pl = (size_t)(e2 - q);
                    if (pl > 0 && pl < 512) {
                        memcpy(sc[n], q, pl);
                        sc[n][pl] = 0;
                        if (sc[n][0] == '/') n++;
                    }
                    q = e2 + 1;
                }
                scope_save(uid, sc, n);
                free(sc);
            }
        }
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
        uint32_t rootid = XL_ROOT;
        for (uint32_t i = 0; i < (uint32_t)v->sn.h.rec_count; i++)
            if (v->sn.recs[i].parent == XL_ROOT) { rootid = i; break; }
        if (rootid == XL_ROOT) continue;
        char p[4096];
        for (int pass = 0; pass < 2; pass++) {
            for (uint32_t i = 0; i < (uint32_t)v->sn.h.rec_count && shown < 200; i++) {
                const xl_rec *r = &v->sn.recs[i];
                if (r->parent != rootid) continue;
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
                    for (uint32_t j = 0; j < (uint32_t)v->sn.h.rec_count && shown < 200; j++) {
                        const xl_rec *c = &v->sn.recs[j];
                        if (c->parent != i || !(c->flags & XL_F_DIR)) continue;
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

static void handle_status(sbuf *out) {
    uint64_t total = 0;
    sb_puts(out, "{\"state\":\"");
    if (g_building) sb_puts(out, "building");
    else if (g_failed) sb_puts(out, "degraded");
    else sb_puts(out, "ready");
    sb_puts(out, "\",\"index_dir\":\"");
    sb_json(out, g_idx_dir, strlen(g_idx_dir));
    sb_puts(out, "\",\"volumes\":[");
    pthread_rwlock_rdlock(&g_rw);
    for (int i = 0; i < g_nvols; i++) {
        vol_t *v = &g_vols[i];
        total += v->records;
        if (i) sb_puts(out, ",");
        sb_puts(out, "{\"root\":\"");
        sb_json(out, v->root, strlen(v->root));
        sb_printf(out, "\",\"ok\":%s,\"records\":%llu,\"built_at\":%llu,\"build_ms\":%llu,\"building\":%s}",
                  v->ok ? "true" : "false",
                  (unsigned long long)v->records,
                  (unsigned long long)v->built_at,
                  (unsigned long long)v->build_ms,
                  (g_building && g_cur_vol == i) ? "true" : "false");
    }
    pthread_rwlock_unlock(&g_rw);
    sb_printf(out, "],\"records\":%llu,\"building\":%s,\"progress\":{\"root\":\"",
              (unsigned long long)total, g_building ? "true" : "false");
    if (g_building && g_cur_vol >= 0) sb_json(out, g_vols[g_cur_vol].root, strlen(g_vols[g_cur_vol].root));
    sb_printf(out, "\",\"records\":%llu,\"elapsed_ms\":%llu},\"error\":\"",
              (unsigned long long)xl_scan_records,
              (unsigned long long)(g_building ? now_ms() - g_build_start : g_build_ms));
    sb_json(out, g_err, strlen(g_err));
    size_t dels = 0;
    size_t adds = live_delta_count(&dels);
    sb_printf(out, "\",\"live\":{\"adds\":%zu,\"dels\":%zu,\"overflow\":%s}",
              adds, dels, live_overflowed() ? "true" : "false");
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

static void handle_client(int fd) {
    g_extra_hdr[0] = 0;
    g_range_hdr[0] = 0;
    g_fs_fd = -1;

    /* Accept-Ranges 之外的响应不带 Range/Cache 头 */

    char req[REQ_MAX];
    size_t n = 0;
    while (n + 1 < sizeof(req)) {
        ssize_t r = recv(fd, req + n, sizeof(req) - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (!n) { close(fd); return; }
    req[n] = 0;

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

    const char *req_body = strstr(req, "\r\n\r\n");
    req_body = req_body ? req_body + 4 : "";
    dump_header_names_once(req);
    char hdr_user[128] = "";
    int hdr_admin = 0;
    header_str(req, "x-trim-username:", hdr_user, sizeof hdr_user);
    {
        char a[16] = "";
        static char last_isadmin[16] = "\x01";
        int has = header_str(req, "x-trim-isadmin:", a, sizeof a);
        if (strcmp(a, last_isadmin)) {
            fprintf(stderr, "xuanlan serve: isadmin 头=%s\n", has ? a : "(缺)");
            snprintf(last_isadmin, sizeof last_isadmin, "%s", a);
        }
        if (has)
            hdr_admin = (a[0] == '1' || a[0] == 't' || a[0] == 'T' || a[0] == 'y' || a[0] == 'Y');
    }
    int uid = header_uid(req);
    const char *uid_src = uid >= 0 ? "trim" : NULL;
    if (uid < 0) {
        int cu = header_client_uid(req);
        if (cu >= 0) { uid = cu; uid_src = "client"; }
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
    if (uid < 0) uid = g_acl_uid;
    char tmpall[16];
    memset(tmpall, 0, sizeof tmpall);
    {
        static int last_uid = -2;
        if (uid != last_uid) {
            fprintf(stderr, "xuanlan serve: 请求身份 uid=%d (%s)\n", uid,
                    uid < 0 ? (uid_src ? uid_src : "无身份头") : uid_src);
            last_uid = uid;
        }
    }
    if (!strcmp(pathonly, "/api/settings") || (!strcmp(pathonly, "/api/dir") && strstr(qs, "all="))) {
        fprintf(stderr, "xuanlan serve: %s admin=%d qs=%s\n", pathonly, hdr_admin || uid == 0, qs);
    }
    if (!strcmp(pathonly, "/api/search")) {
        if (uid < 0) uid = g_acl_uid;
        handle_search(&body, qs, uid, hdr_admin || uid == 0 || admin_by_uid(uid));
    } else if (!strcmp(pathonly, "/api/me")) {
        handle_me(&body, uid, uid_src ? uid_src : (uid >= 0 ? "env" : "none"), hdr_user, hdr_admin || admin_by_uid(uid));
    } else if (!strcmp(pathonly, "/api/status")) {
        handle_status(&body);
    } else if (!strcmp(pathonly, "/api/rebuild")) {
        if (strcmp(method, "POST") != 0) {
            code = 405;
            sb_puts(&body, "{\"error\":\"use POST\"}");
        } else {
            start_build();
            sb_puts(&body, "{\"ok\":true}");
        }
    } else if (!strcmp(pathonly, "/api/clientlog")) {
        char msg[1024];
        if (param(qs, "msg", msg, sizeof msg))
            fprintf(stderr, "xuanlan client: %.900s\n", msg);
        sb_puts(&body, "{\"ok\":true}");
    } else if (!strcmp(pathonly, "/api/settings")) {
        if (strcmp(method, "POST") == 0) handle_settings_post(&body, req_body, uid);
        else handle_settings_get(&body, uid, hdr_admin || uid == 0 || admin_by_uid(uid), param(qs, "all", tmpall, sizeof tmpall));
    } else if (!strcmp(pathonly, "/api/acl")) {
        handle_acl_list(&body, uid, hdr_admin || uid == 0 || admin_by_uid(uid), param(qs, "all", tmpall, sizeof tmpall));
    } else if (!strcmp(pathonly, "/api/dir")) {
        handle_dir(&body, qs, uid, hdr_admin || uid == 0 || admin_by_uid(uid));
    } else if (!strcmp(pathonly, "/api/fsop")) {
        if (strcmp(method, "POST") != 0) { code = 405; sb_puts(&body, "{\"error\":\"use POST\"}"); }
        else handle_fsop(&body, req_body, uid);
    } else if (!strcmp(pathonly, "/api/trash")) {
        if (strcmp(method, "POST") == 0) handle_trash_post(&body, req_body, uid);
        else handle_trash_get(&body, uid);
    } else if (!strcmp(pathonly, "/api/raw")) {
        handle_raw(&body, qs, uid, &ctype);
    } else if (!strcmp(pathonly, "/api/text")) {
        handle_text(&body, qs, uid, &ctype);
    } else if (!strcmp(pathonly, "/api/probe")) {
        handle_probe(&body, qs, uid);
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
                         "%s%sCache-Control: no-store\r\nConnection: close\r\n\r\n",
                  g_fs_status, g_fs_status == 206 ? "Partial Content" : "OK",
                  ctype, (long long)g_fs_len, g_range_hdr, g_extra_hdr);
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
                     "%s%sCache-Control: no-store\r\nConnection: close\r\n\r\n",
              code, code == 200 ? "OK" : (code == 404 ? "Not Found" : "Error"),
              ctype, body.n, g_range_hdr, g_extra_hdr);
    if (body.n) sb_add(&resp, body.p, body.n);
    write_all(fd, resp.p, resp.n);
    free(resp.p);
    free(body.p);
    close(fd);
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
    chmod(path, 0666);
    return fd;
}

static void on_signal(int sig) { (void)sig; g_stop = 1; }

int httpd_serve(const xl_svc_opts *o) {
    const char *acl_env = getenv("XL_ACL_UID");
    g_acl_uid = acl_env ? atoi(acl_env) : -1;
    g_ui_dir = o->ui_dir;
    snprintf(g_idx_dir, sizeof(g_idx_dir), "%s", o->idx_dir);
    snprintf(g_trash, sizeof(g_trash), "%s/trash", g_idx_dir);
    mkdir(g_trash, 0755);
    mkdir(o->idx_dir, 0755);
    if (o->sock_path[0] && probe_existing(o->sock_path)) {
        if (tcp_alive(o->port)) {
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
        vol_t *v = &g_vols[g_nvols];
        memset(v, 0, sizeof(*v));
        snprintf(v->root, sizeof(v->root), "%s", r);
        const char *base = strrchr(r, '/');
        base = base ? base + 1 : r;
        if (snprintf(v->file, sizeof(v->file), "%s/%s.idx", o->idx_dir, base) >= (int)sizeof(v->file)) {
            fprintf(stderr, "xuanlan serve: 索引路径过长: %s\n", r);
            continue;
        }
        if (snapshot_open(v->file, &v->sn) == 0) {
            v->ok = 1;
            v->records = v->sn.h.rec_count;
            v->built_at = v->sn.h.built_at;
        }
        g_nvols++;
    }
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
    ls[nls] = listen_tcp(o->bind_addr, o->port);
    if (ls[nls] < 0) fprintf(stderr, "xuanlan serve: TCP %s:%d 绑定失败: %s\n",
                             o->bind_addr, o->port, strerror(errno));
    else nls++;
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
    if (live_start(o->roots, 1) == 0)
        fprintf(stderr, "xuanlan serve: fanotify 实时增量已启用\n");
    else
        fprintf(stderr, "xuanlan serve: fanotify 不可用（需 root/CAP_SYS_ADMIN），仅周期对账\n");

    int need_build = 0;
    for (int i = 0; i < g_nvols; i++)
        if (!g_vols[i].ok) need_build = 1;
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
    }
    for (int i = 0; i < nls; i++) close(ls[i]);
    if (o->sock_path[0]) unlink(o->sock_path);
    pthread_mutex_lock(&g_qlock);
    pthread_cond_broadcast(&g_qcond);
    pthread_mutex_unlock(&g_qlock);
    return 0;
}
