/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
#include <string.h>
/* Test shim: fakes the firmware services around xdj_exfat.c so it can run on a PC (or under qemu-sh4). */
#define _FILE_OFFSET_BITS 64
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

unsigned long xdj_test_disk_base;
static char g_ver[16] = "1.47.   20240228";
char *xdj_test_ver = g_ver;
const char *xdj_test_get_ver(void) { return g_ver; }
void xdj_test_set_ver(const char *s) { int i; for (i = 0; i < 5 && s[i]; i++) g_ver[i] = s[i]; }
static unsigned char disks[4][0x1E8];
static int g_fd = -1;
static unsigned g_part;
static unsigned long g_calls[16];

void *fw_alloc(unsigned n) { return malloc(n); }
void fw_free(void *p) { free(p); }

/* the original callback adds the disk record's partition start (disk[44]) to every read LBA */
static unsigned d44_of(void *disk) {
    unsigned i;
    for (i = 0; i < 4; i++) if ((void *)disks[i] == disk) return *(unsigned *)(disks[i] + 44);
    return g_part;
}
void xdj_test_set_d44(unsigned v) { unsigned i; for (i = 0; i < 4; i++) *(unsigned *)(disks[i] + 44) = v; }

int fw_orig_io(void *disk, int cmd, unsigned lba, void *buf, unsigned count) {
    if (cmd >= 0 && cmd < 16) g_calls[cmd]++;
    switch (cmd) {
    case 1: {
        size_t want = (size_t)count * 512;
        return pread(g_fd, buf, want, (off_t)(unsigned)(d44_of(disk) + lba) * 512) == (ssize_t)want ? 0 : -1;
    }
    case 2: case 5: case 7: case 8: case 10: return 0;
    default: return -16;
    }
}

int xdj_io(void *disk, int cmd, unsigned lba, void *buf, unsigned count);

int xdj_test_open(const char *path, unsigned part_start) {
    g_fd = open(path, O_RDONLY);
    g_part = part_start;
    xdj_test_set_d44(part_start);
    xdj_test_disk_base = (unsigned long)disks;
    return g_fd < 0 ? -1 : 0;
}
int xdj_test_cmd(int idx, int cmd, unsigned lba, void *buf, unsigned count) {
    return xdj_io(idx >= 0 && idx < 4 ? (void *)disks[idx] : (void *)&g_fd, cmd, lba, buf, count);
}
unsigned long xdj_test_orig_calls(int cmd) { return g_calls[cmd & 15]; }

static unsigned char g_parse_seen[512];
int xdj_test_parse(void *disk, void *buf) { (void)disk; memcpy(g_parse_seen, buf, 512); return 0; }
int xdj_test_ident(unsigned char *sector512) {   /* runs the hook on the given "real" sector 0; returns 0 and leaves what the parser saw in g_parse_seen */
    unsigned char buf[4096]; extern int xdj_ident(void *, void *);
    memcpy(buf, sector512, 512);
    return xdj_ident(disks[0], buf);
}
const unsigned char *xdj_test_parse_seen(void) { return g_parse_seen; }
