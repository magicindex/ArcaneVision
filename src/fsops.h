#ifndef XL_FSOPS_H
#define XL_FSOPS_H
#include <stddef.h>
#include <sys/types.h>

int fs_copy_path(const char *src, const char *dst, uid_t uid, char *err, size_t cap);
int fs_move_path(const char *src, const char *dst, char *err, size_t cap);
int fs_remove_path(const char *p, char *err, size_t cap);
int fs_trash_path(const char *src, const char *trash, uid_t owner,
                  char *id, size_t idcap, char *err, size_t cap);
int fs_restore(const char *trash, const char *id, uid_t uid,
               char *err, size_t cap);
int fs_empty_trash(const char *trash, int *removed, char *err, size_t cap);

typedef struct {
    char id[64];
    char name[512];
    char orig[4096];
    uid_t owner;
    long long size;
    long long mtime;
    long long deleted;
} fs_trash_item;

/* uid 非 0 时只返回该用户的条目 */
int fs_trash_scan(const char *trash, uid_t uid, fs_trash_item *out, int max);
#endif
