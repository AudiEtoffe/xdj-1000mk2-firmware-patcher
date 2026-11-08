/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
/*
 * xdj_exfat - firmware-side wrapper for the XDJ-1000MK2 (v1.45) disk I/O callback.
 *
 * Replaces the callback that attach_drive hands to ini_disk_sect. For every disk it forwards to the
 * original callback, except that when the volume turns out to be exFAT it answers reads from the
 * x2f layer (virtual read-only FAT32) so the player's existing FAT driver can mount it.
 *
 * Callback signature (read from the firmware): io(disk, cmd, lba, buf, count)
 *   cmd 1 = read, 2 = write, 5 = init (every attach), 7/8/10 = other; LBA is partition-relative
 *   (the original callback adds the partition start); count is the 5th argument (16-bit);
 *   return 0 on success.
 *
 * Firmware services are resolved by the linker (--defsym):
 *   fw_alloc(bytes) -> ptr | 0      fw_free(ptr)      fw_orig_io(disk, cmd, lba, buf, count)
 */
#include "x2f.h"

extern void *fw_alloc(unsigned bytes);
extern void  fw_free(void *p);
extern int   fw_orig_io(void *disk, int cmd, unsigned lba, void *buf, unsigned count);

#ifdef XDJ_TEST
extern unsigned long xdj_test_disk_base;
#define DISK_BASE xdj_test_disk_base
#else
#define DISK_BASE 0x0B0F9224ul   /* per-disk records, as used by attach_drive */
#endif
#define DISK_STRIDE 0x1E8ul

/* Diagnostics: the firmware keeps its version text ("1.47.") in RAM; this wrapper overwrites the last two
 * characters with a status letter and a hex digit so the state can be read on the player's version screen.
 * Letters, in order of progress:  I attach seen   P ordinary (non-exFAT) volume   X exFAT found, mount failed
 * U exFAT found, virtual volume could not be built   A virtual volume active   B FAT driver is reading the
 * FAT   C a directory was listed   D file data was read   E an error was returned (digit = error code).
 * The digit after A/B/C/D is the number of directories generated (0-F); after X/U/E it is the error code. */
#ifdef XDJ_TEST
extern char *xdj_test_ver;
#define VER_PTR xdj_test_ver
#define DATE_PTR (xdj_test_ver + 8)
#else
#ifdef XDJ_QUIET
static char g_vdummy[4];                 /* release build: the diagnostic status writes go to an unused buffer */
#define VER_PTR g_vdummy
#else
#define VER_PTR ((char *)0x08000740u)
#endif
#endif
static const char DIAG_ORDER[] = "IRQNPXUAFGBCDE";
static int diag_rank(char c) { int i; for (i = 0; DIAG_ORDER[i]; i++) if (DIAG_ORDER[i] == c) return i; return -1; }
static char hexd(unsigned v) { v &= 15; return (char)(v < 10 ? '0' + v : 'A' + v - 10); }
static void diag_force(char letter, unsigned code) { VER_PTR[2] = letter; VER_PTR[3] = hexd(code); }
static void diag_raise(char letter, unsigned code) {
    if (diag_rank(letter) > diag_rank(VER_PTR[2])) diag_force(letter, code);
}
#define NDISK 4

#define DIR_CACHE_BYTES (1024u * 1024u)  /* generated-directory cache budget */

#define MODE_UNKNOWN 0
#define MODE_PASS    1
#define MODE_EXFAT   2

typedef struct {
    int mode;
    int tries;                 /* own sector-0 reads attempted */
    void *disk;                /* the firmware's disk record */
    uint32_t start;            /* absolute LBA of the exFAT volume on the device */
    exfat_vol vol;
    uint8_t sbuf[512];
    uint8_t fbuf[512];
    x2f_t *x;
    uint32_t on;               /* RAM overlay: sectors the player wrote that are NOT file data (FAT, directories, ...) */
    uint32_t *okey;            /* ... their virtual LBAs */
    uint8_t *odata;            /* ... and contents (OVL_CAP sectors, allocated on first use) */
} dstate;
#define OVL_CAP 40u

/* static state: no heap allocation is needed to start looking at a stick (initialised to zero in the image) */
static dstate *g_ds[NDISK];    /* per-drive state: allocated from the player's heap the first time a drive attaches (keeps the firmware's free space for code) */
static unsigned g_reads;       /* diagnostic: read commands seen since the last attach (saturates at 15) */
static unsigned g_served;      /* diagnostic: reads served from the virtual volume since the attach (saturates at 15) */
static unsigned g_last_err;    /* diagnostic: error code of the last failed read */
static int g_last_cmd, g_last_ret;   /* diagnostic: the last command (not init) the player sent, and what we returned */
static unsigned g_ready;       /* == READY_MAGIC once g_dstate has been cleared */
#define READY_MAGIC 0x600DF00Du

/* The state lives in the 0xFF padding of the firmware image, so its initial contents are NOT zero.
 * Clear it at run time, once, before anything looks at it. */
static void init_state_once(void) {
    unsigned i;
    if (g_ready == READY_MAGIC) return;
    for (i = 0; i < NDISK; i++) g_ds[i] = 0;
    g_reads = 0;
    g_served = 0;
    g_ready = READY_MAGIC;
}
static dstate *get_state(int idx) {
    dstate *st;
    init_state_once();
    if (idx < 0 || idx >= NDISK) return 0;
    st = g_ds[idx];
    if (!st) {
        unsigned char *q; unsigned long n = sizeof(dstate);
        st = (dstate *)fw_alloc((unsigned)n);
        if (!st) return 0;
        for (q = (unsigned char *)st; n--; ) *q++ = 0;
        g_ds[idx] = st;
    }
    return st;
}

#ifndef XDJ_TEST
/* the compiler may emit calls to these for struct copies; the firmware blob has no libc */
void *memcpy(void *d, const void *s, __SIZE_TYPE__ n) {
    unsigned char *a = (unsigned char *)d; const unsigned char *b = (const unsigned char *)s;
    while (n--) *a++ = *b++;
    return d;
}
void *memset(void *d, int c, __SIZE_TYPE__ n) {
    unsigned char *a = (unsigned char *)d;
    while (n--) *a++ = (unsigned char)c;
    return d;
}
#endif

/* answer a read from the virtual volume and update the diagnostic status */
/* ---- writes (stage B: save in place) ----------------------------------------------------------------------------
 * The player believes it is writing to a FAT32 drive. A write is applied to the real stick ONLY if it lands inside the data of
 * an existing file (x2f_data_map): then the same bytes of the real exFAT file are overwritten, and no exFAT structure changes.
 * Every other write (boot/FSInfo sectors, the FAT, directories, free space) is kept in a small RAM overlay so the player sees
 * a consistent drive for this session; it is never written to the stick and is dropped at the next attach. */
static uint32_t disk44(void *disk);
static unsigned g_wr_data, g_wr_ovl;   /* diagnostic: write operations that went to the stick / to the RAM overlay (saturate at 9) */
static int vol_write(dstate *st, uint32_t real, uint32_t n, const void *buf) {
    return fw_orig_io(st->disk, 2, real + st->start - disk44(st->disk), (void *)buf, n) == 0 ? 0 : -1;
}
static int ovl_put(dstate *st, uint32_t lba, const uint8_t *src) {
    uint32_t i, k;
    if (!st->okey) {
        st->okey = (uint32_t *)fw_alloc(OVL_CAP * 4);
        st->odata = (uint8_t *)fw_alloc(OVL_CAP * 512);
        if (!st->okey || !st->odata) {
            if (st->okey) fw_free(st->okey);
            if (st->odata) fw_free(st->odata);
            st->okey = 0; st->odata = 0; return -1;
        }
    }
    for (i = 0; i < st->on; i++) if (st->okey[i] == lba) break;
    if (i == st->on) { if (st->on >= OVL_CAP) return -1; st->okey[st->on++] = lba; }
    for (k = 0; k < 512; k++) st->odata[i * 512 + k] = src[k];
    return 0;
}
static void ovl_apply(const dstate *st, uint32_t lba, uint32_t count, uint8_t *buf) {
    uint32_t i, k;
    for (i = 0; i < st->on; i++)
        if (st->okey[i] >= lba && st->okey[i] - lba < count)
            for (k = 0; k < 512; k++) buf[(st->okey[i] - lba) * 512 + k] = st->odata[i * 512 + k];
}
static int do_write(dstate *st, uint32_t lba, uint32_t count, const uint8_t *buf) {
    uint32_t done = 0;
    if ((uint64_t)lba + count > x2f_total(st->x)) return -1;
    while (done < count) {
        uint32_t real = 0, run = x2f_data_map(st->x, lba + done, count - done, &real);
        if (run) {
            if (vol_write(st, real, run, buf + done * 512)) return -1;
            if (g_wr_data < 9) g_wr_data++;
            done += run;
        } else {
            if (ovl_put(st, lba + done, buf + done * 512)) return -1;
            if (g_wr_ovl < 9) g_wr_ovl++;
            done++;
        }
    }
    VER_PTR[2] = (char)('0' + g_wr_data); VER_PTR[3] = (char)('0' + g_wr_ovl);
    return 0;
}

static int serve(dstate *st, unsigned lba, unsigned count, void *buf) {
    unsigned dirs, data, fat;
    int r = x2f_read(st->x, lba, count, buf);
    if (r != X2F_OK) { g_last_err = (unsigned)-r; diag_raise('E', (unsigned)-r); return -1; }
    if (st->on) ovl_apply(st, lba, count, (uint8_t *)buf);
    if (g_served < 15) g_served++;
    x2f_stats(st->x, &dirs, &data, &fat);
    if (data) diag_raise('D', dirs);
    else if (dirs) diag_raise('C', dirs);
    else if (fat) diag_raise('B', g_served);
    else {
        /* the player has only read the reserved area so far. Digits only (never confused with the progress letters):
         * [2] = 2 * min(reads served so far, 4) + (1 if the LAST read was more than one sector)
         * [3] = start LBA of the LAST read: 0-7 exact, 8 = LBA 8-31, 9 = LBA 32 or more */
        VER_PTR[2] = (char)('0' + (g_served < 4 ? g_served : 4) * 2 + (count > 1 ? 1 : 0));
        VER_PTR[3] = (char)('0' + (lba < 8 ? lba : lba < 32 ? 8 : 9));
    }
    if (diag_rank(VER_PTR[2]) == diag_rank('B')) VER_PTR[3] = hexd(g_served);
    else if (diag_rank(VER_PTR[2]) >= diag_rank('C') && diag_rank(VER_PTR[2]) <= diag_rank('D')) VER_PTR[3] = hexd(dirs);
    return 0;
}

static void m_free_ctx(void *ctx, void *p) { (void)ctx; fw_free(p); }
static void *m_alloc_ctx(void *ctx, uint32_t n) { (void)ctx; return fw_alloc(n); }
/* the firmware's callback adds the record's partition start (disk[44]) to every LBA; that value is NOT stable during
 * an attach (ct_mount_disk resets it to 0), so every device access here is expressed as an absolute LBA and the
 * relative LBA is recomputed from the live disk[44] at the moment of the call. */
static uint32_t disk44(void *disk) { return *(volatile uint32_t *)((char *)disk + 44); }
static int rd_abs(void *disk, uint32_t abs, void *buf) {
    return fw_orig_io(disk, 1, abs - disk44(disk), buf, 1) == 0 ? 0 : -1;
}
static int vol_read(void *ctx, uint32_t lba, uint32_t n, void *buf) {
    dstate *st = (dstate *)ctx;
    return fw_orig_io(st->disk, 1, lba + st->start - disk44(st->disk), buf, n) == 0 ? 0 : -1;
}

static int disk_index_of(void *disk);

static void reset_state(dstate *st) {
    if (st->x) x2f_close(st->x);
    if (st->okey) m_free_ctx(0, st->okey);
    if (st->odata) m_free_ctx(0, st->odata);
    st->okey = 0; st->odata = 0; st->on = 0;
    st->x = 0; st->mode = MODE_UNKNOWN; st->tries = 0;
}

static int same8(const uint8_t *a, const char *b) { unsigned i; for (i = 0; i < 8; i++) if (a[i] != (uint8_t)b[i]) return 0; return 1; }
/* Hook for the attach routine's call to fs_get_partition (0x093A4A8C): shows how far the attach gets, even for a stick
 * that never reaches xdj_io. "F" + partition count (hex), or "FE" if the function failed. A later "I"/"P"/"A" status replaces it. */
#if !defined(XDJ_TEST) && !defined(XDJ_QUIET)
/* Breadcrumbs from the USB mass-storage stack's own debug messages: every message goes through one print function
 * (0x0926B064, all arguments on the stack, the format string on top). The literal pointing at it, inside the USB code,
 * is redirected to xdj_pf, which records which known message was printed and then jumps to the real function. */
static unsigned g_pf_seen;
static const struct { unsigned short lo; char c, d; } PFTAB[] = {
    {0xEE38,'G','1'},{0xEEE8,'G','2'},{0xEF54,'G','3'},{0xEFB0,'G','4'},{0xEFE0,'G','5'},{0xF00C,'G','6'},{0xF350,'U','4'}
};
void xdj_pf_note(const char *fmt) {
    unsigned a = (unsigned)fmt & 0x1FFFFFFFu, i;
    g_pf_seen = 1;
    if ((a >> 16) != 0x0808) return;
    /* mount-stage failures (refresh_cache failed = K, ct_mount_disk failed = T): if the last request to us had FAILED show
     * Z + our read error code (last request was a read) or Y + the command number; otherwise letter + reads served */
    if ((a & 0xFFFF) == 0xF040 || (a & 0xFFFF) == 0xF068) {
        if (g_last_ret != 0) {
            if (g_last_cmd == 1) { VER_PTR[2] = 'Z'; VER_PTR[3] = hexd(g_last_err); }
            else { VER_PTR[2] = 'Y'; VER_PTR[3] = hexd((unsigned)g_last_cmd); }
        } else {
            VER_PTR[2] = (a & 0xFFFF) == 0xF040 ? 'K' : 'T'; VER_PTR[3] = hexd(g_served);
        }
        return;
    }
    for (i = 0; i < sizeof PFTAB / sizeof PFTAB[0]; i++)
        if (PFTAB[i].lo == (a & 0xFFFF)) { VER_PTR[2] = PFTAB[i].c; VER_PTR[3] = PFTAB[i].d; return; }
}
__asm__(
    ".section .text.xdj_pf,\"ax\"\n.align 2\n.global xdj_pf\nxdj_pf:\n"
    "  sts.l pr,@-r15\n"
    "  mov.l @(4,r15),r4\n"
    "  mov.l 1f,r1\n"
    "  jsr @r1\n"
    "  nop\n"
    "  lds.l @r15+,pr\n"
    "  mov.l 2f,r1\n"
    "  jmp @r1\n"
    "  nop\n"
    "  .align 2\n"
    "1: .long xdj_pf_note\n"
    "2: .long 0x0926B064\n"
    ".text\n");
#else
#define g_pf_seen 0
#endif

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* is there an exFAT boot sector at absolute LBA `abs`? (uses sbuf as scratch; on success the sector is copied to fbuf) */
static int try_start(dstate *st, uint32_t abs) {
    if (rd_abs(st->disk, abs, st->sbuf) != 0) return 0;
    if (!exfat_probe(st->sbuf, 512)) return 0;
    memcpy(st->fbuf, st->sbuf, 512);
    st->start = abs;
    return 1;
}

/* fbuf holds absolute sector 0. Look for an exFAT partition in an MBR or GPT. Returns 1 if found (st->start set);
 * *kind = 1 if an MBR was examined, 2 if a GPT was. */
static int find_partition(dstate *st, unsigned *kind) {
    uint8_t *b = st->fbuf;
    int i, gpt = 0;
    if (b[510] != 0x55 || b[511] != 0xAA) return 0;
    if (b[0] == 0xEB || b[0] == 0xE9) {              /* jump + printable OEM name = a volume boot sector (FAT32/NTFS/...), not an MBR */
        for (i = 3; i < 11 && b[i] >= 0x20 && b[i] < 0x7F; i++) {}
        if (i == 11) return 0;
    }
    *kind = 1;
    for (i = 0; i < 4; i++) {
        const uint8_t *e = b + 446 + 16 * i;
        uint32_t s = le32(e + 8), n = le32(e + 12);
        if ((e[0] & 0x7F) != 0) continue;             /* boot flag must be 0x00 or 0x80 */
        if (e[4] == 0xEE) { gpt = 1; continue; }
        if (e[4] == 0 || e[4] == 0x05 || e[4] == 0x0F || e[4] == 0x85 || s == 0 || n == 0) continue;
        if (try_start(st, s)) return 1;
    }
    if (gpt) {
        uint32_t elba, ecount, esz, k;
        *kind = 2;
        if (rd_abs(st->disk, 1, st->sbuf) != 0) return 0;
        if (!same8(st->sbuf, "EFI PART")) return 0;
        elba = le32(st->sbuf + 72); ecount = le32(st->sbuf + 80); esz = le32(st->sbuf + 84);
        if (le32(st->sbuf + 76) != 0 || esz < 128 || esz > 512 || (esz & 7)) return 0;
        if (ecount > 32) ecount = 32;
        for (k = 0; k < ecount; k++) {
            const uint8_t *e;
            uint32_t off = k * esz, s, j, nz = 0;
            if (rd_abs(st->disk, elba + off / 512, st->fbuf) != 0) return 0;
            e = st->fbuf + (off % 512);
            for (j = 0; j < 16; j++) nz |= e[j];
            s = le32(e + 32);
            if (!nz || s == 0 || le32(e + 36) != 0) continue;
            if (try_start(st, s)) return 1;
            /* try_start left the entry sector in fbuf intact on failure */
        }
    }
    return 0;
}

/* first read of any kind on a disk: look at the real device ourselves and decide */
static void probe(dstate *st, void *disk) {
    x2f_mem mem;
    int r;
    unsigned kind = 0, how = 0;
    diag_raise('Q', 0);
    st->tries++;
    st->disk = disk;
    r = fw_orig_io(disk, 1, 0, st->fbuf, 1);       /* the sector at the player's current partition start */
    if (r != 0) {                                   /* could not read: retry a few times */
        diag_raise('N', (unsigned)(r < 0 ? -r : r));
        if (st->tries >= 4) st->mode = MODE_PASS;
        return;
    }
    st->mode = MODE_PASS;
    if (exfat_probe(st->fbuf, 512)) {
        st->start = disk44(disk);
    } else {
        /* the player's partition start may be 0 right now (ct_mount_disk resets it): find the partition ourselves */
        if (disk44(disk) != 0 && rd_abs(disk, 0, st->fbuf) != 0) { diag_raise('P', 0 | (g_pf_seen ? 4 : 0)); return; }
        if (exfat_probe(st->fbuf, 512)) {              /* absolute sector 0 is itself the exFAT boot sector (no partition table) */
            st->start = 0;
        } else {
            if (!find_partition(st, &kind)) { diag_raise('P', kind | (g_pf_seen ? 4 : 0)); return; }
            how = kind;
        }
    }
    mem.alloc = m_alloc_ctx; mem.release = m_free_ctx; mem.ctx = 0;
    r = exfat_mount(&st->vol, vol_read, st, 0, st->sbuf, st->fbuf, sizeof st->sbuf);
    if (r != EXFAT_OK) { diag_raise('X', (unsigned)-r); return; }
    r = x2f_open(&st->x, &st->vol, &mem, DIR_CACHE_BYTES);
    if (r != X2F_OK) { st->x = 0; diag_raise('U', (unsigned)-r); return; }
    st->mode = MODE_EXFAT;
    diag_raise('A', how);
}

#ifdef XDJ_TEST
#define VIEW_MASK(x) (x)
#else
#define VIEW_MASK(x) ((x) & 0x1FFFFFFFul)     /* the same RAM can be addressed through P0/P1/P2/P3 */
#endif
static int disk_index_of(void *disk) {
    unsigned long i;
    for (i = 0; i < NDISK; i++)
        if (VIEW_MASK((unsigned long)disk) == VIEW_MASK(DISK_BASE + i * DISK_STRIDE)) return (int)i;
    return -1;
}

static int xdj_io_inner(void *disk, int cmd, unsigned lba, void *buf, unsigned count) {
    int idx = disk_index_of(disk);
    dstate *st;
    if (idx < 0) return fw_orig_io(disk, cmd, lba, buf, count);
    st = get_state(idx);
    if (!st) return fw_orig_io(disk, cmd, lba, buf, count);     /* no memory for our state: behave as the stock firmware */

    if (cmd == 5) {                              /* new attach: forget everything about this disk */
        reset_state(st);
        g_reads = 0;
        g_served = 0;
        g_wr_data = 0; g_wr_ovl = 0;
        diag_force('I', 0);
        return fw_orig_io(disk, cmd, lba, buf, count);
    }
    if (cmd == 1) {
        count &= 0xFFFF;
        if (g_reads < 15) g_reads++;
        if (diag_rank(VER_PTR[2]) <= diag_rank('R')) diag_force('R', g_reads);
        if (st->mode == MODE_UNKNOWN) probe(st, disk);
        if (st->mode == MODE_EXFAT) return serve(st, lba, count, buf);
        return fw_orig_io(disk, cmd, lba, buf, count);
    }
    if (cmd == 2) {
        if (st->mode == MODE_UNKNOWN) probe(st, disk);
        if (st->mode == MODE_EXFAT) return do_write(st, lba, count & 0xFFFF, (const uint8_t *)buf);
    }
    return fw_orig_io(disk, cmd, lba, buf, count);
}

int xdj_io(void *disk, int cmd, unsigned lba, void *buf, unsigned count) {
    int r = xdj_io_inner(disk, cmd, lba, buf, count);
    if (cmd != 5) { g_last_cmd = cmd; g_last_ret = r; }
    return r;
}

#ifdef XDJ_TEST
int xdj_test_prefetch(int idx) {
    return (idx >= 0 && idx < NDISK && g_ds[idx] && g_ds[idx]->x) ? x2f_prefetch_all(g_ds[idx]->x) : -1;
}
#endif

/* Hook for the media-identify routine (0xA9281416, run by command 7 / ct_mount_disk). It reads ABSOLUTE sector 0 of the stick
 * straight from the USB layer - bypassing the I/O callback - and passes it to the partition/filesystem classifier
 * (0x093A52D2), which only recognises FAT. For a stick the layer has mounted as exFAT, hand the classifier the virtual FAT32
 * boot sector instead of the real exFAT boot sector / MBR. */
#ifdef XDJ_TEST
extern int xdj_test_parse(void *disk, void *buf);
#define ORIG_PARSE1 xdj_test_parse
#define ORIG_PARSE2 xdj_test_parse
#define ORIG_PARSE3 xdj_test_parse
#else
#define ORIG_PARSE1 ((int (*)(void *, void *))0x093A52D2)   /* sector 0 -> partition table / FAT detect */
#define ORIG_PARSE2 ((int (*)(void *, void *))0x093A503C)   /* boot sector at the partition start */
#define ORIG_PARSE3 ((int (*)(void *, void *))0x093A5008)   /* sector at start + disk[54] (the FSInfo sector) */
#endif
/* the identify routine reads three real sectors straight from the USB layer; for an exFAT disk replace each with the matching
 * sector of the virtual FAT32 volume (rel = partition-relative LBA) before the classifier looks at it */
static void ident_sub(void *disk, void *buf, unsigned rel) {
    int idx = disk_index_of(disk);
    if (idx >= 0) {
        dstate *st = get_state(idx);
        if (!st) return;
        if (st->mode == MODE_UNKNOWN) probe(st, disk);
        if (st->mode == MODE_EXFAT && x2f_read(st->x, rel, 1, buf) == X2F_OK && st->on) ovl_apply(st, rel, 1, (uint8_t *)buf);
    }
}
int xdj_ident(void *disk, void *buf)  { ident_sub(disk, buf, 0); return ORIG_PARSE1(disk, buf); }
int xdj_ident2(void *disk, void *buf) { ident_sub(disk, buf, 0); return ORIG_PARSE2(disk, buf); }
int xdj_ident3(void *disk, void *buf) { ident_sub(disk, buf, *(volatile uint16_t *)((char *)disk + 54)); return ORIG_PARSE3(disk, buf); }
