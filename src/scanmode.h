/* 扫描模式策略：shared / isolated / hybrid。
 * 纯解析与解析结果，不依赖扫描器；P1 只负责配置、分类与回退规则。 */
#ifndef XL_SCANMODE_H
#define XL_SCANMODE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define XL_SCAN_ARRAY_MAX 64
#define XL_SCAN_UNIT_MAX  32

typedef enum {
    XL_SCAN_SHARED = 0,
    XL_SCAN_ISOLATED = 1,
    XL_SCAN_HYBRID = 2
} xl_scan_mode;

typedef struct {
    int         version;
    xl_scan_mode mode;
    gid_t share_groups[XL_SCAN_ARRAY_MAX];
    int   n_share_groups;
    gid_t isolated_groups[XL_SCAN_ARRAY_MAX];
    int   n_isolated_groups;
    uid_t share_users[XL_SCAN_ARRAY_MAX];
    int   n_share_users;
    uid_t isolated_users[XL_SCAN_ARRAY_MAX];
    int   n_isolated_users;
} xl_scan_policy;

typedef enum {
    XL_UNIT_SHARED = 0,
    XL_UNIT_USER = 1,
    XL_UNIT_GROUP = 2
} xl_unit_kind;

typedef struct {
    char        key[XL_SCAN_UNIT_MAX];   /* shared / u<uid> / g<gid> */
    xl_unit_kind kind;
    uid_t       uid;
    gid_t       gid;
    int         fallback;                 /* 1 = 未分类回退 shared */
} xl_scan_unit;

/* 读 scope.json；文件缺失或字段非法时回落默认（shared、空分类）。 */
int xl_scan_policy_load(const char *path, xl_scan_policy *p);
const char *xl_scan_mode_name(xl_scan_mode m);

/* 返回 0..n；输出单元可能多个（多隔离组）。 */
int xl_scan_resolve(const xl_scan_policy *p, uid_t uid, const gid_t *gids,
                    int ngids, xl_scan_unit *out, int cap);

/* idx/units 布局 */
int xl_scan_unit_dir(const char *idx_dir, const char *unit, char *out, size_t cap);
int xl_units_init(const char *idx_dir, char *err, size_t cap);
int xl_units_capacity(const char *idx_dir, int *count, uint64_t *bytes, uint64_t *delta_bytes);
int xl_units_gc(const char *idx_dir, const char *const *active, int nactive,
                int grace_days, int purge, int *removed);
int xl_units_compact_deltas(const char *idx_dir, uint64_t threshold);
int xl_units_check(const char *idx_dir, char bad[][XL_SCAN_UNIT_MAX], int max, int *checked);
int xl_units_rollback(const char *idx_dir, int purge);

#endif
