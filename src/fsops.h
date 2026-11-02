#ifndef XL_FSOPS_H
#define XL_FSOPS_H
#include <stddef.h>

int fs_copy_path(const char *src, const char *dst, char *err, size_t cap);
int fs_move_path(const char *src, const char *dst, char *err, size_t cap);
int fs_remove_path(const char *p, char *err, size_t cap);
int fs_trash_path(const char *src, const char *trash, char *id, size_t idcap,
                  char *err, size_t cap);
int fs_restore(const char *trash, const char *id, char *err, size_t cap);
int fs_empty_trash(const char *trash, int *removed, char *err, size_t cap);

typedef struct {
    char id[64];
    char name[512];
    char orig[4096];
    long long size;
    long long mtime;
    long long deleted;
} fs_trash_item;

int fs_trash_scan(const char *trash, fs_trash_item *out, int max);
#endif
