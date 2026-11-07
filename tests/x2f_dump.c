/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
/* Dump the virtual FAT32 volume of an exFAT image to a file (for fsck.fat / mtools checks). */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "x2f.h"

static int img_read(void *ctx, uint32_t lba, uint32_t n, void *buf) {
    int fd = *(int *)ctx; size_t want = (size_t)n * 512;
    return pread(fd, buf, want, (off_t)lba * 512) == (ssize_t)want ? 0 : -1;
}
static void *m_alloc(void *c, uint32_t n) { (void)c; return malloc(n); }
static void m_free(void *c, void *p) { (void)c; free(p); }

int main(int argc, char **argv) {
    static exfat_vol vol; static uint8_t sb[4096], fb[4096];
    x2f_mem mem = { m_alloc, m_free, 0 };
    x2f_t *x; int fd, r; FILE *out; uint32_t lba, total;
    uint8_t *chunk;
    if (argc < 3) return 2;
    fd = open(argv[1], O_RDONLY); if (fd < 0) { perror("open"); return 2; }
    if ((r = exfat_mount(&vol, img_read, &fd, 0, sb, fb, sizeof sb))) { fprintf(stderr, "mount %d\n", r); return 1; }
    if ((r = x2f_open(&x, &vol, &mem, 64u << 20))) { fprintf(stderr, "x2f_open %d\n", r); return 1; }
    if ((r = x2f_prefetch_all(x))) { fprintf(stderr, "prefetch %d\n", r); return 1; }
    total = x2f_total_sectors(x);
    out = fopen(argv[2], "wb"); chunk = malloc(128 * 512);
    for (lba = 0; lba < total; lba += 128) {
        uint32_t n = total - lba < 128 ? total - lba : 128;
        if ((r = x2f_read(x, lba, n, chunk))) { fprintf(stderr, "read lba %u: %d\n", lba, r); return 1; }
        fwrite(chunk, 512, n, out);
    }
    fclose(out);
    fprintf(stderr, "virtual FAT32 volume: %u sectors (%u MB)\n", total, total / 2048);
    x2f_close(x);
    return 0;
}
