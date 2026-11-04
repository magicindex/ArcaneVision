/* 玄览 / Arcane Vision — 全盘文件索引引擎
 * 公共类型与接口。原创实现，不包含任何第三方项目的源代码。
 */
#ifndef XL_H
#define XL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stddef.h>
#include <stdbool.h>

#define XL_MAGIC       "XLANIDX"
/* v2：记录内 size/mtime 为真实值；旧索引启动时重建 */
#define XL_VERSION     2u
#define XL_HDR_SIZE    128u
#define XL_ALIGN       4096u
#define XL_BLOCK       128u   /* 倒排块内条目数 */
#define XL_NAME_MAX    255u

/* 记录标志 */
#define XL_F_DIR   0x01
#define XL_F_LINK  0x02

/* 20 字节定长记录：名字只存最后一段，路径靠 parent 链还原 */
typedef struct {
    uint32_t parent;    /* 记录号；根为 XL_ROOT */
    uint32_t name_off;  /* 名字池偏移 */
    uint32_t size;      /* 低 32 位；--stat 时才填 */
    uint32_t mtime;     /* 秒；--stat 时才填 */
    uint16_t name_len;
    uint8_t  flags;
    uint8_t  reserved;
} xl_rec;

#define XL_ROOT 0xFFFFFFFFu

_Static_assert(sizeof(xl_rec) == 20, "xl_rec must stay 20 bytes");

/* 快照头：定长 128 字节，各段 4KB 对齐 */
typedef struct {
    char     magic[8];        /* "XLANIDX" */
    uint32_t version;
    uint32_t flags;
    uint64_t rec_count;
    uint64_t rec_off;
    uint64_t names_bytes;
    uint64_t names_off;
    uint32_t tri_count;       /* 出现过的 trigram 个数 */
    uint32_t pad0;
    uint64_t tri_ids_off;     /* uint32[tri_count]，升序 trigram 值 */
    uint64_t tri_ranges_off;  /* xl_range[tri_count] */
    uint64_t postings_off;
    uint64_t postings_bytes;
    uint64_t root_off;        /* 扫描根路径，不含结尾 '/' */
    uint32_t root_len;
    uint32_t pad1;
    uint64_t built_at;        /* unix 秒 */
    uint64_t scan_ms;
    uint64_t reserved2;
} xl_hdr;

_Static_assert(sizeof(xl_hdr) == 128, "xl_hdr must stay 128 bytes");

typedef struct {
    uint32_t off;   /* postings 段内字节偏移（块序列起点） */
    uint32_t count; /* 该 trigram 的条目数 */
} xl_range;

/* 扫描产物 */
typedef struct {
    xl_rec *recs;
    uint32_t n, cap;
    char   *names;
    size_t  names_len, names_cap;
} xl_store;

/* 内存索引（构建期） */
typedef struct {
    uint32_t *tri_ids;
    xl_range *tri_ranges;
    uint32_t  tri_count;
    uint32_t *postings;     /* 构建期：定宽 id 数组，按 trigram 连续存放 */
    uint32_t  postings_n;
    uint8_t  *blob;         /* 编码后：块序列 */
    size_t    blob_len, blob_cap;
    uint64_t  build_ms;
} xl_index;

/* ---- scan.c ---- */
void store_init(xl_store *s);
void store_free(xl_store *s);
int  scan_tree(const char *root, xl_store *s, int want_stat, int verbose);
void scan_add_exclude(const char *prefix);
int  scan_has_exclude(void);
uint32_t store_push_root(xl_store *s, uint8_t flags);
uint32_t store_push(xl_store *s, uint32_t parent, const char *name, size_t len,
                    uint8_t flags);
uint32_t store_push_meta(xl_store *s, uint32_t parent, const char *name, size_t len,
                         uint8_t flags, uint32_t size, uint32_t mtime);

/* ---- fsreader_ext4.c ---- */
int fsreader_ext4_scan(const char *dev, const char *mount_root, xl_store *s,
                       int verbose);
void store_shrink(xl_store *s);

/* ---- index.c ---- */
void index_init(xl_index *ix);
void index_free(xl_index *ix);
int  index_build(xl_index *ix, const xl_store *s, int verbose);
/* 返回该 trigram 的 id 序列（回调式），count 为总数 */
uint32_t tri_count_of(const xl_index *ix, uint32_t tg);
int  tri_foreach(const xl_index *ix, uint32_t tg,
                 void (*cb)(uint32_t id, void *ctx), void *ctx);

/* ---- snapshot.c ---- */
int  snapshot_save(const char *path, const xl_store *s, const xl_index *ix,
                   const char *root, uint64_t scan_ms);
typedef struct {
    void    *map;
    size_t   map_len;
    xl_hdr   h;
    xl_rec  *recs;
    char    *names;
    uint32_t *tri_ids;
    xl_range *tri_ranges;
    const uint8_t *postings;
    const char *root;
} xl_snap;
int  snapshot_open(const char *path, xl_snap *sn);
void snapshot_close(xl_snap *sn);

/* 扫描进度（供服务层展示） */
extern volatile uint64_t xl_scan_records;

/* ---- query.c ---- */
typedef struct {
    uint32_t max_results;
    int      json;
    int      dirs_only;
    int      files_only;
} xl_query_opts;

int query_run(const xl_snap *sn, const char *needle, const xl_query_opts *o);
/* 收集命中记录号（malloc 返回，调用方 free）；无命中返回 NULL 且 *n_out=0 */
uint32_t *query_hits(const xl_snap *sn, const char *needle, const xl_query_opts *o,
                     uint32_t *n_out, int *truncated_out, uint64_t *elapsed_ms,
                     uint64_t *total_out);
void snap_path(const xl_snap *sn, uint32_t id, char *out, size_t cap);
uint32_t tri_lookup_sorted(const uint32_t *ids, uint32_t n, uint32_t tg);

/* ---- httpd.c ---- */
typedef struct {
    char sock_path[512];
    char ui_dir[512];
    char idx_dir[1024];
    char roots[1024];
    char bind_addr[64];
    int  port;
    int  no_tcp;
    int  rebuild_hours;
} xl_svc_opts;
int httpd_serve(const xl_svc_opts *o);

/* ---- perm.c ---- */
int perm_check(uid_t uid, const char *path, int need, int is_dir);
int perm_dir_listable(uid_t uid, const char *dir);
int perm_file_readable(uid_t uid, const char *path);
int perm_dir_writable(uid_t uid, const char *dir);
int perm_parent_writable(uid_t uid, const char *path);
int perm_mode_readable(uid_t uid, const char *path, const struct stat *st);
extern int perm_strict_home;
void perm_set_strict_home(int on);

/* ---- util.c ---- */
void  *xmalloc(size_t n);
void  *xcalloc(size_t n, size_t m);
void  *xrealloc(void *p, size_t n);
void   lower_copy(char *dst, const char *src, size_t len);
int    mem_ci(const char *hay, size_t hlen, const char *ndl, size_t nlen);
const char *memmem_ci(const char *hay, size_t hlen, const char *ndl, size_t nlen);
uint64_t now_ms(void);

/* ---- 增量层（fanotify + delta） ---- */
typedef struct { uint32_t size; uint32_t mtime; uint8_t flags; } xl_store_stat;
typedef struct { char *path; uint32_t size; uint32_t mtime; uint8_t flags; } xl_live_hit;

int    live_start(const char *roots_csv, int verbose);
void   live_stop(void);
void   live_clear(void);
size_t live_delta_count(size_t *dels);
size_t live_query(const char *needle, xl_live_hit **out);
size_t live_dir(const char *dir, xl_live_hit **out);
int    live_is_deleted(const char *path);
int    live_overflowed(void);

/* ---- 权限（trimacl / POSIX ACL） ---- */
#endif
