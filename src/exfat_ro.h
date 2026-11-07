/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
/*
 * exfat_ro - small read-only exFAT driver.
 *
 * Freestanding: no libc, no malloc, no globals. Endian- and alignment-neutral (all
 * on-disk fields are read byte by byte), so it is safe on little-endian SuperH.
 * The caller supplies a sector-read callback and two sector-sized scratch buffers.
 *
 * API mirrors the player's VFS (open / read / seek / tell / eof / length / close),
 * plus directory enumeration.
 *
 * Limitations (deliberate, for a first version):
 *   - read only; TexFAT (2 FATs) uses the first FAT
 *   - names compare case-insensitively for ASCII only (non-ASCII must match exactly)
 *   - volumes up to 2^32 sectors
 */
#ifndef EXFAT_RO_H
#define EXFAT_RO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXFAT_OK            0
#define EXFAT_ERR_IO       (-1)
#define EXFAT_ERR_FORMAT   (-2)
#define EXFAT_ERR_NOTFOUND (-3)
#define EXFAT_ERR_NOTDIR   (-4)
#define EXFAT_ERR_ISDIR    (-5)
#define EXFAT_ERR_CORRUPT  (-6)
#define EXFAT_ERR_NAME     (-7)

#define EXFAT_ATTR_DIR     0x10
#define EXFAT_MAX_NAME16   255

/* Read `nsect` sectors starting at volume-relative LBA `lba` into buf. Return 0 on success. */
typedef int (*exfat_read_fn)(void *ctx, uint32_t lba, uint32_t nsect, void *buf);

typedef struct {
    exfat_read_fn read;
    void *ctx;
    uint32_t base_lba;        /* added to every LBA passed to read(): partition start */
    uint32_t sector_size, sector_shift;
    uint32_t cluster_shift;   /* log2(sectors per cluster) */
    uint32_t cluster_size;    /* bytes */
    uint32_t fat_lba, heap_lba, cluster_count, root_cluster;
    uint32_t volume_sectors;  /* VolumeLength from the boot sector, clamped to 32 bits */
    uint8_t *sbuf;            /* scratch, >= sector_size (caller owned) */
    uint8_t *fbuf;            /* FAT sector cache, >= sector_size (caller owned) */
    uint32_t fbuf_lba;        /* LBA currently held in fbuf, 0xFFFFFFFF = none */
    uint32_t sbuf_lba;        /* LBA currently held in sbuf, 0xFFFFFFFF = none */
} exfat_vol;

typedef struct {
    uint32_t first_cluster;
    uint64_t size;            /* DataLength */
    uint64_t valid_size;      /* ValidDataLength; bytes beyond read as zero */
    uint8_t  attr;            /* EXFAT_ATTR_* (low byte of file attributes) */
    uint8_t  no_fat_chain;    /* clusters are contiguous, FAT not used */
} exfat_node;

typedef struct {
    exfat_node node;
    uint16_t   name16[EXFAT_MAX_NAME16 + 1]; /* UTF-16, NUL terminated */
    uint8_t    name_len;                     /* in UTF-16 units */
    uint32_t   ctime, mtime, atime;          /* raw 32-bit DOS date/time (date<<16 | time) */
} exfat_dirent;

typedef struct {
    exfat_vol *vol;
    exfat_node dir;
    uint32_t cluster;         /* current cluster of the directory stream */
    uint32_t off;             /* byte offset inside that cluster */
    uint64_t pos;             /* bytes consumed from the directory stream */
    uint32_t hops;            /* cluster-chain guard */
    int done;
} exfat_dir;

typedef struct {
    exfat_vol *vol;
    exfat_node node;
    uint64_t pos;
    uint32_t cur_idx;         /* cluster index (0-based within the file) cached below */
    uint32_t cur_cluster;     /* 0 = nothing cached */
} exfat_file;

/* Does this sector 0 of a volume look like exFAT? (cheap check for partition probing) */
int exfat_probe(const uint8_t *sector0, uint32_t len);

/* Mount: reads and validates the boot sector at LBA 0 (volume relative). */
int exfat_mount(exfat_vol *v, exfat_read_fn rd, void *ctx, uint32_t base_lba,
                uint8_t *sbuf, uint8_t *fbuf, uint32_t buf_size);

int exfat_root(exfat_vol *v, exfat_node *out);
int exfat_lookup(exfat_vol *v, const char *utf8_path, exfat_node *out);

int exfat_opendir(exfat_vol *v, const exfat_node *dir, exfat_dir *d);
/* 1 = entry returned, 0 = end of directory, <0 = error */
int exfat_readdir(exfat_dir *d, exfat_dirent *e);

/* file API (VFS-shaped) */
int      exfat_fopen(exfat_vol *v, const char *utf8_path, exfat_file *f);
int      exfat_fopen_node(exfat_vol *v, const exfat_node *n, exfat_file *f);
int32_t  exfat_fread(exfat_file *f, void *buf, uint32_t len);   /* bytes read, or <0 */
int      exfat_fseek(exfat_file *f, int64_t off, int whence);   /* 0 SET, 1 CUR, 2 END */
uint64_t exfat_ftell(const exfat_file *f);
int      exfat_feof(const exfat_file *f);
uint64_t exfat_filelen(const exfat_file *f);

/* low-level access (used by the exFAT->FAT32 facade) */
int exfat_fat_entry(exfat_vol *v, uint32_t cluster, uint32_t *raw);            /* raw FAT value */
int exfat_read_sectors(exfat_vol *v, uint32_t lba, uint32_t nsect, void *buf); /* volume-relative */
int exfat_count_clusters(exfat_vol *v, uint32_t first, uint32_t *count);       /* walk a FAT chain */

/* helpers */
int exfat_utf8_to_utf16(const char *s, uint16_t *out, int max_units);   /* units written, <0 on error */
int exfat_utf16_to_utf8(const uint16_t *s, int units, char *out, int max_bytes); /* bytes, excl. NUL */

#ifdef __cplusplus
}
#endif
#endif
