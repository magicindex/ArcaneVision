#include "unitdelta.h"
#include "xl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

#define UD_MAGIC 0x31444c58u   /* "XLD1" */
#define UD_CAP   (16u * 1024u * 1024u)

static pthread_mutex_t g_ud_mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    uint32_t magic;
    uint32_t len;
    uint8_t  op;       /* 0=add 1=delete */
    uint8_t  flags;
    uint16_t pad;
    uint32_t size;
    uint32_t mtime;
} ud_hdr;

static int ud_write_item(int fd, const xl_unit_delta *it) {
    ud_hdr h;
    memset(&h, 0, sizeof h);
    h.magic = UD_MAGIC;
    h.len = (uint32_t)strlen(it->path);
    h.op = it->deleted ? 1 : 0;
    h.flags = it->flags;
    h.size = it->size;
    h.mtime = it->mtime;
    if (h.len > 8192) return -1;
    return (write(fd, &h, sizeof h) == (ssize_t)sizeof h &&
            write(fd, it->path, h.len) == (ssize_t)h.len) ? 0 : -1;
}

int xl_unit_delta_append_batch(const char *file, const xl_unit_delta *items, size_t n) {
    if (!file || !items || !n) return -1;
    pthread_mutex_lock(&g_ud_mu);
    struct stat st;
    if (stat(file, &st) == 0 && st.st_size > (off_t)UD_CAP) {
        pthread_mutex_unlock(&g_ud_mu);
        return -1;
    }
    int fd = open(file, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) { pthread_mutex_unlock(&g_ud_mu); return -1; }
    int ok = 1;
    for (size_t i = 0; i < n; i++) {
        if (ud_write_item(fd, &items[i]) != 0) { ok = 0; break; }
    }
    close(fd);
    pthread_mutex_unlock(&g_ud_mu);
    return ok ? 0 : -1;
}

int xl_unit_delta_append(const char *file, const char *path, int deleted,
                         uint8_t flags, uint32_t size, uint32_t mtime) {
    if (!path) return -1;
    xl_unit_delta it;
    memset(&it, 0, sizeof it);
    it.path = (char *)path;
    it.deleted = deleted ? 1 : 0;
    it.flags = flags;
    it.size = size;
    it.mtime = mtime;
    return xl_unit_delta_append_batch(file, &it, 1);
}

static int ud_load_locked(const char *file, xl_unit_delta_set *out) {
    memset(out, 0, sizeof(*out));
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return 0; }
    char *buf = malloc((size_t)st.st_size);
    if (!buf) { close(fd); return -1; }
    ssize_t got = 0;
    while (got < st.st_size) {
        ssize_t r = read(fd, buf + got, (size_t)(st.st_size - got));
        if (r <= 0) break;
        got += r;
    }
    close(fd);
    size_t off = 0;
    while (off + sizeof(ud_hdr) <= (size_t)got) {
        ud_hdr h;
        memcpy(&h, buf + off, sizeof h);
        off += sizeof h;
        if (h.magic != UD_MAGIC || h.len > 8192 || off + h.len > (size_t)got) break;
        if (out->n == out->cap) {
            size_t nc = out->cap ? out->cap * 2 : 128;
            xl_unit_delta *nv = realloc(out->v, nc * sizeof(*nv));
            if (!nv) break;
            out->v = nv; out->cap = nc;
        }
        xl_unit_delta *d = &out->v[out->n];
        memset(d, 0, sizeof(*d));
        d->path = malloc(h.len + 1);
        if (!d->path) break;
        memcpy(d->path, buf + off, h.len);
        d->path[h.len] = 0;
        d->size = h.size; d->mtime = h.mtime;
        d->flags = h.flags; d->deleted = h.op ? 1 : 0;
        out->n++;
        off += h.len;
    }
    free(buf);
    return 0;
}

int xl_unit_delta_load(const char *file, xl_unit_delta_set *out) {
    pthread_mutex_lock(&g_ud_mu);
    int rc = ud_load_locked(file, out);
    pthread_mutex_unlock(&g_ud_mu);
    return rc;
}

static uint64_t ud_h(const char *s) {
    uint64_t h = 1469598103934665603ull;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ull; }
    return h ? h : 1;
}

int xl_unit_delta_compact(const char *file) {
    if (!file) return -1;
    pthread_mutex_lock(&g_ud_mu);
    xl_unit_delta_set in;
    if (ud_load_locked(file, &in) != 0 || in.n == 0) {
        xl_unit_delta_free(&in);
        pthread_mutex_unlock(&g_ud_mu);
        return 0;
    }
    size_t cap = 1;
    while (cap < in.n * 2) cap <<= 1;
    int *slots = malloc(cap * sizeof(int));
    if (!slots) { xl_unit_delta_free(&in); pthread_mutex_unlock(&g_ud_mu); return -1; }
    for (size_t i = 0; i < cap; i++) slots[i] = -1;
    for (size_t i = 0; i < in.n; i++) {
        size_t j = ud_h(in.v[i].path) & (cap - 1);
        while (slots[j] >= 0 && strcmp(in.v[slots[j]].path, in.v[i].path)) j = (j + 1) & (cap - 1);
        slots[j] = (int)i;
    }
    char tmp[1400];
    int w = snprintf(tmp, sizeof tmp, "%s.compact-%ld", file, (long)getpid());
    int fd = (w > 0 && (size_t)w < sizeof tmp) ? open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600) : -1;
    int ok = fd >= 0;
    for (size_t i = 0; ok && i < in.n; i++) {
        size_t j = ud_h(in.v[i].path) & (cap - 1);
        while (slots[j] >= 0 && strcmp(slots[j] >= 0 ? in.v[slots[j]].path : "", in.v[i].path)) j = (j + 1) & (cap - 1);
        if (slots[j] != (int)i) continue;
        ud_hdr h;
        memset(&h, 0, sizeof h);
        h.magic = UD_MAGIC;
        h.len = (uint32_t)strlen(in.v[i].path);
        h.op = in.v[i].deleted ? 1 : 0;
        h.flags = in.v[i].flags;
        h.size = in.v[i].size;
        h.mtime = in.v[i].mtime;
        if (write(fd, &h, sizeof h) != (ssize_t)sizeof h ||
            write(fd, in.v[i].path, h.len) != (ssize_t)h.len) ok = 0;
    }
    if (fd >= 0) close(fd);
    if (ok && rename(tmp, file) == 0) chmod(file, 0600);
    else { ok = 0; unlink(tmp); }
    free(slots);
    xl_unit_delta_free(&in);
    pthread_mutex_unlock(&g_ud_mu);
    return ok ? 0 : -1;
}

void xl_unit_delta_free(xl_unit_delta_set *out) {
    if (!out) return;
    for (size_t i = 0; i < out->n; i++) free(out->v[i].path);
    free(out->v);
    memset(out, 0, sizeof(*out));
}

int xl_unit_delta_deleted(const xl_unit_delta_set *s, const char *path) {
    if (!s || !path) return 0;
    int deleted = 0;
    for (size_t i = 0; i < s->n; i++) {
        const xl_unit_delta *d = &s->v[i];
        if (!strcmp(d->path, path)) { deleted = d->deleted; continue; }
        if (d->deleted && (d->flags & XL_F_DIR)) {
            size_t n = strlen(d->path);
            if (!strncmp(d->path, path, n) && path[n] == '/') deleted = 1;
        }
    }
    return deleted;
}

size_t xl_unit_delta_query(const xl_unit_delta_set *s, const char *needle,
                           xl_live_hit **out) {
    *out = NULL;
    if (!s || !needle || !*needle) return 0;
    size_t n = 0;
    for (size_t i = 0; i < s->n; i++) {
        const xl_unit_delta *d = &s->v[i];
        if (d->deleted) continue;
        int later = 0;
        for (size_t j = i + 1; j < s->n; j++)
            if (!strcmp(s->v[j].path, d->path)) { later = 1; break; }
        if (later) continue;
        const char *bn = strrchr(d->path, '/');
        bn = bn ? bn + 1 : d->path;
        size_t bl = strlen(bn), nl = strlen(needle);
        if (!mem_ci(bn, bl, needle, nl)) continue;
        xl_live_hit *nv = realloc(*out, (n + 1) * sizeof(**out));
        if (!nv) break;
        *out = nv;
        memset(&nv[n], 0, sizeof(nv[n]));
        nv[n].path = strdup(d->path);
        nv[n].size = d->size;
        nv[n].mtime = d->mtime;
        nv[n].flags = d->flags;
        if (!nv[n].path) break;
        n++;
    }
    return n;
}
