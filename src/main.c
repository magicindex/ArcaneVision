/* 玄览 / Arcane Vision — CLI
 *   xuanlan build --root DIR --out FILE [--exclude DIR] [--stat] [-v]
 *   xuanlan query --index FILE [-n N] [--json] [--dirs|--files] TERM
 *   xuanlan info  --index FILE
 */
#include "xl.h"

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
          "                [--roots /vol1,/vol3,...] [--exclude DIR]...\n", stderr);
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
    if (!n) snprintf(out, cap, "/vol1,/vol2,/vol3,/vol4");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];

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
