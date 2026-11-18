/* 单元级增量日志：快照之外持久化 add/delete，重启后仍可合并查询。 */
#ifndef XL_UNITDELTA_H
#define XL_UNITDELTA_H

#include "xl.h"

typedef struct {
    char    *path;
    uint32_t size;
    uint32_t mtime;
    uint8_t  flags;
    uint8_t  deleted;
} xl_unit_delta;

typedef struct {
    xl_unit_delta *v;
    size_t n, cap;
} xl_unit_delta_set;

int  xl_unit_delta_append(const char *file, const char *path, int deleted,
                          uint8_t flags, uint32_t size, uint32_t mtime);
/* 批量写：一次 open/锁，适合事件队列消费。 */
int  xl_unit_delta_append_batch(const char *file, const xl_unit_delta *items, size_t n);
int  xl_unit_delta_load(const char *file, xl_unit_delta_set *out);
/* 按路径保留最后一条事件，原子重写；用于控制 delta 体积。 */
int  xl_unit_delta_compact(const char *file);
void xl_unit_delta_free(xl_unit_delta_set *out);
int  xl_unit_delta_deleted(const xl_unit_delta_set *s, const char *path);
size_t xl_unit_delta_query(const xl_unit_delta_set *s, const char *needle,
                           xl_live_hit **out);

#endif
