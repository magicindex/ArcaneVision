#include "fsops.h"
#include "xl.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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
    int in = open(src, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (in < 0) { set_err(err, cap, "打不开源文件: %s", src); return -1; }
    struct stat sst;
    if (fstat(in, &sst) != 0 || !S_ISREG(sst.st_mode)) {
        close(in);
        set_err(err, cap, "不支持的文件类型: %s", src);
        return -1;
    }
    /* O_EXCL|O_NOFOLLOW：避免并发替换目标为软链造成越界覆盖 */
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
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
    if (!ok && fsync(out) != 0) { set_err(err, cap, "刷新失败: %s", dst); ok = -1; }
    close(in);
    if (close(out) != 0 && !ok) { set_err(err, cap, "关闭失败: %s", dst); ok = -1; }
    if (ok) unlink(dst);   /* 失败不留半文件 */
    return ok;
}

static int copy_tree(const char *src, const char *dst, uid_t uid, char *err, size_t cap)
{
    struct stat st;
    if (lstat(src, &st) != 0) { set_err(err, cap, "源不存在: %s", src); return -1; }
    /* 复制目录时逐级复核权限，防止从"可列父目录"绕过子目录/文件 ACL */
    if (uid != 0) {
        if (S_ISDIR(st.st_mode) && !perm_dir_listable(uid, src)) {
            set_err(err, cap, "没有源目录读权限: %s", src); return -1;
        }
        if (!S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) && !perm_file_readable(uid, src)) {
            set_err(err, cap, "没有源文件读权限: %s", src); return -1;
        }
    }
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
            if (copy_tree(a, b, uid, err, cap) != 0) { rc = -1; break; }
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

int fs_copy_path(const char *src, const char *dst, uid_t uid, char *err, size_t cap)
{
    if (!src || !dst || !*src || !*dst) { set_err(err, cap, "参数为空", NULL); return -1; }
    struct stat st;
    if (lstat(src, &st) != 0) { set_err(err, cap, "源不存在: %s", src); return -1; }
    /* 目标目录不能在源目录内部，否则递归复制会自我放大直到磁盘写满 */
    {
        char rsrc[PATH_MAX], rdir[PATH_MAX], dp[PATH_MAX];
        if (!realpath(src, rsrc)) { set_err(err, cap, "源路径无法解析: %s", src); return -1; }
        snprintf(dp, sizeof dp, "%s", dst);
        char *sl = strrchr(dp, '/');
        if (!sl || sl == dp) { set_err(err, cap, "目标路径非法: %s", dst); return -1; }
        *sl = 0;
        if (!realpath(dp, rdir)) { set_err(err, cap, "目标目录无法解析: %s", dp); return -1; }
        size_t rl = strlen(rsrc);
        if (!strncmp(rdir, rsrc, rl) && (rdir[rl] == '/' || rdir[rl] == 0)) {
            set_err(err, cap, "不能复制到源目录内部", NULL); return -1;
        }
    }
    return copy_tree(src, dst, uid, err, cap);
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

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif

int fs_move_path(const char *src, const char *dst, char *err, size_t cap)
{
#ifdef SYS_renameat2
    /* 不覆盖已存在的目标，避免 check-then-rename 竞争导致覆盖他人文件 */
    if (syscall(SYS_renameat2, AT_FDCWD, src, AT_FDCWD, dst, RENAME_NOREPLACE) == 0) return 0;
    if (errno == EEXIST) { set_err(err, cap, "目标已存在", NULL); return -1; }
    if (errno != ENOSYS && errno != EINVAL && errno != EXDEV) {
        set_err(err, cap, "移动失败（%s）", strerror(errno));
        return -1;
    }
#endif
    if (rename(src, dst) == 0) return 0;
    if (errno != EXDEV) {
        set_err(err, cap, "移动失败（%s）", strerror(errno));
        return -1;
    }
    if (copy_tree(src, dst, 0, err, cap) != 0) {
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

int fs_trash_path(const char *src, const char *trash, uid_t owner,
                  char *id, size_t idcap, char *err, size_t cap)
{
    struct stat st;
    if (lstat(src, &st) != 0) { set_err(err, cap, "文件不存在: %s", src); return -1; }
    if (mkdir_p(trash, 0755) != 0) { set_err(err, cap, "无法创建回收站", NULL); return -1; }
    static unsigned seq = 0;
    for (int i = 0; i < 100; i++) {
        unsigned sn = __sync_fetch_and_add(&seq, 1);
        snprintf(id, idcap, "%lld-%u-%u", (long long)time(NULL), (unsigned)getpid(), sn);
        char dir[4400], payload[4400];
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
            /* 元数据放在 .meta/<id>，不再与负载同目录，避免源文件名叫 meta.txt 时被覆盖 */
            char mdir[4400], meta[4600], tmp[4700];
            snprintf(mdir, sizeof mdir, "%s/.meta", trash);
            if (mkdir_p(mdir, 0700) != 0) { fs_move_path(payload, src, NULL, 0); rmdir(dir);
                set_err(err, cap, "无法创建元数据目录", NULL); return -1; }
            snprintf(meta, sizeof meta, "%.4300s/%.60s", mdir, id);
            snprintf(tmp, sizeof tmp, "%s.tmp", meta);
            FILE *f = fopen(tmp, "w");
            if (!f) { fs_move_path(payload, src, NULL, 0); rmdir(dir);
                set_err(err, cap, "无法写入元数据", NULL); return -1; }
            fchmod(fileno(f), 0600);
            fprintf(f, "%s\n%u\n", src, (unsigned)owner);
            fflush(f);
            fsync(fileno(f));
            fclose(f);
            if (rename(tmp, meta) != 0) { unlink(tmp); fs_move_path(payload, src, NULL, 0); rmdir(dir);
                set_err(err, cap, "无法保存元数据", NULL); return -1; }
            return 0;
        }
        if (errno != EEXIST) break;
    }
    set_err(err, cap, "无法创建回收站条目", NULL);
    return -1;
}

/* 返回 0 成功；owner=(uid_t)-1 表示旧格式无属主信息（仅管理员可操作） */
static int read_meta(const char *trash, const char *id, char *orig, size_t cap, uid_t *owner)
{
    char p[4400];
    snprintf(p, sizeof p, "%.4200s/.meta/%.60s", trash, id);
    FILE *f = fopen(p, "r");
    if (f) {
        if (!fgets(orig, (int)cap, f)) { fclose(f); return -1; }
        unsigned u = 0;
        if (fscanf(f, "%u", &u) != 1) u = 0;
        fclose(f);
        *owner = (uid_t)u;
    } else {
        /* 兼容 0.7.3 及以前的 <trash>/<id>/meta.txt */
        snprintf(p, sizeof p, "%.4200s/%.60s/meta.txt", trash, id);
        f = fopen(p, "r");
        if (!f) return -1;
        if (!fgets(orig, (int)cap, f)) { fclose(f); return -1; }
        fclose(f);
        *owner = (uid_t)-1;
    }
    size_t n = strlen(orig);
    while (n && (orig[n - 1] == '\n' || orig[n - 1] == '\r')) orig[--n] = 0;
    return n ? 0 : -1;
}

static int list_payload(const char *trash, const char *id, char *name, size_t ncap)
{
    char dir[4400];
    snprintf(dir, sizeof dir, "%s/%s", trash, id);
    char meta[4400];
    snprintf(meta, sizeof meta, "%.4200s/.meta/%.60s", trash, id);
    int newfmt = (access(meta, F_OK) == 0);
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de;
    int found = -1;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        /* 旧布局把元数据放在负载同级 meta.txt；新布局元数据在 .meta/<id>，
           此时 meta.txt 是合法的用户文件名，不能跳过 */
        if (!newfmt && !strcmp(de->d_name, "meta.txt")) continue;
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

int fs_restore(const char *trash, const char *id, uid_t uid, char *err, size_t cap)
{
    if (strchr(id, '/') || strstr(id, "..")) { set_err(err, cap, "非法条目", NULL); return -1; }
    char orig[4096], name[512];
    uid_t owner = (uid_t)-1;
    if (read_meta(trash, id, orig, sizeof orig, &owner) != 0) { set_err(err, cap, "条目缺少元数据", NULL); return -1; }
    if (uid != 0 && (owner == (uid_t)-1 || owner != uid)) { set_err(err, cap, "无权恢复他人条目", NULL); return -1; }
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
    if (uid != 0) {
        /* 目标父目录可能已被删除：找到最近的存在祖先，要求用户对其可写 */
        char chk[4400];
        snprintf(chk, sizeof chk, "%s", parent);
        struct stat cst;
        while (stat(chk, &cst) != 0) {
            char *s = strrchr(chk, '/');
            if (!s || s == chk) break;
            *s = 0;
        }
        if (stat(chk, &cst) == 0 && !perm_dir_writable(uid, chk)) {
            set_err(err, cap, "原目录不可写", NULL);
            return -1;
        }
    }
    if (mkdir_p(parent, 0755) != 0) { set_err(err, cap, "无法创建原目录", NULL); return -1; }
    if (fs_move_path(payload, orig, err, cap) != 0) return -1;
    char dir[4400], meta[4400], legacy[4400];
    snprintf(dir, sizeof dir, "%s/%s", trash, id);
    snprintf(meta, sizeof meta, "%.4200s/.meta/%.60s", trash, id);
    snprintf(legacy, sizeof legacy, "%.4200s/%.60s/meta.txt", trash, id);
    unlink(meta);
    unlink(legacy);
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
    int rc = 0;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!strcmp(de->d_name, ".meta")) continue;
        char p[4096];
        snprintf(p, sizeof p, "%s/%s", trash, de->d_name);
        if (fs_remove_path(p, err, cap) == 0) {
            if (removed) (*removed)++;
        } else {
            rc = -1;   /* 保留元数据，失败条目仍可恢复 */
        }
    }
    closedir(d);
    if (rc == 0) {
        char mdir[4400];
        snprintf(mdir, sizeof mdir, "%s/.meta", trash);
        fs_remove_path(mdir, NULL, 0);
    }
    return rc;
}

int fs_trash_scan(const char *trash, uid_t uid, fs_trash_item *out, int max)
{
    DIR *d = opendir(trash);
    if (!d) return 0;
    int n = 0;
    struct dirent *de;
    while (n < max && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!strcmp(de->d_name, ".meta")) continue;
        fs_trash_item *it = &out[n];
        memset(it, 0, sizeof *it);
        snprintf(it->id, sizeof it->id, "%.63s", de->d_name);
        char dir[4400], payload[4400];
        snprintf(dir, sizeof dir, "%.4200s/%.60s", trash, de->d_name);
        if (list_payload(trash, de->d_name, it->name, sizeof it->name) != 0) continue;
        if (read_meta(trash, de->d_name, it->orig, sizeof it->orig, &it->owner) != 0) continue;
        if (uid != 0 && (it->owner == (uid_t)-1 || it->owner != uid)) continue;
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
