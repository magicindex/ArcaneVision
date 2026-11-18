/* scope.json 解析与单元解析（P1）。 */
#include "scanmode.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static int json_str_field(const char *buf, const char *key, char *out, size_t cap,
                          const char *def) {
    snprintf(out, cap, "%s", def);
    if (!buf) return 0;
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    p = strchr(p, '"');
    if (!p) return -1;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return -1;
    size_t n = (size_t)(e - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return 0;
}

static int json_int_field(const char *buf, const char *key, int def) {
    if (!buf) return def;
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return def;
    p = strchr(p + strlen(pat), ':');
    if (!p) return def;
    p = skip_ws(p + 1);
    char *e = NULL;
    long v = strtol(p, &e, 10);
    if (e == p) return def;
    return (int)v;
}

/* 解析数字数组；允许 ["9001", 9002] 这类混写。 */
static int json_array_u32(const char *buf, const char *key, unsigned int *out, int max) {
    if (!buf) return 0;
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;
    int n = 0;
    while (*p && *p != ']' && n < max) {
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '"') {
            p++;
            p = skip_ws(p);
        }
        if (!isdigit((unsigned char)*p)) {
            if (*p == ']' || *p == 0) break;
            p++;
            continue;
        }
        char *e = NULL;
        unsigned long v = strtoul(p, &e, 10);
        if (e == p) { p++; continue; }
        out[n++] = (unsigned int)v;
        p = e;
    }
    return n;
}

static int gid_in(const gid_t *a, int n, gid_t g) {
    for (int i = 0; i < n; i++) if (a[i] == g) return 1;
    return 0;
}

static int uid_in(const uid_t *a, int n, uid_t u) {
    for (int i = 0; i < n; i++) if (a[i] == u) return 1;
    return 0;
}

int xl_scan_policy_load(const char *path, xl_scan_policy *p) {
    memset(p, 0, sizeof(*p));
    p->version = 1;
    p->mode = XL_SCAN_SHARED;
    if (!path || !*path) return 0;

    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;

    p->version = json_int_field(buf, "version", 1);
    char mode[16];
    json_str_field(buf, "scanMode", mode, sizeof mode, "shared");
    if (!strcmp(mode, "isolated")) p->mode = XL_SCAN_ISOLATED;
    else if (!strcmp(mode, "hybrid")) p->mode = XL_SCAN_HYBRID;
    else p->mode = XL_SCAN_SHARED;

    unsigned int tmp[XL_SCAN_ARRAY_MAX];
    int k;
    k = json_array_u32(buf, "shareGroups", tmp, XL_SCAN_ARRAY_MAX);
    for (int i = 0; i < k; i++) if (tmp[i]) p->share_groups[p->n_share_groups++] = (gid_t)tmp[i];
    k = json_array_u32(buf, "isolatedGroups", tmp, XL_SCAN_ARRAY_MAX);
    for (int i = 0; i < k; i++) if (tmp[i]) p->isolated_groups[p->n_isolated_groups++] = (gid_t)tmp[i];
    k = json_array_u32(buf, "shareUsers", tmp, XL_SCAN_ARRAY_MAX);
    for (int i = 0; i < k; i++) if (tmp[i]) p->share_users[p->n_share_users++] = (uid_t)tmp[i];
    k = json_array_u32(buf, "isolatedUsers", tmp, XL_SCAN_ARRAY_MAX);
    for (int i = 0; i < k; i++) if (tmp[i]) p->isolated_users[p->n_isolated_users++] = (uid_t)tmp[i];
    return 0;
}

const char *xl_scan_mode_name(xl_scan_mode m) {
    return m == XL_SCAN_ISOLATED ? "isolated" : (m == XL_SCAN_HYBRID ? "hybrid" : "shared");
}

static void unit_shared(xl_scan_unit *u, int fallback) {
    memset(u, 0, sizeof(*u));
    snprintf(u->key, sizeof u->key, "shared");
    u->kind = XL_UNIT_SHARED;
    u->fallback = fallback;
}

static void unit_user(xl_scan_unit *u, uid_t uid) {
    memset(u, 0, sizeof(*u));
    snprintf(u->key, sizeof u->key, "u%u", (unsigned)uid);
    u->kind = XL_UNIT_USER;
    u->uid = uid;
}

static void unit_group(xl_scan_unit *u, gid_t gid) {
    memset(u, 0, sizeof(*u));
    snprintf(u->key, sizeof u->key, "g%u", (unsigned)gid);
    u->kind = XL_UNIT_GROUP;
    u->gid = gid;
}

int xl_scan_resolve(const xl_scan_policy *p, uid_t uid, const gid_t *gids,
                    int ngids, xl_scan_unit *out, int cap) {
    if (cap <= 0) return 0;
    if (!p || p->mode == XL_SCAN_SHARED) {
        unit_shared(&out[0], 0);
        return 1;
    }
    if (p->mode == XL_SCAN_ISOLATED) {
        unit_user(&out[0], uid);
        return 1;
    }
    /* hybrid */
    if (uid_in(p->isolated_users, p->n_isolated_users, uid)) {
        unit_user(&out[0], uid);
        return 1;
    }
    if (uid_in(p->share_users, p->n_share_users, uid)) {
        unit_shared(&out[0], 0);
        return 1;
    }
    gid_t iso[XL_SCAN_ARRAY_MAX];
    int niso = 0;
    for (int i = 0; i < ngids; i++) {
        if (!gid_in(p->isolated_groups, p->n_isolated_groups, gids[i])) continue;
        if (!gid_in(iso, niso, gids[i])) iso[niso++] = gids[i];
    }
    for (int i = 1; i < niso; i++) {          /* gid 升序，输出稳定 */
        gid_t v = iso[i];
        int j = i - 1;
        while (j >= 0 && iso[j] > v) { iso[j + 1] = iso[j]; j--; }
        iso[j + 1] = v;
    }
    int n = 0;
    for (int i = 0; i < niso && n < cap; i++) unit_group(&out[n++], iso[i]);
    if (n) return n;
    for (int i = 0; i < ngids; i++)
        if (gid_in(p->share_groups, p->n_share_groups, gids[i])) {
            unit_shared(&out[0], 0);
            return 1;
        }
    unit_shared(&out[0], 1);
    return 1;
}
