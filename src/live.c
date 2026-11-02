/* 增量层：fanotify 文件系统标记 + 目录/文件增删的轻量 delta
 *
 * 设计要点：
 *  - FAN_MARK_FILESYSTEM（本机内核下 FAN_MARK_MOUNT 与 FAN_REPORT_FID 组合被拒）
 *  - 事件只取 DFID_NAME：给出父目录的文件句柄 + 子项名；父目录路径用
 *    open_by_handle_at + /proc/self/fd 还原。
 *  - 增量只维护"新增/改名后的新名字"和"已删除路径"两个小集合；查询时与
 *    基线快照合并。整卷重建由对账线程负责，delta 随之清空。
 */
#include "xl.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define LV_MAX_ROOTS 8
#define LV_MAX_ADDS  (1u << 17)     /* 13 万条上限，超出即请求整卷重建 */
#define LV_QUEUE     4096

typedef struct {
    char *path;
    uint32_t size;
    uint32_t mtime;
    uint8_t flags;
} lv_add;

typedef struct { char *path; } lv_del;

/* 开放寻址字符串表：键为路径哈希；值 0xFFFFFFFF 表示墓碑 */
#define LV_TOMB 0xFFFFFFFFu
typedef struct {
    uint64_t *keys;
    uint32_t *vals;
    size_t cap, used;
} lv_map;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int started, stop, fatal;
    int fd;
    int root_fd[LV_MAX_ROOTS];
    char roots[LV_MAX_ROOTS][256];
    dev_t root_dev[LV_MAX_ROOTS];
    int nroots;

    lv_add *adds; size_t adds_n, adds_cap;
    lv_del *dels; size_t dels_n, dels_cap;
    lv_map add_map, del_map;

    struct { char *dir; char name[XL_NAME_MAX + 1]; uint32_t mask; } q[LV_QUEUE];
    size_t qh, qt;
} L;

static uint64_t h64(const char *s) {
    uint64_t h = 1469598103934665603ull;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ull; }
    return h ? h : 1;
}

static void map_init(lv_map *m, size_t cap) {
    m->cap = cap; m->used = 0;
    m->keys = calloc(cap, sizeof(uint64_t));
    m->vals = calloc(cap, sizeof(uint32_t));
}
static void map_free(lv_map *m) { free(m->keys); free(m->vals); memset(m, 0, sizeof(*m)); }
static uint32_t *map_get(lv_map *m, uint64_t k) {
    if (!m->cap) return NULL;
    size_t i = k & (m->cap - 1);
    for (size_t n = 0; n < m->cap; n++, i = (i + 1) & (m->cap - 1)) {
        if (!m->keys[i]) return NULL;
        if (m->keys[i] == k) return m->vals[i] == LV_TOMB ? NULL : &m->vals[i];
    }
    return NULL;
}
static void map_put(lv_map *m, uint64_t k, uint32_t v) {
    if (!m->cap) return;
    if (m->used * 10 >= m->cap * 7) {           /* 负载 0.7 扩容 */
        lv_map n; map_init(&n, m->cap * 2);
        for (size_t i = 0; i < m->cap; i++)
            if (m->keys[i]) {
                size_t j = m->keys[i] & (n.cap - 1);
                while (n.keys[j]) j = (j + 1) & (n.cap - 1);
                n.keys[j] = m->keys[i]; n.vals[j] = m->vals[i];
            }
        n.used = m->used;
        map_free(m); *m = n;
    }
    size_t i = k & (m->cap - 1);
    while (m->keys[i] && m->keys[i] != k) i = (i + 1) & (m->cap - 1);
    if (!m->keys[i]) { m->keys[i] = k; m->used++; }
    m->vals[i] = v;
}
static void map_del(lv_map *m, uint64_t k) {
    if (!m->cap) return;
    size_t i = k & (m->cap - 1);
    for (size_t n = 0; n < m->cap; n++, i = (i + 1) & (m->cap - 1))
        if (m->keys[i] == k) { m->vals[i] = LV_TOMB; return; }
}

/* ---------- delta 维护（调用方持锁） ---------- */
static void add_put(const char *path, const xl_store_stat *st) {
    uint64_t k = h64(path);
    uint32_t *at = map_get(&L.add_map, k);
    if (at) {
        lv_add *a = &L.adds[*at];
        a->size = st->size; a->mtime = st->mtime; a->flags = st->flags;
        return;
    }
    if (L.adds_n >= LV_MAX_ADDS) { L.fatal = 1; return; }
    if (L.adds_n == L.adds_cap) {
        L.adds_cap = L.adds_cap ? L.adds_cap * 2 : 1024;
        L.adds = realloc(L.adds, L.adds_cap * sizeof(lv_add));
    }
    L.adds[L.adds_n].path = strdup(path);
    L.adds[L.adds_n].size = st->size;
    L.adds[L.adds_n].mtime = st->mtime;
    L.adds[L.adds_n].flags = st->flags;
    map_put(&L.add_map, k, (uint32_t)L.adds_n);
    L.adds_n++;
    map_del(&L.del_map, k);
}

static void add_drop(const char *path) {
    uint64_t k = h64(path);
    uint32_t *at = map_get(&L.add_map, k);
    if (!at) return;
    uint32_t i = *at;
    free(L.adds[i].path);
    L.adds[i] = L.adds[--L.adds_n];
    map_del(&L.add_map, k);
    if (i < L.adds_n) map_put(&L.add_map, h64(L.adds[i].path), i);
}

static void del_put(const char *path) {
    uint64_t k = h64(path);
    add_drop(path);
    if (map_get(&L.del_map, k)) return;
    if (L.dels_n == L.dels_cap) {
        L.dels_cap = L.dels_cap ? L.dels_cap * 2 : 1024;
        L.dels = realloc(L.dels, L.dels_cap * sizeof(lv_del));
    }
    L.dels[L.dels_n].path = strdup(path);
    map_put(&L.del_map, k, (uint32_t)L.dels_n);
    L.dels_n++;
}

/* ---------- 事件处理 ---------- */
static int under_root(const char *path, int *ri) {
    for (int i = 0; i < L.nroots; i++) {
        size_t n = strlen(L.roots[i]);
        if (strncmp(path, L.roots[i], n) == 0 && (path[n] == '/' || path[n] == 0)) {
            *ri = i; return 1;
        }
    }
    return 0;
}

static void apply_one(const char *dir, const char *name, uint32_t mask) {
    if (!dir || !name || !*name) return;
    int ri;
    if (!under_root(dir, &ri)) return;
    char path[PATH_MAX];
    int cl = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (cl <= 0 || (size_t)cl >= sizeof(path)) return;

    if (mask & (FAN_DELETE | FAN_MOVED_FROM)) {
        del_put(path);
        return;
    }
    if (mask & (FAN_CREATE | FAN_MOVED_TO)) {
        struct stat st;
        if (lstat(path, &st) != 0) return;
        if (st.st_dev != L.root_dev[ri]) return;      /* 嵌套挂载的内容不入索引 */
        xl_store_stat xs;
        xs.size = (uint32_t)st.st_size;
        xs.mtime = (uint32_t)st.st_mtime;
        xs.flags = S_ISDIR(st.st_mode) ? XL_F_DIR : (S_ISLNK(st.st_mode) ? XL_F_LINK : 0);
        add_put(path, &xs);
    }
}

static void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&L.mu);
    for (;;) {
        if (L.stop) break;
        while (L.qh == L.qt && !L.stop)
            pthread_cond_wait(&L.cv, &L.mu);
        if (L.stop) {
            while (L.qh != L.qt) {          /* 退出前释放未处理事件 */
                free(L.q[L.qh].dir);
                L.q[L.qh].dir = NULL;
                L.qh = (L.qh + 1) % LV_QUEUE;
            }
            break;
        }
        if (L.qh != L.qt) {
            /* 静默 250ms 后再应用，合并同目录的高频事件 */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 250 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&L.cv, &L.mu, &ts);
            while (L.qh != L.qt) {
                size_t slot = L.qh;
                char *d = L.q[slot].dir;
                char name[XL_NAME_MAX + 1];
                memcpy(name, L.q[slot].name, sizeof(name));
                uint32_t m = L.q[slot].mask;
                L.q[slot].dir = NULL;              /* 已取走，槽位不再持有指针 */
                L.qh = (slot + 1) % LV_QUEUE;
                pthread_mutex_unlock(&L.mu);
                apply_one(d, name, m);
                free(d);
                pthread_mutex_lock(&L.mu);
            }
        }
    }
    pthread_mutex_unlock(&L.mu);
    return NULL;
}

static void *reader(void *arg) {
    (void)arg;
    char buf[16384] __attribute__((aligned(8)));
    while (!__atomic_load_n(&L.stop, __ATOMIC_RELAXED)) {
        struct pollfd p = { .fd = L.fd, .events = POLLIN };
        int r = poll(&p, 1, 500);
        if (r <= 0) continue;
        ssize_t n = read(L.fd, buf, sizeof(buf));
        if (n <= 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            break;
        }
        for (struct fanotify_event_metadata *m = (void *)buf; FAN_EVENT_OK(m, n);
             m = FAN_EVENT_NEXT(m, n)) {
            char *h = (char *)m + sizeof(*m);
            char *end = (char *)m + m->event_len;
            while (h + sizeof(struct fanotify_event_info_header) <= end) {
                struct fanotify_event_info_header *ih = (void *)h;
                if (ih->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
                    struct fanotify_event_info_fid *fid = (void *)ih;
                    struct file_handle *fh = (void *)fid->handle;
                    size_t fh_len = sizeof(struct file_handle) + fh->handle_bytes;
                    size_t name_off = (fh_len + 3) & ~(size_t)3;
                    const char *nm = (char *)fh + name_off;
                    size_t nm_len = ih->len > sizeof(*fid) + name_off
                                  ? ih->len - sizeof(*fid) - name_off : 0;
                    char name[XL_NAME_MAX + 1];
                    size_t copy = nm_len;
                    if (copy && nm[copy - 1] == 0) copy--;   /* 事件里带结尾 NUL */
                    if (copy > XL_NAME_MAX) copy = XL_NAME_MAX;
                    memcpy(name, nm, copy); name[copy] = 0;

                    char dir[PATH_MAX];
                    dir[0] = 0;
                    for (int i = 0; i < L.nroots && !dir[0]; i++) {
                        int f = open_by_handle_at(L.root_fd[i], fh, O_PATH | O_CLOEXEC);
                        if (f < 0) continue;
                        char proc[64], lp[PATH_MAX];
                        snprintf(proc, sizeof(proc), "/proc/self/fd/%d", f);
                        ssize_t k = readlink(proc, lp, sizeof(lp) - 1);
                        close(f);
                        if (k <= 0) continue;
                        lp[k] = 0;
                        char *suf = strstr(lp, " (deleted)");
                        if (suf) *suf = 0;
                        snprintf(dir, sizeof(dir), "%s", lp);
                    }
                    if (dir[0] && name[0]) {
                        pthread_mutex_lock(&L.mu);
                        size_t nxt = (L.qt + 1) % LV_QUEUE;
                        if (nxt != L.qh) {
                            char *dup = strdup(dir);
                            free(L.q[L.qt].dir);   /* 正常为 NULL；保留兜底 */
                            L.q[L.qt].dir = dup;
                            snprintf(L.q[L.qt].name, sizeof(L.q[L.qt].name), "%s", name);
                            L.q[L.qt].mask = m->mask;
                            L.qt = nxt;
                            pthread_cond_signal(&L.cv);
                        }
                        pthread_mutex_unlock(&L.mu);
                    }
                }
                h += ih->len;
            }
        }
    }
    return NULL;
}

/* ---------- 对外 API ---------- */
int live_start(const char *roots_csv, int verbose) {
    memset(&L, 0, sizeof(L));
    pthread_mutex_init(&L.mu, NULL);
    pthread_cond_init(&L.cv, NULL);
    L.fd = -1;
    for (int i = 0; i < LV_MAX_ROOTS; i++) L.root_fd[i] = -1;

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", roots_csv ? roots_csv : "/vol1,/vol2,/vol3,/vol4");
    char *save = NULL;
    for (char *t = strtok_r(tmp, ",", &save); t && L.nroots < LV_MAX_ROOTS; t = strtok_r(NULL, ",", &save)) {
        while (*t == ' ') t++;
        if (!*t) continue;
        int fd = open(t, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) continue;
        struct stat st;
        if (fstat(fd, &st) != 0) { close(fd); continue; }
        int ri = L.nroots++;
        L.root_fd[ri] = fd;
        L.root_dev[ri] = st.st_dev;
        snprintf(L.roots[ri], sizeof(L.roots[ri]), "%s", t);
    }
    if (!L.nroots) return -1;

    L.fd = fanotify_init(FAN_CLASS_NOTIF | FAN_CLOEXEC | FAN_REPORT_FID | FAN_REPORT_DFID_NAME,
                         O_RDONLY | O_CLOEXEC);
    if (L.fd < 0) return -1;
    int marked = 0;
    for (int i = 0; i < L.nroots; i++) {
        if (fanotify_mark(L.fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM,
                          FAN_CREATE | FAN_DELETE | FAN_MOVED_FROM | FAN_MOVED_TO | FAN_ONDIR,
                          AT_FDCWD, L.roots[i]) == 0) marked++;
        else if (verbose)
            fprintf(stderr, "xuanlan: fanotify_mark(%s): %s\n", L.roots[i], strerror(errno));
    }
    if (!marked) { close(L.fd); L.fd = -1; return -1; }
    map_init(&L.add_map, 4096);
    map_init(&L.del_map, 1024);
    L.started = 1;
    pthread_t t1, t2;
    pthread_create(&t1, NULL, reader, NULL);
    pthread_create(&t2, NULL, worker, NULL);
    pthread_detach(t1);
    pthread_detach(t2);
    return 0;
}

void live_clear(void) {
    if (!L.started) return;
    pthread_mutex_lock(&L.mu);
    for (size_t i = 0; i < L.adds_n; i++) free(L.adds[i].path);
    for (size_t i = 0; i < L.dels_n; i++) free(L.dels[i].path);
    L.adds_n = L.dels_n = 0;
    L.fatal = 0;
    map_free(&L.add_map); map_free(&L.del_map);
    map_init(&L.add_map, 4096); map_init(&L.del_map, 1024);
    pthread_mutex_unlock(&L.mu);
}

void live_stop(void) {
    if (!L.started) return;
    __atomic_store_n(&L.stop, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&L.mu);
    pthread_cond_broadcast(&L.cv);
    pthread_mutex_unlock(&L.mu);
}

size_t live_delta_count(size_t *dels) {
    if (!L.started) { if (dels) *dels = 0; return 0; }
    pthread_mutex_lock(&L.mu);
    size_t a = L.adds_n, d = L.dels_n;
    pthread_mutex_unlock(&L.mu);
    if (dels) *dels = d;
    return a;
}

int live_overflowed(void) { return L.fatal; }

int live_is_deleted(const char *path) {
    if (!L.started || !L.dels_n) return 0;
    int hit = 0;
    pthread_mutex_lock(&L.mu);
    hit = map_get(&L.del_map, h64(path)) != NULL;
    pthread_mutex_unlock(&L.mu);
    return hit;
}

/* 扫描 delta 中匹配的新增项，按 mtime 降序返回 */
size_t live_query(const char *q, xl_live_hit **out) {
    *out = NULL;
    if (!L.started) return 0;
    if (!q || !*q) return 0;
    size_t n = 0, cap = 0;
    xl_live_hit *v = NULL;
    pthread_mutex_lock(&L.mu);
    for (size_t i = 0; i < L.adds_n; i++) {
        const char *base = strrchr(L.adds[i].path, '/');
        base = base ? base + 1 : L.adds[i].path;
        if (!memmem_ci(base, strlen(base), q, strlen(q))) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            v = realloc(v, cap * sizeof(*v));
        }
        v[n].path = strdup(L.adds[i].path);
        v[n].mtime = L.adds[i].mtime;
        v[n].size = L.adds[i].size;
        v[n].flags = L.adds[i].flags;
        n++;
    }
    pthread_mutex_unlock(&L.mu);
    *out = v;
    return n;
}
