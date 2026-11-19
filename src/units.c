/* 隔离单元目录布局：idx/units/<unit>/。P1 只建布局，不建索引。 */
#include "scanmode.h"
#include "unitdelta.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int valid_unit_key(const char *unit) {
    if (!unit || !*unit) return 0;
    if (!strcmp(unit, "shared")) return 0;   /* shared 不走 units/ */
    if (!(unit[0] == 'u' || unit[0] == 'g' || unit[0] == 'h')) return 0;
    if (!unit[1]) return 0;
    for (const char *p = unit + 1; *p; p++) {
        if (unit[0] == 'h') {
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= 'A' && *p <= 'Z') || *p == '_')) return 0;
        } else if (*p < '0' || *p > '9') return 0;
    }
    return 1;
}

int xl_scan_unit_dir(const char *idx_dir, const char *unit, char *out, size_t cap) {
    if (!idx_dir || !valid_unit_key(unit)) return -1;
    int n = snprintf(out, cap, "%s/units/%s", idx_dir, unit);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int xl_units_init(const char *idx_dir, char *err, size_t cap) {
    if (!idx_dir || !*idx_dir) {
        if (err) snprintf(err, cap, "empty index dir");
        return -1;
    }
    char path[1200];
    int n = snprintf(path, sizeof path, "%s/units", idx_dir);
    if (n <= 0 || (size_t)n >= sizeof path) return -1;
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        if (err) snprintf(err, cap, "mkdir %s: %s", path, strerror(errno));
        return -1;
    }
    chmod(path, 0700);
    return 0;
}

#include <dirent.h>
#include <stdint.h>
#include <time.h>

static int join_path(char *out, size_t cap, const char *a, const char *b) {
    size_t al = strlen(a), bl = strlen(b);
    if (al + 1 + bl + 1 > cap) return -1;
    memcpy(out, a, al);
    out[al] = '/';
    memcpy(out + al + 1, b, bl);
    out[al + 1 + bl] = 0;
    return 0;
}

int xl_units_capacity(const char *idx_dir, int *count, uint64_t *bytes, uint64_t *delta_bytes) {
    if (count) *count = 0;
    if (bytes) *bytes = 0;
    if (delta_bytes) *delta_bytes = 0;
    char path[1200];
    snprintf(path, sizeof path, "%s/units", idx_dir);
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char ud[1400];
        if (join_path(ud, sizeof ud, path, e->d_name) < 0) continue;
        struct stat st;
        if (stat(ud, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (count) (*count)++;
        DIR *u = opendir(ud);
        if (!u) continue;
        struct dirent *f;
        while ((f = readdir(u)) != NULL) {
            if (f->d_name[0] == '.') continue;
            char fp[1600];
            if (join_path(fp, sizeof fp, ud, f->d_name) < 0) continue;
            struct stat fs;
            if (stat(fp, &fs) != 0 || !S_ISREG(fs.st_mode)) continue;
            if (bytes) *bytes += (uint64_t)fs.st_size;
            if (delta_bytes && !strcmp(f->d_name, "delta.log"))
                *delta_bytes += (uint64_t)fs.st_size;
        }
        closedir(u);
    }
    closedir(d);
    return 0;
}

static int rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1800];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        rm_rf(p);
    }
    closedir(d);
    return rmdir(path);
}

static int active_has(const char *const *active, int n, const char *key) {
    for (int i = 0; i < n; i++) if (!strcmp(active[i], key)) return 1;
    return 0;
}

int xl_units_gc(const char *idx_dir, const char *const *active, int nactive,
                int grace_days, int purge, int *removed) {
    if (removed) *removed = 0;
    char base[1200], trash[1300];
    snprintf(base, sizeof base, "%s/units", idx_dir);
    snprintf(trash, sizeof trash, "%s/.trash", base);
    mkdir(trash, 0700);
    time_t now = time(NULL);
    time_t cutoff = now - (time_t)grace_days * 86400;
    DIR *d = opendir(base);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
            !strcmp(e->d_name, ".trash") || !strncmp(e->d_name, ".tmp", 4))
            continue;
        char ud[1400];
        if (join_path(ud, sizeof ud, base, e->d_name) < 0) continue;
        struct stat st;
        if (stat(ud, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (active_has(active, nactive, e->d_name)) continue;
        if (st.st_mtime > cutoff) continue;
        if (purge) {
            if (rm_rf(ud) == 0 && removed) (*removed)++;
        } else {
            char dst[1700];
            snprintf(dst, sizeof dst, "%s/%s-%ld", trash, e->d_name, (long)now);
            if (rename(ud, dst) == 0 && removed) (*removed)++;
        }
    }
    closedir(d);
    /* 清理回收站中过期项 */
    d = opendir(trash);
    if (d) {
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char tp[1700];
            snprintf(tp, sizeof tp, "%s/%s", trash, e->d_name);
            struct stat st;
            if (stat(tp, &st) == 0 && st.st_mtime < cutoff) rm_rf(tp);
        }
        closedir(d);
    }
    return 0;
}

int xl_units_compact_deltas(const char *idx_dir, uint64_t threshold) {
    char base[1200];
    snprintf(base, sizeof base, "%s/units", idx_dir);
    DIR *d = opendir(base);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char fp[1600];
        snprintf(fp, sizeof fp, "%s/%s/delta.log", base, e->d_name);
        struct stat st;
        if (stat(fp, &st) == 0 && st.st_size > (off_t)threshold)
            xl_unit_delta_compact(fp);
    }
    closedir(d);
    return 0;
}

int xl_units_rollback(const char *idx_dir, int purge) {
    char dir[1200], path[1300], tmp[1400];
    snprintf(dir, sizeof dir, "%s", idx_dir);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0;
    snprintf(path, sizeof path, "%s/scanmode.json", dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fchmod(fileno(f), 0600);
    fputs("{\"version\":2,\"scanMode\":\"shared\",\"shareGroups\":[],"
          "\"isolatedGroups\":[],\"shareUsers\":[],\"isolatedUsers\":[]}", f);
    fflush(f); fsync(fileno(f));
    if (fclose(f) != 0 || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    if (purge) {
        char src[1200], dst[1400];
        snprintf(src, sizeof src, "%s/units", idx_dir);
        snprintf(dst, sizeof dst, "%s/units.trash-%ld", idx_dir, (long)time(NULL));
        rename(src, dst);
    }
    return 0;
}


int xl_units_check(const char *idx_dir, char bad[][XL_SCAN_UNIT_MAX], int max, int *checked) {
    if (checked) *checked = 0;
    int nbad = 0;
    char base[1200];
    snprintf(base, sizeof base, "%s/units", idx_dir);
    DIR *d = opendir(base);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char ud[1400];
        if (join_path(ud, sizeof ud, base, e->d_name) < 0) continue;
        DIR *u = opendir(ud);
        if (!u) continue;
        int bad_unit = 0;
        struct dirent *f;
        while ((f = readdir(u)) != NULL) {
            size_t l = strlen(f->d_name);
            if (l < 4 || strcmp(f->d_name + l - 4, ".idx")) continue;
            char fp[1600];
            if (join_path(fp, sizeof fp, ud, f->d_name) < 0) continue;
            xl_snap sn;
            if (snapshot_open(fp, &sn) != 0) bad_unit = 1;
            else snapshot_close(&sn);
            if (checked) (*checked)++;
        }
        closedir(u);
        if (bad_unit && nbad < max) {
            size_t bl = strlen(e->d_name);
            if (bl >= XL_SCAN_UNIT_MAX) bl = XL_SCAN_UNIT_MAX - 1;
            memcpy(bad[nbad], e->d_name, bl);
            bad[nbad][bl] = 0;
            nbad++;
        }
    }
    closedir(d);
    return nbad;
}
