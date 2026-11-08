/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
/*
 * x2f - present a mounted exFAT volume to a FAT32 driver as a virtual, read-only FAT32 volume.
 *
 * The FAT32 driver reads sectors through x2f_read(); everything is synthesized on demand:
 *   - boot sector / FSInfo (sectors 0,1,6,7)
 *   - the FAT (two identical copies), built from the exFAT FAT / contiguous-file flags
 *   - directories, converted from exFAT entry sets to FAT long-name + short-name entries
 * File data sectors pass straight through to the device (1:1 cluster numbering).
 *
 * Cluster numbers are virtual and are assigned when a directory is first listed, in blocks that
 * start on a FAT-sector boundary, so a FAT sector is never served before its content is final
 * (the FAT driver may cache it indefinitely). A directory is generated the first time one of its
 * sectors is requested. Call x2f_prefetch_all() to generate everything eagerly (tests).
 *
 * Limits: 512-byte sectors, files < 4 GiB (larger files are hidden), ValidDataLength is not represented, no writes.
 */
#ifndef X2F_H
#define X2F_H

#include "exfat_ro.h"

#ifdef __cplusplus
extern "C" {
#endif

#define X2F_OK            0
#define X2F_ERR_IO       (-1)
#define X2F_ERR_UNSUPPORTED (-2)
#define X2F_ERR_NOMEM    (-3)
#define X2F_ERR_RANGE    (-4)
#define X2F_ERR_CORRUPT  (-5)
#define X2F_ERR_FULL     (-6)   /* out of virtual directory clusters */

typedef struct {
    void *(*alloc)(void *ctx, uint32_t bytes);
    void  (*release)(void *ctx, void *p);
    void  *ctx;
} x2f_mem;

typedef struct x2f x2f_t;

/* v must be a mounted exFAT volume that outlives the facade. The virtual volume never exceeds the
 * real volume's size and uses clusters of at most 32 KB (real clusters of any size are supported).
 * cache_bytes: memory budget for generated directory contents. */
int  x2f_open(x2f_t **out, exfat_vol *v, const x2f_mem *mem, uint32_t cache_bytes);
void x2f_close(x2f_t *x);

/* partition-relative sector read of the virtual FAT32 volume (same shape as the I/O callback) */
int  x2f_read(x2f_t *x, uint32_t lba, uint32_t nsect, void *buf);
uint32_t x2f_total(const x2f_t *x);                                                    /* size of the virtual volume in sectors */
uint32_t x2f_data_map(x2f_t *x, uint32_t lba, uint32_t n, uint32_t *real_out);   /* writes: where does this file-data sector really live (0 = not file data) */
uint32_t x2f_total_sectors(const x2f_t *x);

/* diagnostics: directories generated, file-data reads, FAT sectors served so far */
void x2f_stats(const x2f_t *x, uint32_t *dirs, uint32_t *data, uint32_t *fat);

/* generate every directory (and register all file extents) */
int  x2f_prefetch_all(x2f_t *x);

#ifdef __cplusplus
}
#endif
#endif
