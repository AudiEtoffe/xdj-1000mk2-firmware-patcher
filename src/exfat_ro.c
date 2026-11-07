/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
/* exfat_ro - small read-only exFAT driver. See exfat_ro.h. */
#include "exfat_ro.h"

#define NONE_LBA 0xFFFFFFFFu
#define UNKNOWN_SIZE (~(uint64_t)0)

/* ---- little-endian field access (alignment/endian neutral) ---- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static void mcopy(void *d, const void *s, uint32_t n) {
    uint8_t *a = (uint8_t *)d; const uint8_t *b = (const uint8_t *)s;
    while (n--) *a++ = *b++;
}
static void mzero(void *d, uint32_t n) { uint8_t *a = (uint8_t *)d; while (n--) *a++ = 0; }

/* ---- probing / mounting ---- */

int exfat_probe(const uint8_t *s, uint32_t len) {
    static const char sig[8] = { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };
    uint32_t i, ss;
    if (len < 512) return 0;
    for (i = 0; i < 8; i++) if (s[3 + i] != (uint8_t)sig[i]) return 0;
    for (i = 11; i < 64; i++) if (s[i]) return 0;           /* MustBeZero */
    if (s[108] < 9 || s[108] > 12) return 0;
    ss = (uint32_t)1 << s[108];
    if (len >= ss && (s[ss - 2] != 0x55 || s[ss - 1] != 0xAA)) return 0;
    return 1;
}

static int dev_read(exfat_vol *v, uint32_t lba, uint32_t n, void *buf) {
    return v->read(v->ctx, v->base_lba + lba, n, buf) ? EXFAT_ERR_IO : 0;
}

int exfat_mount(exfat_vol *v, exfat_read_fn rd, void *ctx, uint32_t base_lba,
                uint8_t *sbuf, uint8_t *fbuf, uint32_t buf_size) {
    uint32_t fat_off, fat_len, heap_off, ccount, root, ss_shift, spc_shift;
    uint8_t nfats;
    uint16_t rev, flags;
    if (buf_size < 512) return EXFAT_ERR_FORMAT;
    v->read = rd; v->ctx = ctx; v->base_lba = base_lba;
    v->sbuf = sbuf; v->fbuf = fbuf;
    v->fbuf_lba = NONE_LBA; v->sbuf_lba = NONE_LBA;
    if (dev_read(v, 0, 1, sbuf)) return EXFAT_ERR_IO;
    if (!exfat_probe(sbuf, buf_size)) return EXFAT_ERR_FORMAT;

    ss_shift = sbuf[108]; spc_shift = sbuf[109]; nfats = sbuf[110];
    fat_off = rd32(sbuf + 80); fat_len = rd32(sbuf + 84);
    heap_off = rd32(sbuf + 88); ccount = rd32(sbuf + 92); root = rd32(sbuf + 96);
    rev = rd16(sbuf + 104); flags = rd16(sbuf + 106);

    if ((rev >> 8) != 1) return EXFAT_ERR_FORMAT;
    if ((uint32_t)1 << ss_shift > buf_size) return EXFAT_ERR_FORMAT;
    if (ss_shift + spc_shift > 25) return EXFAT_ERR_FORMAT;
    if (nfats != 1 && nfats != 2) return EXFAT_ERR_FORMAT;
    if (ccount == 0 || ccount > 0xFFFFFFF5u) return EXFAT_ERR_FORMAT;
    if (root < 2 || root - 2 >= ccount) return EXFAT_ERR_FORMAT;
    if (fat_off < 24 || fat_len == 0 || heap_off < fat_off) return EXFAT_ERR_FORMAT;

    v->sector_shift = ss_shift;
    v->sector_size = (uint32_t)1 << ss_shift;
    v->cluster_shift = spc_shift;
    v->cluster_size = v->sector_size << spc_shift;
    v->fat_lba = fat_off;
    if (nfats == 2 && (flags & 1)) v->fat_lba = fat_off + fat_len;   /* TexFAT: active FAT */
    v->heap_lba = heap_off;
    v->cluster_count = ccount;
    v->root_cluster = root;
    {
        uint64_t vl = rd64(sbuf + 72);
        v->volume_sectors = vl > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)vl;
    }
    return EXFAT_OK;
}

int exfat_root(exfat_vol *v, exfat_node *out) {
    out->first_cluster = v->root_cluster;
    out->size = UNKNOWN_SIZE;          /* root has no length: follow the FAT chain */
    out->valid_size = UNKNOWN_SIZE;
    out->attr = EXFAT_ATTR_DIR;
    out->no_fat_chain = 0;
    return EXFAT_OK;
}

/* ---- FAT / cluster access ---- */

static int valid_cluster(const exfat_vol *v, uint32_t c) { return c >= 2 && c - 2 < v->cluster_count; }

/* 1 = *next valid, 0 = end of chain, <0 = error */
static int fat_next(exfat_vol *v, uint32_t c, uint32_t *next) {
    uint32_t per = v->sector_shift - 2;
    uint32_t lba = v->fat_lba + (c >> per);
    uint32_t val;
    if (!valid_cluster(v, c)) return EXFAT_ERR_CORRUPT;
    if (v->fbuf_lba != lba) {
        if (dev_read(v, lba, 1, v->fbuf)) { v->fbuf_lba = NONE_LBA; return EXFAT_ERR_IO; }
        v->fbuf_lba = lba;
    }
    val = rd32(v->fbuf + ((c & (((uint32_t)1 << per) - 1)) << 2));
    if (val >= 0xFFFFFFF8u) return 0;
    if (!valid_cluster(v, val)) return EXFAT_ERR_CORRUPT;   /* free (0), bad (F7) or out of range */
    *next = val;
    return 1;
}

/* read len bytes at byte offset `off` inside cluster c; the range must not leave the cluster */
static int read_in_cluster(exfat_vol *v, uint32_t c, uint32_t off, uint8_t *dst, uint32_t len) {
    uint32_t lba0, sec, so, n, take, step;
    if (!valid_cluster(v, c)) return EXFAT_ERR_CORRUPT;
    lba0 = v->heap_lba + ((c - 2) << v->cluster_shift);
    while (len) {
        sec = off >> v->sector_shift;
        so = off & (v->sector_size - 1);
        if (so == 0 && len >= v->sector_size) {
            n = len >> v->sector_shift;
            if (dev_read(v, lba0 + sec, n, dst)) return EXFAT_ERR_IO;
            step = n << v->sector_shift;
        } else {
            if (v->sbuf_lba != lba0 + sec) {
                if (dev_read(v, lba0 + sec, 1, v->sbuf)) { v->sbuf_lba = NONE_LBA; return EXFAT_ERR_IO; }
                v->sbuf_lba = lba0 + sec;
            }
            take = v->sector_size - so;
            if (take > len) take = len;
            mcopy(dst, v->sbuf + so, take);
            step = take;
        }
        dst += step; off += step; len -= step;
    }
    return 0;
}

/* ---- directories ---- */

int exfat_opendir(exfat_vol *v, const exfat_node *dir, exfat_dir *d) {
    if (!(dir->attr & EXFAT_ATTR_DIR)) return EXFAT_ERR_NOTDIR;
    d->vol = v; d->dir = *dir;
    d->cluster = dir->first_cluster; d->off = 0; d->pos = 0; d->hops = 0;
    d->done = (dir->first_cluster == 0);
    return EXFAT_OK;
}

/* next raw 32-byte directory entry: 1 = got one, 0 = end, <0 = error */
static int dir_raw(exfat_dir *d, uint8_t *out) {
    exfat_vol *v = d->vol;
    uint32_t next;
    int r;
    if (d->done) return 0;
    if (d->dir.size != UNKNOWN_SIZE && d->pos >= d->dir.size) { d->done = 1; return 0; }
    if (d->off >= v->cluster_size) {
        if (d->dir.no_fat_chain) {
            next = d->cluster + 1;
            if (!valid_cluster(v, next)) { d->done = 1; return EXFAT_ERR_CORRUPT; }
        } else {
            r = fat_next(v, d->cluster, &next);
            if (r == 0) { d->done = 1; return 0; }
            if (r < 0) return r;
        }
        d->cluster = next; d->off = 0;
        if (++d->hops > v->cluster_count) return EXFAT_ERR_CORRUPT;
    }
    r = read_in_cluster(v, d->cluster, d->off, out, 32);
    if (r) return r;
    d->off += 32; d->pos += 32;
    return 1;
}

static uint16_t set_sum(uint16_t s, const uint8_t *b, int first) {
    int i;
    for (i = 0; i < 32; i++) {
        if (first && (i == 2 || i == 3)) continue;
        s = (uint16_t)((((s & 1) ? 0x8000u : 0u) | (s >> 1)) + b[i]);
    }
    return s;
}

int exfat_readdir(exfat_dir *d, exfat_dirent *e) {
    uint8_t ent[32], sec_ent[32];
    for (;;) {
        int r = dir_raw(d, ent);
        uint8_t secs, nlen = 0, flags = 0;
        uint16_t want, sum, attr;
        uint32_t first = 0;
        uint64_t vlen = 0, dlen = 0;
        int i, ok = 1, name_units;
        if (r <= 0) return r;
        if (ent[0] == 0x00) { d->done = 1; return 0; }           /* end-of-directory marker */
        if (ent[0] != 0x85) continue;                             /* deleted, bitmap, upcase, label... */
        secs = ent[1];
        if (secs < 2 || secs > 18) continue;
        want = rd16(ent + 2);
        attr = rd16(ent + 4);
        sum = set_sum(0, ent, 1);
        for (i = 1; i <= secs; i++) {
            r = dir_raw(d, sec_ent);
            if (r < 0) return r;
            if (r == 0) { ok = 0; break; }
            sum = set_sum(sum, sec_ent, 0);
            if (i == 1) {
                if (sec_ent[0] != 0xC0) { ok = 0; break; }
                flags = sec_ent[1]; nlen = sec_ent[3];
                vlen = rd64(sec_ent + 8); first = rd32(sec_ent + 20); dlen = rd64(sec_ent + 24);
                name_units = nlen;
                if (nlen == 0 || ((nlen + 14) / 15) > secs - 1) { ok = 0; /* keep consuming the set */ }
                (void)name_units;
            } else if (sec_ent[0] == 0xC1 && ok) {
                int base = (i - 2) * 15, k;
                for (k = 0; k < 15; k++)
                    if (base + k < nlen) e->name16[base + k] = rd16(sec_ent + 2 + 2 * k);
            }
        }
        if (!ok || sum != want) continue;                         /* corrupt or incomplete set: skip */
        e->name16[nlen] = 0;
        e->name_len = nlen;
        e->ctime = rd32(ent + 8); e->mtime = rd32(ent + 12); e->atime = rd32(ent + 16);
        e->node.attr = (uint8_t)(attr & 0xFF);
        e->node.no_fat_chain = (flags & 2) ? 1 : 0;
        e->node.first_cluster = first;
        e->node.size = dlen;
        e->node.valid_size = vlen;
        return 1;
    }
}

/* ---- low-level access ---- */

int exfat_fat_entry(exfat_vol *v, uint32_t c, uint32_t *raw) {
    uint32_t per = v->sector_shift - 2;
    uint32_t lba = v->fat_lba + (c >> per);
    if (!valid_cluster(v, c)) return EXFAT_ERR_CORRUPT;
    if (v->fbuf_lba != lba) {
        if (dev_read(v, lba, 1, v->fbuf)) { v->fbuf_lba = NONE_LBA; return EXFAT_ERR_IO; }
        v->fbuf_lba = lba;
    }
    *raw = rd32(v->fbuf + ((c & (((uint32_t)1 << per) - 1)) << 2));
    return EXFAT_OK;
}

int exfat_read_sectors(exfat_vol *v, uint32_t lba, uint32_t nsect, void *buf) {
    return dev_read(v, lba, nsect, buf);
}

int exfat_count_clusters(exfat_vol *v, uint32_t first, uint32_t *count) {
    uint32_t c = first, n = 0, nx;
    if (first == 0) { *count = 0; return EXFAT_OK; }
    for (;;) {
        int r;
        n++;
        if (n > v->cluster_count) return EXFAT_ERR_CORRUPT;
        r = fat_next(v, c, &nx);
        if (r == 0) break;
        if (r < 0) return r;
        c = nx;
    }
    *count = n;
    return EXFAT_OK;
}

/* ---- names ---- */

static int utf8_to_utf16_n(const char *src, int slen, uint16_t *out, int max_units) {
    const uint8_t *s = (const uint8_t *)src;
    int i = 0, n = 0;
    while (i < slen) {
        uint32_t c = s[i++];
        int extra, k;
        if (c < 0x80) { extra = 0; }
        else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
        else return EXFAT_ERR_NAME;
        for (k = 0; k < extra; k++) {
            if (i >= slen || (s[i] & 0xC0) != 0x80) return EXFAT_ERR_NAME;
            c = (c << 6) | (s[i++] & 0x3F);
        }
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return EXFAT_ERR_NAME;
        if (c >= 0x10000) {
            if (n + 2 > max_units) return EXFAT_ERR_NAME;
            c -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (c >> 10));
            out[n++] = (uint16_t)(0xDC00 + (c & 0x3FF));
        } else {
            if (n + 1 > max_units) return EXFAT_ERR_NAME;
            out[n++] = (uint16_t)c;
        }
    }
    return n;
}

int exfat_utf8_to_utf16(const char *s, uint16_t *out, int max_units) {
    int len = 0;
    while (s[len]) len++;
    return utf8_to_utf16_n(s, len, out, max_units);
}

int exfat_utf16_to_utf8(const uint16_t *s, int units, char *out, int max_bytes) {
    int i = 0, n = 0;
    while (i < units) {
        uint32_t c = s[i++];
        int len;
        if (c >= 0xD800 && c <= 0xDBFF && i < units && s[i] >= 0xDC00 && s[i] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i++] - 0xDC00);
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        len = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (n + len + 1 > max_bytes) return EXFAT_ERR_NAME;
        if (len == 1) out[n++] = (char)c;
        else if (len == 2) { out[n++] = (char)(0xC0 | (c >> 6)); out[n++] = (char)(0x80 | (c & 0x3F)); }
        else if (len == 3) { out[n++] = (char)(0xE0 | (c >> 12)); out[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[n++] = (char)(0x80 | (c & 0x3F)); }
        else { out[n++] = (char)(0xF0 | (c >> 18)); out[n++] = (char)(0x80 | ((c >> 12) & 0x3F)); out[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[n++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[n] = 0;
    return n;
}

static uint16_t fold(uint16_t c) { return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32) : c; }

/* ---- path lookup ---- */

int exfat_lookup(exfat_vol *v, const char *path, exfat_node *out) {
    exfat_node cur;
    const char *p = path;
    exfat_root(v, &cur);
    while (*p == '/') p++;
    while (*p) {
        const char *q = p;
        uint16_t comp[EXFAT_MAX_NAME16 + 1];
        exfat_dirent e;
        exfat_dir d;
        int n, found = 0, r;
        while (*q && *q != '/') q++;
        n = utf8_to_utf16_n(p, (int)(q - p), comp, EXFAT_MAX_NAME16);
        if (n < 0) return n;
        if (n == 0) return EXFAT_ERR_NAME;
        if (!(cur.attr & EXFAT_ATTR_DIR)) return EXFAT_ERR_NOTDIR;
        r = exfat_opendir(v, &cur, &d);
        if (r) return r;
        for (;;) {
            int i, same = 1;
            r = exfat_readdir(&d, &e);
            if (r < 0) return r;
            if (r == 0) break;
            if (e.name_len != n) continue;
            for (i = 0; i < n; i++) if (fold(e.name16[i]) != fold(comp[i])) { same = 0; break; }
            if (same) { cur = e.node; found = 1; break; }
        }
        if (!found) return EXFAT_ERR_NOTFOUND;
        p = q;
        while (*p == '/') p++;
    }
    *out = cur;
    return EXFAT_OK;
}

/* ---- files ---- */

int exfat_fopen_node(exfat_vol *v, const exfat_node *n, exfat_file *f) {
    if (n->attr & EXFAT_ATTR_DIR) return EXFAT_ERR_ISDIR;
    f->vol = v; f->node = *n; f->pos = 0; f->cur_idx = 0; f->cur_cluster = 0;
    return EXFAT_OK;
}

int exfat_fopen(exfat_vol *v, const char *path, exfat_file *f) {
    exfat_node n;
    int r = exfat_lookup(v, path, &n);
    if (r) return r;
    return exfat_fopen_node(v, &n, f);
}

int32_t exfat_fread(exfat_file *f, void *buf, uint32_t len) {
    exfat_vol *v = f->vol;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t total = 0, cshift = v->sector_shift + v->cluster_shift;
    uint64_t rem;
    if (f->pos >= f->node.size) return 0;
    rem = f->node.size - f->pos;
    if ((uint64_t)len > rem) len = (uint32_t)rem;
    if (len && f->node.first_cluster == 0) return EXFAT_ERR_CORRUPT;
    while (len) {
        uint32_t cidx = (uint32_t)(f->pos >> cshift);
        uint32_t off = (uint32_t)(f->pos & (v->cluster_size - 1));
        uint32_t take = v->cluster_size - off, rd_bytes;
        if (take > len) take = len;
        if (f->node.no_fat_chain) {
            f->cur_cluster = f->node.first_cluster + cidx;
            f->cur_idx = cidx;
        } else {
            if (f->cur_cluster == 0 || cidx < f->cur_idx) { f->cur_idx = 0; f->cur_cluster = f->node.first_cluster; }
            while (f->cur_idx < cidx) {
                uint32_t nx;
                int r = fat_next(v, f->cur_cluster, &nx);
                if (r == 0) return EXFAT_ERR_CORRUPT;          /* chain shorter than the file */
                if (r < 0) return r;
                f->cur_cluster = nx; f->cur_idx++;
            }
        }
        /* bytes at or beyond ValidDataLength read as zero */
        if (f->pos >= f->node.valid_size) rd_bytes = 0;
        else if (f->pos + take > f->node.valid_size) rd_bytes = (uint32_t)(f->node.valid_size - f->pos);
        else rd_bytes = take;
        if (rd_bytes) {
            int r = read_in_cluster(v, f->cur_cluster, off, dst, rd_bytes);
            if (r) return r;
        }
        if (rd_bytes < take) mzero(dst + rd_bytes, take - rd_bytes);
        dst += take; len -= take; total += take; f->pos += take;
    }
    return (int32_t)total;
}

int exfat_fseek(exfat_file *f, int64_t off, int whence) {
    int64_t np;
    if (whence == 0) np = off;
    else if (whence == 1) np = (int64_t)f->pos + off;
    else if (whence == 2) np = (int64_t)f->node.size + off;
    else return EXFAT_ERR_FORMAT;
    if (np < 0 || (uint64_t)np > f->node.size) return EXFAT_ERR_FORMAT;
    f->pos = (uint64_t)np;
    return EXFAT_OK;
}

uint64_t exfat_ftell(const exfat_file *f) { return f->pos; }
int exfat_feof(const exfat_file *f) { return f->pos >= f->node.size; }
uint64_t exfat_filelen(const exfat_file *f) { return f->node.size; }
