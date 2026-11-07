/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
/* Run the exact firmware blob (linked at 0x08119E80) under qemu-sh4 with fake firmware functions at the
 * real addresses it calls: alloc 0x09265F78, free 0x09265FBE, original I/O 0x09281694. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

static int fd; static unsigned part;
static void *t_alloc(unsigned n) { return malloc(n); }
static void t_free(void *p) { free(p); }
static int t_io(void *disk, int cmd, unsigned lba, void *buf, unsigned count) {
    (void)part;
    if (cmd == 1) { size_t w = (size_t)count * 512; return pread(fd, buf, w, (off_t)(unsigned)(*(unsigned *)((char *)disk + 44) + lba) * 512) == (ssize_t)w ? 0 : -1; }
    if (cmd == 2 || cmd == 5 || cmd == 7 || cmd == 8 || cmd == 10) return 0;
    return -16;
}
static void stub(uint32_t at, void *target) {
    static const uint8_t code[6] = { 0x01, 0xD0, 0x2B, 0x40, 0x09, 0x00 };   /* mov.l @(1,PC),r0 ; jmp @r0 ; nop */
    uint32_t lit = ((at + 4) & ~3u) + 4, t = (uint32_t)(uintptr_t)target;
    memcpy((void *)(uintptr_t)at, code, 6);
    memcpy((void *)(uintptr_t)lit, &t, 4);
}
static void *map(uint32_t at, size_t len) {
    void *p = mmap((void *)(uintptr_t)at, len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(3); }
    return p;
}
static uint64_t fnv(const uint8_t *p, size_t n) { uint64_t h = 1469598103934665603ull; while (n--) { h ^= *p++; h *= 1099511628211ull; } return h; }

static uint8_t g_seen[512];
static int t_parse(void *d, void *b) { (void)d; memcpy(g_seen, b, 512); return 0; }
int main(int argc, char **argv) {
    typedef int (*io_fn)(void *, int, unsigned, void *, unsigned);
    static uint8_t buf[65536];
    FILE *bf; unsigned entry, F, Sd, spc; io_fn io; void *disk = (void *)0x0B0F9224u; int ai; char *ver = (char *)0x08000740u;
    if (argc < 5) return 2;
    fd = open(argv[1], O_RDONLY); part = (unsigned)strtoul(argv[2], 0, 0);
    entry = (unsigned)strtoul(argv[3], 0, 0);
    map(0x08119000, 0x7000); map(0x09265000, 0x1000); map(0x09281000, 0x1000); map(0x0B0F9000, 0x2000); map(0x08000000, 0x1000);
    memcpy(ver, "1.47.", 5);
    if (getenv("XDJ_FILL")) memset((void *)0x08119000, 0xFF, 0x7000);   /* the real padding area is 0xFF, not zero */
    for (ai = 4; ai < argc; ai++) {                  /* fragments given as ADDR:FILE */
        char *colon = strchr(argv[ai], ':'); unsigned long addr; long n;
        *colon = 0; addr = strtoul(argv[ai], 0, 0);
        bf = fopen(colon + 1, "rb"); fseek(bf, 0, SEEK_END); n = ftell(bf); fseek(bf, 0, SEEK_SET); n = (long)fread((void *)(uintptr_t)addr, 1, (size_t)n, bf); fclose(bf); printf("loaded %ld bytes from %s at 0x%lx\n", n, colon + 1, addr);
    }
    *(unsigned *)((char *)disk + 44) = getenv("XDJ_D44") ? (unsigned)strtoul(getenv("XDJ_D44"), 0, 0) : part;   /* the record's partition start, as the firmware sees it */
    stub(0x09265F78, (void *)t_alloc); stub(0x09265FBE, (void *)t_free); stub(0x09281694, (void *)t_io);
    if (getenv("XDJ_VIEW")) disk = (void *)(uintptr_t)strtoul(getenv("XDJ_VIEW"), 0, 0);   /* e.g. 0xAB0F9224: the P2 view of the same record */
    io = (io_fn)(uintptr_t)entry;
    { unsigned char *p = (unsigned char *)(uintptr_t)entry; printf("entry bytes: %02x %02x %02x %02x %02x %02x\n", p[0], p[1], p[2], p[3], p[4], p[5]); fflush(stdout); }
    printf("attach cmd5 -> %d  status %.4s\n", io(disk, 5, 0, 0, 1), ver);
    if (getenv("XDJ_IDENT")) {                       /* identify hook: runs BEFORE any read of ours, handed the REAL absolute sector 0 */
        typedef int (*id_fn)(void *, void *); id_fn id = (id_fn)(uintptr_t)strtoul(getenv("XDJ_IDENT"), 0, 0); static uint8_t rb[4096]; int r;
        map(0x093A5000, 0x1000); stub(0x093A52D2, (void *)t_parse);
        if (pread(fd, rb, 512, 0) != 512) return 3;
        r = id(disk, rb);
        printf("ident hook -> %d  classifier saw fnv %016llx  status %.4s\n", r, (unsigned long long)fnv(g_seen, 512), ver);
    }
    printf("read lba0 -> %d  status %.4s\n", io(disk, 1, 0, buf, 1), ver);
    printf("boot sector fnv %016llx fs-type '%.8s'\n", (unsigned long long)fnv(buf, 512), (char *)buf + 82);
    F = buf[36] | (buf[37] << 8) | (buf[38] << 16) | ((unsigned)buf[39] << 24); spc = buf[13]; Sd = 32 + 2 * F;
    io(disk, 1, 1, buf, 1);                          printf("lba1 fnv %016llx\n", (unsigned long long)fnv(buf, 512));
    io(disk, 1, 32, buf, 2);                         printf("fat sectors 0-1 fnv %016llx\n", (unsigned long long)fnv(buf, 1024));
    io(disk, 1, Sd, buf, spc);                       printf("root dir cluster fnv %016llx  status %.4s\n", (unsigned long long)fnv(buf, spc * 512), ver);
    printf("write refused -> %d\n", io(disk, 2, 100, buf, 1));
    printf("total sectors per bpb %u spc %u F %u Sd %u\n", buf[0] ? 0 : 0, spc, F, Sd);
    return 0;
}
