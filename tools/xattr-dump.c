#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/xattr.h>

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        unsigned char buf[4096];
        ssize_t n = getxattr(argv[i], "system.trim_acl", buf, sizeof buf);
        printf("== %s: %zd\n", argv[i], n);
        if (n < 0) { perror("  getxattr"); continue; }
        for (ssize_t k = 0; k < n; k++) {
            printf("%02x%s", buf[k], (k % 16 == 15) ? "\n" : " ");
        }
        printf("\n");
    }
    return 0;
}
