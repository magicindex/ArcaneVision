/* 玄览 / Arcane Vision — CLI
 *   xuanlan build --root DIR --out FILE [--exclude DIR] [--stat] [-v]
 *   xuanlan query --index FILE [-n N] [--json] [--dirs|--files] TERM
 *   xuanlan info  --index FILE
 */
#include "xl.h"
#include "scanmode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 从挂载表里找出设备对应的挂载点，用于还原绝对路径 */
static char *mount_for(const char *dev) {
    FILE *f = fopen("/proc/self/mounts", "r");
    if (!f) return NULL;
    char line[4096], d[2048], mnt[2048];
    char *found = NULL;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%2047s %2047s", d, mnt) != 2) continue;
        if (strcmp(d, dev) != 0) continue;
        /* 处理 \040 等转义 */
        char *out = mnt, *in = mnt;
        while (*in) {
            if (in[0] == '\\' && in[1] >= '0' && in[1] <= '7' && in[2] && in[3]) {
                *out++ = (char)(((in[1] - '0') << 6) | ((in[2] - '0') << 3) | (in[3] - '0'));
                in += 4;
            } else *out++ = *in++;
        }
        *out = 0;
        found = strdup(mnt);
        break;
    }
    fclose(f);
    return found;
}

static void usage(void) {
    fputs("玄览 / Arcane Vision — 全盘文件索引\n"
          "用法:\n"
          "  xuanlan build --root DIR --out FILE [--exclude DIR]... [--include-hidden] [--no-stat] [-v]\n"
          "  xuanlan query --index FILE [-n N] [--json] [--dirs|--files] TERM\n"
          "  xuanlan info  --index FILE\n"
          "  xuanlan dump  --index FILE\n"
          "  xuanlan serve [--port N] [--bind ADDR] [--socket PATH] [--no-socket]\n"
          "                [--no-tcp] [--ui DIR] [--index-dir DIR]\n"
          "                [--roots /vol1,/vol3,...] [--exclude DIR]...\n"
          "  xuanlan scanmode --config FILE --uid N [--gids 1,2] [--json]\n"
          "  xuanlan units init --index-dir DIR\n"
          "  xuanlan units stats --index-dir DIR\n"
          "  xuanlan units gc --index-dir DIR [--active u1000,g9001] [--grace-days 7] [--purge]\n"
          "  xuanlan units rollback --index-dir DIR [--purge]\n"
          "  xuanlan units compact --index-dir DIR [--threshold-mb 4]\n"
          "  xuanlan units check --index-dir DIR\n", stderr);
}

/* 默认索引卷：只取 /proc/self/mounts 里实际挂载的 /vol1–/vol8，避免把残留空目录当卷 */
static void detect_default_roots(char *out, size_t cap) {
    int seen[9] = {0};
    size_t n = 0;
    out[0] = 0;
    FILE *f = fopen("/proc/self/mounts", "r");
    if (f) {
        char line[2048];
        while (fgets(line, sizeof line, f)) {
            char *sp = strchr(line, ' ');
            if (!sp) continue;
            char *mnt = sp + 1;
            char *e = strchr(mnt, ' ');
            if (!e) continue;
            *e = 0;
            if (strncmp(mnt, "/vol", 4) != 0) continue;
            char *p = mnt + 4;
            if (*p < '1' || *p > '8') continue;      /* 排除 /vol0、/vol00、/vol02 */
            int idx = *p - '0';
            p++;
            if (*p) continue;                        /* 排除 /vol10 及更长名字 */
            if (seen[idx]) continue;
            seen[idx] = 1;
            int w = snprintf(out + n, cap - n, "%s/vol%d", n ? "," : "", idx);
            if (w < 0 || (size_t)w >= cap - n) break;
            n += (size_t)w;
        }
        fclose(f);
    }
    if (!n) snprintf(out, cap, "/vol1,/vol2,/vol3,/vol4,/vol5,/vol6,/vol7,/vol8");
}

static int parse_id_list(const char *s, unsigned int *out, int max) {
    int n = 0;
    while (s && *s && n < max) {
        while (*s == ' ' || *s == ',') s++;
        if (!*s) break;
        char *e = NULL;
        unsigned long v = strtoul(s, &e, 10);
        if (e == s) break;
        out[n++] = (unsigned int)v;
        s = e;
    }
    return n;
}

static int cmd_scanmode(int argc, char **argv) {
    const char *cfg = NULL;
    int uid = -1, json = 0;
    unsigned int gidv[XL_SCAN_ARRAY_MAX];
    int ngids = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) cfg = argv[++i];
        else if (!strcmp(argv[i], "--uid") && i + 1 < argc) uid = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gids") && i + 1 < argc) ngids = parse_id_list(argv[++i], gidv, XL_SCAN_ARRAY_MAX);
        else if (!strcmp(argv[i], "--json")) json = 1;
        else { fprintf(stderr, "xuanlan scanmode: unknown %s\n", argv[i]); return 2; }
    }
    if (uid < 0) {
        fprintf(stderr, "用法: xuanlan scanmode --config FILE --uid N [--gids 1,2] [--json]\n");
        return 2;
    }
    xl_scan_policy p;
    if (xl_scan_policy_load(cfg, &p) < 0) {
        fprintf(stderr, "xuanlan scanmode: 无法读取 %s\n", cfg ? cfg : "(default)");
        return 1;
    }
    gid_t gids[XL_SCAN_ARRAY_MAX];
    for (int i = 0; i < ngids; i++) gids[i] = (gid_t)gidv[i];
    xl_scan_unit units[XL_SCAN_ARRAY_MAX];
    int n = xl_scan_resolve(&p, (uid_t)uid, gids, ngids, units, XL_SCAN_ARRAY_MAX);
    if (json) {
        printf("{\"mode\":\"%s\",\"uid\":%d,\"units\":[", xl_scan_mode_name(p.mode), uid);
        for (int i = 0; i < n; i++) {
            if (i) putchar(',');
            const char *kind = units[i].kind == XL_UNIT_GROUP ? "group"
                             : (units[i].kind == XL_UNIT_USER ? "user" : "shared");
            printf("{\"unit\":\"%s\",\"kind\":\"%s\",\"gid\":%u,\"uid\":%u,\"fallback\":%s}",
                   units[i].key, kind, (unsigned)units[i].gid, (unsigned)units[i].uid,
                   units[i].fallback ? "true" : "false");
        }
        puts("]}");
    } else {
        for (int i = 0; i < n; i++)
            printf("mode=%s uid=%d unit=%s kind=%s fallback=%d\n",
                   xl_scan_mode_name(p.mode), uid, units[i].key,
                   units[i].kind == XL_UNIT_GROUP ? "group" :
                   (units[i].kind == XL_UNIT_USER ? "user" : "shared"),
                   units[i].fallback);
    }
    return 0;
}

static int cmd_units(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "用法: xuanlan units init|stats|gc|rollback|compact ...\n");
        return 2;
    }
    const char *sub = argv[2];
    const char *dir = NULL;
    const char *active_csv = NULL;
    int grace = 7, purge = 0, threshold_mb = 4;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--index-dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--active") && i + 1 < argc) active_csv = argv[++i];
        else if (!strcmp(argv[i], "--grace-days") && i + 1 < argc) grace = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threshold-mb") && i + 1 < argc) threshold_mb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--purge")) purge = 1;
        else { fprintf(stderr, "xuanlan units: unknown %s\n", argv[i]); return 2; }
    }
    if (!dir) { fprintf(stderr, "xuanlan units: --index-dir required\n"); return 2; }

    if (!strcmp(sub, "init")) {
        char err[256];
        if (xl_units_init(dir, err, sizeof err) < 0) { fprintf(stderr, "xuanlan units: %s\n", err); return 1; }
        printf("units: %s/units\n", dir);
        return 0;
    }
    if (!strcmp(sub, "stats")) {
        int count; uint64_t bytes, delta;
        xl_units_capacity(dir, &count, &bytes, &delta);
        printf("units=%d bytes=%llu delta_bytes=%llu\n", count,
               (unsigned long long)bytes, (unsigned long long)delta);
        return 0;
    }
    if (!strcmp(sub, "check")) {
        char bad[XL_SCAN_ARRAY_MAX][XL_SCAN_UNIT_MAX];
        int checked = 0;
        int n = xl_units_check(dir, bad, XL_SCAN_ARRAY_MAX, &checked);
        printf("units check: checked=%d bad=%d\n", checked, n);
        for (int i = 0; i < n; i++) printf("bad %s\n", bad[i]);
        return n ? 1 : 0;
    }
    if (!strcmp(sub, "compact")) {
        if (threshold_mb < 1) threshold_mb = 1;
        xl_units_compact_deltas(dir, (uint64_t)threshold_mb * 1024 * 1024);
        printf("units compact: threshold=%d MB\n", threshold_mb);
        return 0;
    }
    if (!strcmp(sub, "rollback")) {
        if (xl_units_rollback(dir, purge) < 0) { fprintf(stderr, "xuanlan units: rollback failed\n"); return 1; }
        printf("units rollback: scanMode=shared%s\n", purge ? " (units moved to trash)" : "");
        return 0;
    }
    if (!strcmp(sub, "gc")) {
        char *buf = active_csv ? strdup(active_csv) : NULL;
        const char *active[XL_SCAN_ARRAY_MAX];
        int n = 0;
        if (buf) {
            char *save = NULL;
            for (char *t = strtok_r(buf, ",", &save); t && n < XL_SCAN_ARRAY_MAX; t = strtok_r(NULL, ",", &save)) {
                while (*t == ' ') t++;
                if (*t) active[n++] = t;
            }
        }
        int removed = 0;
        xl_units_gc(dir, active, n, grace, purge, &removed);
        printf("units gc: removed=%d grace=%d purge=%d\n", removed, grace, purge);
        free(buf);
        return 0;
    }
    fprintf(stderr, "xuanlan units: unknown subcommand %s\n", sub);
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];

    if (strcmp(cmd, "scanmode") == 0) return cmd_scanmode(argc, argv);
    if (strcmp(cmd, "units") == 0) return cmd_units(argc, argv);

    if (strcmp(cmd, "build") == 0) {
        const char *root = NULL, *out = NULL, *device = NULL;
        int want_stat = 1, verbose = 0;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
            else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
            else if ((!strcmp(argv[i], "--device") || !strcmp(argv[i], "--raw")) && i + 1 < argc)
                device = argv[++i];
            else if (!strcmp(argv[i], "--exclude") && i + 1 < argc) scan_add_exclude(argv[++i]);
            else if (!strcmp(argv[i], "--include-hidden")) scan_set_skip_hidden(0);
            else if (!strcmp(argv[i], "--stat")) want_stat = 1;
            else if (!strcmp(argv[i], "--no-stat")) want_stat = 0;
            else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) verbose = 1;
            else { fprintf(stderr, "xuanlan: unknown option %s\n", argv[i]); return 2; }
        }
        if ((!root && !device) || !out) { usage(); return 2; }

        xl_store st;
        store_init(&st);
        uint64_t t0 = now_ms();
        int rc;
        if (device) {
            char *mp = mount_for(device);
            const char *use_root = root ? root : (mp ? mp : "/");
            rc = fsreader_ext4_scan(device, use_root, &st, verbose);
            if (mp && !root) {
                /* 记录用挂载点作为根：替换第 0 条记录的路径前缀 */
                fprintf(stderr, "device %s mounted at %s\n", device, mp);
            }
            free(mp);
            root = use_root;
        } else {
            rc = scan_tree(root, &st, want_stat, verbose);
        }
        uint64_t scan_ms = now_ms() - t0;
        if (rc < 0 && st.n <= 1) {
            fprintf(stderr, "xuanlan: nothing scanned\n");
            store_free(&st);
            return 1;
        }
        store_shrink(&st);

        fprintf(stderr, "scan: %u records, %.1f MB names, %llu ms\n",
                st.n, (double)st.names_len / 1048576.0, (unsigned long long)scan_ms);

        xl_index ix;
        index_init(&ix);
        index_build(&ix, &st, 1);

        uint64_t t1 = now_ms();
        if (snapshot_save(out, &st, &ix, root, scan_ms) < 0) {
            store_free(&st);
            index_free(&ix);
            return 1;
        }
        uint64_t write_ms = now_ms() - t1;

        struct stat sb;
        stat(out, &sb);
        fprintf(stderr, "index: %.1f MB file, %.1f MB postings, build %llu ms, write %llu ms\n",
                (double)sb.st_size / 1048576.0, (double)ix.blob_len / 1048576.0,
                (unsigned long long)ix.build_ms, (unsigned long long)write_ms);
        store_free(&st);
        index_free(&ix);
        return 0;
    }

    if (strcmp(cmd, "serve") == 0) {
        xl_svc_opts o;
        memset(&o, 0, sizeof(o));
        const char *appdir = getenv("TRIM_APPDEST");
        const char *vardir = getenv("TRIM_PKGVAR");
        const char *sport  = getenv("TRIM_SERVICE_PORT");
        o.port = sport ? atoi(sport) : 0;
        if (o.port <= 0) o.port = 8971;
        snprintf(o.bind_addr, sizeof(o.bind_addr), "127.0.0.1");
        {
            char roots[1024];
            detect_default_roots(roots, sizeof roots);
            snprintf(o.roots, sizeof(o.roots), "%s", roots);
        }
        o.rebuild_hours = 6;
        if (appdir) {
            snprintf(o.sock_path, sizeof(o.sock_path), "%s/app.sock", appdir);
            snprintf(o.ui_dir, sizeof(o.ui_dir), "%s/ui", appdir);
        } else {
            snprintf(o.ui_dir, sizeof(o.ui_dir), "ui");
        }
        if (vardir) snprintf(o.idx_dir, sizeof(o.idx_dir), "%s/idx", vardir);
        else snprintf(o.idx_dir, sizeof(o.idx_dir), "./idx");
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--port") && i + 1 < argc) o.port = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--bind") && i + 1 < argc)
                snprintf(o.bind_addr, sizeof(o.bind_addr), "%s", argv[++i]);
            else if (!strcmp(argv[i], "--socket") && i + 1 < argc)
                snprintf(o.sock_path, sizeof(o.sock_path), "%s", argv[++i]);
            else if (!strcmp(argv[i], "--ui") && i + 1 < argc)
                snprintf(o.ui_dir, sizeof(o.ui_dir), "%s", argv[++i]);
            else if (!strcmp(argv[i], "--index-dir") && i + 1 < argc)
                snprintf(o.idx_dir, sizeof(o.idx_dir), "%s", argv[++i]);
            else if (!strcmp(argv[i], "--roots") && i + 1 < argc)
                snprintf(o.roots, sizeof(o.roots), "%s", argv[++i]);
            else if (!strcmp(argv[i], "--rebuild-hours") && i + 1 < argc) o.rebuild_hours = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--exclude") && i + 1 < argc) scan_add_exclude(argv[++i]);
            else if (!strcmp(argv[i], "--no-socket")) o.sock_path[0] = 0;
            else if (!strcmp(argv[i], "--no-tcp")) o.no_tcp = 1;
            else { fprintf(stderr, "xuanlan: unknown option %s\n", argv[i]); return 2; }
        }
        return httpd_serve(&o);
    }

    if (strcmp(cmd, "acl") == 0) {
        const char *se = getenv("XL_STRICT_HOME");
        perm_set_strict_home(se ? atoi(se) : 1);
        if (argc < 4) { fprintf(stderr, "用法: xuanlan acl UID DIR...\n"); return 2; }
        uid_t uid = (uid_t)atoi(argv[2]);
        for (int i = 3; i < argc; i++)
            printf("%d %s\n", perm_dir_listable(uid, argv[i]), argv[i]);
        return 0;
    }

    if (strcmp(cmd, "query") == 0) {
        const char *path = NULL, *term = NULL;
        xl_query_opts o;
        memset(&o, 0, sizeof(o));
        o.max_results = 50;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--index") && i + 1 < argc) path = argv[++i];
            else if (!strcmp(argv[i], "-n") && i + 1 < argc) o.max_results = (uint32_t)atoi(argv[++i]);
            else if (!strcmp(argv[i], "--json")) o.json = 1;
            else if (!strcmp(argv[i], "--dirs")) o.dirs_only = 1;
            else if (!strcmp(argv[i], "--files")) o.files_only = 1;
            else if (!term) term = argv[i];
            else { fprintf(stderr, "xuanlan: unexpected %s\n", argv[i]); return 2; }
        }
        if (!path || !term) { usage(); return 2; }
        xl_snap sn;
        if (snapshot_open(path, &sn) < 0) return 1;
        int rc = query_run(&sn, term, &o);
        snapshot_close(&sn);
        return rc;
    }

    if (strcmp(cmd, "info") == 0) {
        const char *path = NULL;
        for (int i = 2; i < argc; i++)
            if (!strcmp(argv[i], "--index") && i + 1 < argc) path = argv[++i];
        if (!path) { usage(); return 2; }
        xl_snap sn;
        if (snapshot_open(path, &sn) < 0) return 1;
        printf("root      %s\n", sn.root);
        printf("records   %llu\n", (unsigned long long)sn.h.rec_count);
        printf("names     %.1f MB\n", (double)sn.h.names_bytes / 1048576.0);
        printf("trigrams  %u\n", sn.h.tri_count);
        printf("postings  %.1f MB\n", (double)sn.h.postings_bytes / 1048576.0);
        printf("scan      %llu ms\n", (unsigned long long)sn.h.scan_ms);
        printf("built     %llu\n", (unsigned long long)sn.h.built_at);
        snapshot_close(&sn);
        return 0;
    }

    if (strcmp(cmd, "dump") == 0) {
        const char *path = NULL;
        for (int i = 2; i < argc; i++)
            if (!strcmp(argv[i], "--index") && i + 1 < argc) path = argv[++i];
        if (!path) { usage(); return 2; }
        xl_snap sn;
        if (snapshot_open(path, &sn) < 0) return 1;
        char *p = xmalloc(8192);
        for (uint32_t i = 0; i < (uint32_t)sn.h.rec_count; i++) {
            snap_path(&sn, i, p, 8192);
            fputs(p, stdout);
            fputc('\n', stdout);
        }
        free(p);
        snapshot_close(&sn);
        return 0;
    }

    usage();
    return 2;
}
