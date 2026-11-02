#include "xl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "xuanlan: out of memory (%zu)\n", n); exit(2); }
    return p;
}

void *xcalloc(size_t n, size_t m) {
    void *p = calloc(n ? n : 1, m ? m : 1);
    if (!p) { fprintf(stderr, "xuanlan: out of memory (%zu x %zu)\n", n, m); exit(2); }
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "xuanlan: out of memory (%zu)\n", n); exit(2); }
    return q;
}

/* ASCII 折小写；UTF-8 多字节保持原样（>0x7F 不动） */
void lower_copy(char *dst, const char *src, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
}

static inline unsigned char fold(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

/* 大小写不敏感的子串查找（ASCII 折叠），返回 1 表示命中 */
int mem_ci(const char *hay, size_t hlen, const char *ndl, size_t nlen) {
    if (nlen == 0 || nlen > hlen) return nlen == 0;
    unsigned char first = fold((unsigned char)ndl[0]);
    size_t last = hlen - nlen;
    for (size_t i = 0; i <= last; i++) {
        if (fold((unsigned char)hay[i]) != first) continue;
        size_t j = 1;
        for (; j < nlen; j++)
            if (fold((unsigned char)hay[i + j]) != fold((unsigned char)ndl[j])) break;
        if (j == nlen) return 1;
    }
    return 0;
}

/* 同上，返回首个命中位置；未命中返回 NULL */
const char *memmem_ci(const char *hay, size_t hlen, const char *ndl, size_t nlen) {
    if (nlen == 0) return hay;
    if (nlen > hlen) return NULL;
    unsigned char first = fold((unsigned char)ndl[0]);
    size_t last = hlen - nlen;
    for (size_t i = 0; i <= last; i++) {
        if (fold((unsigned char)hay[i]) != first) continue;
        size_t j = 1;
        for (; j < nlen; j++)
            if (fold((unsigned char)hay[i + j]) != fold((unsigned char)ndl[j])) break;
        if (j == nlen) return hay + i;
    }
    return NULL;
}

uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
