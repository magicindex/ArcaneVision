#include "fsops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

static void set_err(char *err, size_t cap, const char *fmt, const char *a)
{
    if (!err || !cap) return;
    if (a) snprintf(err, cap, fmt, a);
    else snprintf(err, cap, "%s", fmt);
}

static int mkdir_p(const char *p, mode_t mode)
{
    char tmp[4096];
    size_t n = strlen(p);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, p, n + 1);
    for (char *s = tmp + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
        *s = '/';
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int copy_file(const char *src, const char *dst, char *err, size_t cap)
{
    int in = open(src, O_RDONLY | O_NOFOLLOW);
    if (in < 0) { set_err(err, cap, "打不开源文件: %s", src); return -1; }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); set_err(err, cap, "无法写入目标: %s", dst); return -1; }
    char buf[262144];
    ssize_t r;
    int ok = 0;
    while ((r = read(in, buf, sizeof buf)) > 0) {
        char *p = buf;
        ssize_t left = r;
        while (left > 0) {
            ssize_t w = write(out, p, (size_t)left);
            if (w <= 0) { set_err(err, cap, "写入失败: %s", dst); ok = -1; break; }
            p += w; left -= w;
        }
        if (ok) break;
    }
    if (r < 0 && !ok) { set_err(err, cap, "读取失败: %s", src); ok = -1; }
    struct stat st;
    if (fstat(in, &st) == 0) {
        if (!ok) fchmod(out, st.st_mode & 07777);
        if (geteuid() == 0) fchown(out, st.st_uid, st.st_gid);
        struct timespec ts[2] = { st.st_atim, st.st_mtim };
        futimens(out, ts);
    }
    close(in); close(out);
    return ok;
}

static int copy_tree(const char *src, const char *dst, char *err, size_t cap)
{
    struct stat st;
    if (lstat(src, &st) != 0) { set_err(err, cap, "源不存在: %s", src); return -1; }
    if (S_ISLNK(st.st_mode)) {
        char tgt[4096];
        ssize_t n = readlink(src, tgt, sizeof tgt - 1);
        if (n < 0) { set_err(err, cap, "读取链接失败: %s", src); return -1; }
        tgt[n] = 0;
        unlink(dst);
        if (symlink(tgt, dst) != 0) { set_err(err, cap, "创建链接失败: %s", dst); return -1; }
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 07777) != 0 && errno != EEXIST) {
            set_err(err, cap, "创建目录失败: %s", dst);
            return -1;
        }
        DIR *d = opendir(src);
        if (!d) { set_err(err, cap, "打不开目录: %s", src); return -1; }
        struct dirent *de;
        int rc = 0;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char a[4096], b[4096];
            snprintf(a, sizeof a, "%s/%s", src, de->d_name);
            snprintf(b, sizeof b, "%s/%s", dst, de->d_name);
            if (copy_tree(a, b, err, cap) != 0) { rc = -1; break; }
        }
        closedir(d);
        if (geteuid() == 0) chown(dst, st.st_uid, st.st_gid);
        chmod(dst, st.st_mode & 07777);
        struct timespec ts[2] = { st.st_atim, st.st_mtim };
        utimensat(AT_FDCWD, dst, ts, 0);
        return rc;
    }
    return copy_file(src, dst, err, cap);
}

int fs_copy_path(const char *src, const char *dst, char *err, size_t cap)
{
    if (!src || !dst || !*src || !*dst) { set_err(err, cap, "参数为空", NULL); return -1; }
    struct stat st;
    if (lstat(src, &st) != 0) { set_err(err, cap, "源不存在: %s", src); return -1; }
    return copy_tree(src, dst, err, cap);
}

int fs_remove_path(const char *p, char *err, size_t cap)
{
    struct stat st;
    if (lstat(p, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        if (!d) { set_err(err, cap, "打不开目录: %s", p); return -1; }
        struct dirent *de;
        int rc = 0;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char sub[4096];
            snprintf(sub, sizeof sub, "%s/%s", p, de->d_name);
            if (fs_remove_path(sub, err, cap) != 0) { rc = -1; break; }
        }
        closedir(d);
        if (rc == 0 && rmdir(p) != 0) { set_err(err, cap, "删除目录失败: %s", p); return -1; }
        return rc;
    }
    if (unlink(p) != 0) { set_err(err, cap, "删除失败: %s", p); return -1; }
    return 0;
}

int fs_move_path(const char *src, const char *dst, char *err, size_t cap)
{
    if (rename(src, dst) == 0) return 0;
    if (errno != EXDEV) {
        set_err(err, cap, "移动失败（%s）", strerror(errno));
        return -1;
    }
    if (copy_tree(src, dst, err, cap) != 0) {
        fs_remove_path(dst, NULL, 0);
        return -1;
    }
    return fs_remove_path(src, err, cap);
}

static void base_name(const char *p, char *out, size_t cap)
{
    const char *s = strrchr(p, '/');
    snprintf(out, cap, "%s", s ? s + 1 : p);
}

int fs_trash_path(const char *src, const char *trash, char *id, size_t idcap,
                  char *err, size_t cap)
{
    struct stat st;
    if (lstat(src, &st) != 0) { set_err(err, cap, "文件不存在: %s", src); return -1; }
    if (mkdir_p(trash, 0755) != 0) { set_err(err, cap, "无法创建回收站", NULL); return -1; }
    static unsigned seq = 0;
    for (int i = 0; i < 100; i++) {
        snprintf(id, idcap, "%lld-%u-%u", (long long)time(NULL), (unsigned)getpid(), seq++);
        char dir[4400], payload[4400], meta[4400];
        snprintf(dir, sizeof dir, "%s/%s", trash, id);
        if (mkdir(dir, 0755) == 0) {
            char name[512];
            base_name(src, name, sizeof name);
            {
                size_t dl = strlen(dir);
                if (dl > sizeof payload - 2) dl = sizeof payload - 2;
                memcpy(payload, dir, dl);
                payload[dl] = '/';
                snprintf(payload + dl + 1, sizeof payload - dl - 1, "%.400s", name);
            }
            if (fs_move_path(src, payload, err, cap) != 0) { rmdir(dir); return -1; }
            snprintf(meta, sizeof meta, "%.4200s/meta.txt", dir);
            FILE *f = fopen(meta, "w");
            if (f) { fprintf(f, "%s\n", src); fclose(f); }
            return 0;
        }
        if (errno != EEXIST) break;
    }
    set_err(err, cap, "无法创建回收站条目", NULL);
    return -1;
}

static int read_meta(const char *trash, const char *id, char *orig, size_t cap)
{
    char p[4400];
    snprintf(p, sizeof p, "%.4200s/%.60s/meta.txt", trash, id);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    if (!fgets(orig, (int)cap, f)) { fclose(f); return -1; }
    fclose(f);
    size_t n = strlen(orig);
    while (n && (orig[n - 1] == '\n' || orig[n - 1] == '\r')) orig[--n] = 0;
    return n ? 0 : -1;
}

static int list_payload(const char *trash, const char *id, char *name, size_t ncap)
{
    char dir[4400];
    snprintf(dir, sizeof dir, "%s/%s", trash, id);
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de;
    int found = -1;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!strcmp(de->d_name, "meta.txt")) continue;
        snprintf(name, ncap, "%s", de->d_name);
        found = 0;
        break;
    }
    closedir(d);
    return found;
}

static int ancestor_under_vol(const char *path)
{
    char tmp[4096], real[PATH_MAX];
    {
        size_t n = strlen(path);
        if (n >= sizeof tmp) return 0;
        memcpy(tmp, path, n + 1);
    }
    for (int i = 0; i < 64; i++) {
        if (realpath(tmp, real)) return strncmp(real, "/vol", 4) == 0;
        char *sl = strrchr(tmp, '/');
        if (!sl || sl == tmp) return 0;
        *sl = 0;
    }
    return 0;
}

int fs_restore(const char *trash, const char *id, char *err, size_t cap)
{
    if (strchr(id, '/') || strstr(id, "..")) { set_err(err, cap, "非法条目", NULL); return -1; }
    char orig[4096], name[512];
    if (read_meta(trash, id, orig, sizeof orig) != 0) { set_err(err, cap, "条目缺少元数据", NULL); return -1; }
    if (list_payload(trash, id, name, sizeof name) != 0) { set_err(err, cap, "条目内容缺失", NULL); return -1; }
    char payload[4400];
    snprintf(payload, sizeof payload, "%s/%s/%s", trash, id, name);
    struct stat st;
    if (lstat(orig, &st) == 0) { set_err(err, cap, "原位置已存在同名文件", NULL); return -1; }
    char parent[4400];
    snprintf(parent, sizeof parent, "%s", orig);
    char *slash = strrchr(parent, '/');
    if (!slash) { set_err(err, cap, "原路径无效", NULL); return -1; }
    *slash = 0;
    if (strncmp(parent, "/vol", 4) != 0 || !ancestor_under_vol(parent)) {
        set_err(err, cap, "原路径越界", NULL); return -1;
    }
    if (mkdir_p(parent, 0755) != 0) { set_err(err, cap, "无法创建原目录", NULL); return -1; }
    if (fs_move_path(payload, orig, err, cap) != 0) return -1;
    char dir[4400], meta[4400];
    snprintf(dir, sizeof dir, "%s/%s", trash, id);
    snprintf(meta, sizeof meta, "%.4200s/meta.txt", dir);
    unlink(meta);
    rmdir(dir);
    return 0;
}

int fs_empty_trash(const char *trash, int *removed, char *err, size_t cap)
{
    if (removed) *removed = 0;
    DIR *d = opendir(trash);
    if (!d) {
        if (errno == ENOENT) return 0;
        set_err(err, cap, "打不开回收站", NULL);
        return -1;
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[4096];
        snprintf(p, sizeof p, "%s/%s", trash, de->d_name);
        if (fs_remove_path(p, err, cap) == 0 && removed) (*removed)++;
    }
    closedir(d);
    return 0;
}

int fs_trash_scan(const char *trash, fs_trash_item *out, int max)
{
    DIR *d = opendir(trash);
    if (!d) return 0;
    int n = 0;
    struct dirent *de;
    while (n < max && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        fs_trash_item *it = &out[n];
        memset(it, 0, sizeof *it);
        snprintf(it->id, sizeof it->id, "%.63s", de->d_name);
        char dir[4400], payload[4400];
        snprintf(dir, sizeof dir, "%.4200s/%.60s", trash, de->d_name);
        if (list_payload(trash, de->d_name, it->name, sizeof it->name) != 0) continue;
        if (read_meta(trash, de->d_name, it->orig, sizeof it->orig) != 0) continue;
        {
            size_t dl = strlen(dir);
            if (dl > sizeof payload - 2) dl = sizeof payload - 2;
            memcpy(payload, dir, dl);
            payload[dl] = '/';
            snprintf(payload + dl + 1, sizeof payload - dl - 1, "%.400s", it->name);
        }
        struct stat st;
        if (stat(payload, &st) == 0) {
            it->size = (long long)st.st_size;
            it->mtime = (long long)st.st_mtime;
        }
        struct stat ds;
        if (stat(dir, &ds) == 0) it->deleted = (long long)ds.st_mtime;
        n++;
    }
    closedir(d);
    return n;
}
