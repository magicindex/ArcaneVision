/* 按单元 Scope 独立扫描并写隔离索引（P5）。 */
#ifndef XL_UNITBUILD_H
#define XL_UNITBUILD_H

#include "xl.h"

typedef struct {
    int         kind;       /* 1=user 2=group */
    uid_t       uid;
    gid_t       gid;
    const uid_t *members;   /* group 成员；user 可空 */
    int         nmembers;
    const char *root;       /* 当前扫描根，用于根目录文件的父级判断 */
} xl_unit_subject;

/* 从 root 独立遍历，只保留该主体可穿越/可读的目录与文件。 */
int xl_unit_scan_to_file(const char *root, const xl_unit_subject *subj,
                         const char *out_path);
/* 只重扫 changed 子树，把旧快照中该子树外的记录合并后原子替换。
 * changed 为文件时使用其父目录；目录必须是旧快照中的结构节点。 */
int xl_unit_patch_to_file(const char *root, const char *changed,
                          const xl_unit_subject *subj,
                          const char *old_path, const char *out_path);

#endif
