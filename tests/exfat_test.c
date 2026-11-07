/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
/* Host/qemu test harness for exfat_ro: exfat_test IMG ls | cat PATH [OFF LEN] | probe */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "exfat_ro.h"

static int img_read(void *ctx, uint32_t lba, uint32_t n, void *buf) {
    int fd = *(int *)ctx;
    size_t want = (size_t)n * 512;
    ssize_t got = pread(fd, buf, want, (off_t)lba * 512);
    return got == (ssize_t)want ? 0 : -1;
}

static exfat_vol vol;
static uint8_t sbuf[4096], fbuf[4096];

static int walk(const char *path, const exfat_node *dir) {
    exfat_dir d; exfat_dirent e; int r;
    if ((r = exfat_opendir(&vol, dir, &d))) return r;
    while ((r = exfat_readdir(&d, &e)) == 1) {
        char name[800], full[4096];
        if (exfat_utf16_to_utf8(e.name16, e.name_len, name, sizeof name) < 0) return EXFAT_ERR_NAME;
        snprintf(full, sizeof full, "%s/%s", path, name);
        if (e.node.attr & EXFAT_ATTR_DIR) {
            printf("D\t0\t%s\n", full);
            if ((r = walk(full, &e.node))) return r;
        } else {
            printf("F\t%llu\t%s\n", (unsigned long long)e.node.size, full);
        }
    }
    return r;
}

int main(int argc, char **argv) {
    int fd, r;
    if (argc < 3) { fprintf(stderr, "usage\n"); return 2; }
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 2; }
    r = exfat_mount(&vol, img_read, &fd, 0, sbuf, fbuf, sizeof sbuf);
    if (r) { fprintf(stderr, "mount failed: %d\n", r); return 1; }
    if (!strcmp(argv[2], "probe")) { printf("exfat ok, cluster_size=%u\n", vol.cluster_size); return 0; }
    if (!strcmp(argv[2], "ls")) {
        exfat_node root; exfat_root(&vol, &root);
        r = walk("", &root);
        if (r) { fprintf(stderr, "walk failed: %d\n", r); return 1; }
        return 0;
    }
    if (!strcmp(argv[2], "cat") && argc >= 4) {
        exfat_file f; uint8_t *buf; uint64_t off = 0, len = 0; int32_t got;
        r = exfat_fopen(&vol, argv[3], &f);
        if (r) { fprintf(stderr, "open failed: %d\n", r); return 1; }
        if (argc >= 6) { off = strtoull(argv[4], 0, 0); len = strtoull(argv[5], 0, 0);
                         if (exfat_fseek(&f, (int64_t)off, 0)) { fprintf(stderr, "seek failed\n"); return 1; } }
        else len = exfat_filelen(&f);
        buf = malloc(4093);
        while (len) {
            uint32_t chunk = len > 4093 ? 4093 : (uint32_t)len;      /* odd size on purpose */
            got = exfat_fread(&f, buf, chunk);
            if (got < 0) { fprintf(stderr, "read failed: %d\n", got); return 1; }
            if (got == 0) break;
            fwrite(buf, 1, (size_t)got, stdout);
            len -= (uint32_t)got;
        }
        return 0;
    }
    fprintf(stderr, "bad command\n");
    return 2;
}
