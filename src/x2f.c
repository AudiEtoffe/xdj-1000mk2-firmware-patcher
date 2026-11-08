/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
/* x2f - virtual read-only FAT32 view of an exFAT volume. See x2f.h.
 *
 * Cluster numbering is virtual. Directories and files receive virtual clusters when their parent
 * directory is first generated, in blocks that start on a FAT-sector boundary (128 clusters), so
 * every FAT sector's content is final the first time it is served; a FAT driver may cache it forever.
 *   dir arena  [2, fstart)       the child directories of one parent are packed together
 *   file arena [fstart, fend)    all files of one directory are packed together (contiguous numbers)
 * A file's virtual clusters map to real clusters: first + k for contiguous files, or through a chain
 * table read lazily from the exFAT FAT for fragmented ones.
 */
#include "x2f.h"

#define SS 512u
#define RESERVED 32u
#define FAT_EOC 0x0FFFFFFFu
#define ALIGN 128u

typedef struct {
    uint32_t vstart, ncl;  /* virtual clusters */
    uint32_t rfirst;       /* real first cluster */
    uint32_t nreal;        /* real clusters the file occupies */
    uint32_t *chain;       /* real cluster of each virtual cluster (fragmented files), built lazily */
    uint8_t  nofat;
} x2f_ext;

typedef struct {
    uint32_t vstart, vres, vused;   /* virtual clusters: start, reserved (= FAT chain length), used */
    uint32_t xfirst, xlen;          /* exFAT first cluster and DataLength */
    uint32_t parent_v;
    uint32_t first_child, first_ext;/* where this directory's children/extents start in the arrays */
    uint8_t  nofat, is_root, generated;
    uint8_t *buf;                   /* generated content (vres clusters), NULL when not cached */
    uint32_t stamp;
} x2f_dir;

struct x2f {
    exfat_vol *v;
    x2f_mem mem;
    uint32_t spc, spc_shift, cs_shift;      /* VIRTUAL sectors per cluster (may be smaller than the real one) */
    uint32_t rshift;                        /* log2 of the REAL sectors per cluster */
    uint32_t ccount;
    uint32_t F, Sd, total;
    uint32_t fstart, fend;          /* file arena [fstart, fend); dir arena [2, fstart) */
    uint32_t dnext, fnext;          /* bump pointers, always 128-aligned between generations */
    x2f_ext *ext; uint32_t next, capx;
    x2f_dir *dirs; uint32_t nd, capd;
    uint32_t cache_bytes, cache_cap, stamp;
    uint32_t volid;
    uint32_t n_dirgen, n_data, n_fat;       /* diagnostics: directories generated, data reads, FAT sectors served */
    exfat_node t_node;              /* scratch used by dir_generate (kept off the stack) */
    exfat_dir t_it;
    exfat_dirent t_e;
};

/* ---------------- small helpers ---------------- */

static void mset(void *d, int c, uint32_t n) { uint8_t *p = (uint8_t *)d; while (n--) *p++ = (uint8_t)c; }
static void mcpy(void *d, const void *s, uint32_t n) {
    uint8_t *a = (uint8_t *)d; const uint8_t *b = (const uint8_t *)s;
    while (n--) *a++ = *b++;
}
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t align_up(uint32_t v) { return (v + ALIGN - 1) & ~(ALIGN - 1); }

static void *xalloc(x2f_t *x, uint32_t n) { return x->mem.alloc(x->mem.ctx, n); }
static void xfree(x2f_t *x, void *p) { if (p) x->mem.release(x->mem.ctx, p); }

/* ---------------- extents and directory registry ---------------- */

static int ext_append(x2f_t *x, uint32_t vstart, uint32_t ncl, uint32_t rfirst, uint32_t nreal, uint8_t nofat) {
    if (x->next == x->capx) {
        uint32_t ncap = x->capx ? x->capx * 2 : 1024;
        x2f_ext *ne = (x2f_ext *)xalloc(x, ncap * (uint32_t)sizeof(x2f_ext));
        if (!ne) return X2F_ERR_NOMEM;
        if (x->ext) { mcpy(ne, x->ext, x->next * (uint32_t)sizeof(x2f_ext)); xfree(x, x->ext); }
        x->ext = ne; x->capx = ncap;
    }
    x->ext[x->next].vstart = vstart; x->ext[x->next].ncl = ncl;
    x->ext[x->next].rfirst = rfirst; x->ext[x->next].nreal = nreal; x->ext[x->next].nofat = nofat; x->ext[x->next].chain = 0;
    x->next++;
    return X2F_OK;
}

static x2f_ext *ext_find(const x2f_t *x, uint32_t c) {
    uint32_t lo = 0, hi = x->next;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (x->ext[mid].vstart <= c) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return 0;
    if (c - x->ext[lo - 1].vstart < x->ext[lo - 1].ncl) return &x->ext[lo - 1];
    return 0;
}

static int dir_append(x2f_t *x, uint32_t vstart, uint32_t vres, uint32_t xfirst, uint32_t xlen,
                      uint8_t nofat, uint32_t parent_v, int is_root, uint32_t *idx) {
    x2f_dir *d;
    if (x->nd == x->capd) {
        uint32_t ncap = x->capd ? x->capd * 2 : 256;
        x2f_dir *nd = (x2f_dir *)xalloc(x, ncap * (uint32_t)sizeof(x2f_dir));
        if (!nd) return X2F_ERR_NOMEM;
        if (x->dirs) { mcpy(nd, x->dirs, x->nd * (uint32_t)sizeof(x2f_dir)); xfree(x, x->dirs); }
        x->dirs = nd; x->capd = ncap;
    }
    d = &x->dirs[x->nd];
    mset(d, 0, (uint32_t)sizeof(*d));
    d->vstart = vstart; d->vres = vres; d->xfirst = xfirst; d->xlen = xlen;
    d->nofat = nofat; d->parent_v = parent_v; d->is_root = (uint8_t)is_root;
    *idx = x->nd++;
    return X2F_OK;
}

static int dir_find_v(const x2f_t *x, uint32_t vc) {
    uint32_t lo = 0, hi = x->nd;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (x->dirs[mid].vstart <= vc) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return -1;
    if (vc - x->dirs[lo - 1].vstart < x->dirs[lo - 1].vres) return (int)(lo - 1);
    return -1;
}

/* virtual clusters to reserve for a child directory: FAT needs at most ~1.11x the exFAT entry
 * bytes plus the '.' and '..' entries */
static uint32_t reserve_for(const x2f_t *x, uint32_t xlen) {
    uint32_t cs = (uint32_t)1 << x->cs_shift;
    uint32_t r = (uint32_t)(((uint64_t)xlen + (xlen >> 3) + 128 + cs - 1) >> x->cs_shift);
    return r ? r : 1;
}

/* ---------------- FAT directory entry generation ---------------- */

static uint8_t sfn_char(uint16_t c) {
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 32);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (uint8_t)c;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '(': case ')':
    case '-': case '@': case '^': case '_': case '`': case '{': case '}': case '~':
        return (uint8_t)c;
    default: return '_';
    }
}

static void make_sfn(const uint16_t *n, int len, uint32_t ord, uint8_t out[11]) {
    static const char hex[] = "0123456789ABCDEF";
    int lastdot = -1, i, k = 0;
    for (i = 0; i < len; i++) if (n[i] == '.') lastdot = i;
    mset(out, ' ', 11);
    for (i = 0; i < len && k < 3; i++) {
        if (n[i] == '.' || n[i] == ' ') continue;
        out[k++] = sfn_char(n[i]);
    }
    while (k < 3) out[k++] = '_';
    out[3] = '~';
    out[4] = (uint8_t)hex[(ord >> 12) & 15]; out[5] = (uint8_t)hex[(ord >> 8) & 15];
    out[6] = (uint8_t)hex[(ord >> 4) & 15];  out[7] = (uint8_t)hex[ord & 15];
    if (lastdot > 0) {
        k = 8;
        for (i = lastdot + 1; i < len && k < 11; i++) out[k++] = sfn_char(n[i]);
    }
}

static uint8_t lfn_sum(const uint8_t *s) {
    uint8_t sum = 0; int i;
    for (i = 0; i < 11; i++) sum = (uint8_t)((((sum & 1) ? 0x80 : 0) + (sum >> 1)) + s[i]);
    return sum;
}

/* write LFN entries + SFN entry; returns bytes written */
static uint32_t put_entries(uint8_t *p, const exfat_dirent *e, uint32_t ord, uint32_t first_cluster,
                            uint32_t size, int is_dir) {
    static const uint8_t lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    uint8_t sfn[11], sum;
    uint32_t nl = ((uint32_t)e->name_len + 12) / 13, i, j, w = 0;
    uint32_t cdate = e->ctime >> 16, ctime_ = e->ctime & 0xFFFF;
    uint32_t mdate = e->mtime >> 16, mtime_ = e->mtime & 0xFFFF;
    uint32_t adate = e->atime >> 16;
    if (!cdate) cdate = 0x21;
    if (!mdate) mdate = 0x21;
    if (!adate) adate = 0x21;
    make_sfn(e->name16, e->name_len, ord, sfn);
    sum = lfn_sum(sfn);
    for (i = nl; i >= 1; i--) {
        uint8_t *q = p + w;
        mset(q, 0, 32);
        q[0] = (uint8_t)(i | (i == nl ? 0x40 : 0));
        q[11] = 0x0F; q[13] = sum;
        for (j = 0; j < 13; j++) {
            uint32_t idx = (i - 1) * 13 + j;
            uint32_t ch = idx < e->name_len ? e->name16[idx] : (idx == e->name_len ? 0x0000u : 0xFFFFu);
            put16(q + lfn_pos[j], ch);
        }
        w += 32;
    }
    {
        uint8_t *q = p + w;
        mset(q, 0, 32);
        mcpy(q, sfn, 11);
        q[11] = (uint8_t)((e->node.attr & 0x27) | (is_dir ? 0x10 : 0));
        put16(q + 14, ctime_); put16(q + 16, cdate); put16(q + 18, adate);
        put16(q + 20, first_cluster >> 16);
        put16(q + 22, mtime_); put16(q + 24, mdate);
        put16(q + 26, first_cluster & 0xFFFF);
        put32(q + 28, is_dir ? 0 : size);
        w += 32;
    }
    return w;
}

static void put_dot(uint8_t *q, int dotdot, uint32_t cluster) {
    mset(q, 0, 32);
    mset(q, ' ', 11);
    q[0] = '.'; if (dotdot) q[1] = '.';
    q[11] = 0x10;
    put16(q + 16, 0x21); put16(q + 24, 0x21); put16(q + 18, 0x21);
    put16(q + 20, cluster >> 16); put16(q + 26, cluster & 0xFFFF);
}

/* ---------------- directory generation + cache ---------------- */

static void cache_evict(x2f_t *x, uint32_t need, uint32_t keep) {
    while (x->cache_bytes + need > x->cache_cap) {
        uint32_t i, best = 0xFFFFFFFFu, bi = 0xFFFFFFFFu;
        for (i = 0; i < x->nd; i++)
            if (i != keep && x->dirs[i].buf && x->dirs[i].stamp < best) { best = x->dirs[i].stamp; bi = i; }
        if (bi == 0xFFFFFFFFu) return;
        x->cache_bytes -= x->dirs[bi].vres << x->cs_shift;
        xfree(x, x->dirs[bi].buf);
        x->dirs[bi].buf = 0;
    }
}

static int dir_generate(x2f_t *x, uint32_t di) {
    uint32_t cap, pos = 0, ord = 0, cs = (uint32_t)1 << x->cs_shift;
    uint32_t save_nd = x->nd, save_next = x->next, cd = x->dnext, cf = x->fnext, cj = 0, ej = 0;
    uint8_t *buf;
    int first, r;
    exfat_node *node = &x->t_node;
    exfat_dir *it = &x->t_it;
    exfat_dirent *e = &x->t_e;

    if (x->dirs[di].buf) { x->dirs[di].stamp = ++x->stamp; return X2F_OK; }
    first = !x->dirs[di].generated;
    cap = x->dirs[di].vres << x->cs_shift;
    cache_evict(x, cap, di);
    buf = (uint8_t *)xalloc(x, cap);
    if (!buf) return X2F_ERR_NOMEM;
    mset(buf, 0, cap);
    if (first) { x->dirs[di].first_child = x->nd; x->dirs[di].first_ext = x->next; }

    if (x->dirs[di].is_root) {
        exfat_root(x->v, node);
    } else {
        node->first_cluster = x->dirs[di].xfirst;
        node->size = x->dirs[di].xlen;
        node->valid_size = node->size;
        node->attr = EXFAT_ATTR_DIR;
        node->no_fat_chain = x->dirs[di].nofat;
        put_dot(buf + pos, 0, x->dirs[di].vstart); pos += 32;
        put_dot(buf + pos, 1, x->dirs[di].parent_v == 2 ? 0 : x->dirs[di].parent_v); pos += 32;
    }
    r = exfat_opendir(x->v, node, it);
    if (r) { r = X2F_ERR_IO; goto fail; }
    while ((r = exfat_readdir(it, e)) == 1) {
        int is_dir = (e->node.attr & EXFAT_ATTR_DIR) != 0;
        uint32_t nl = ((uint32_t)e->name_len + 12) / 13, vfirst = 0, size = 0;
        if (!is_dir && e->node.size > 0xFFFFFFFFull) continue;       /* cannot be represented */
        if (pos + 32 * (nl + 1) > cap) { r = X2F_ERR_CORRUPT; goto fail; }
        ord++;
        if (ord > 0xFFFF) { r = X2F_ERR_FULL; goto fail; }
        if (is_dir) {
            uint32_t ci;
            if (first) {
                uint32_t xl = e->node.size > 0x7FFFFFFFull ? 0x7FFFFFFFu : (uint32_t)e->node.size;
                uint32_t vres = reserve_for(x, xl);
                if (cd + vres > x->fstart) { r = X2F_ERR_FULL; goto fail; }
                r = dir_append(x, cd, vres, e->node.first_cluster, xl, e->node.no_fat_chain,
                               x->dirs[di].vstart, 0, &ci);
                if (r) goto fail;
                cd += vres;
            } else {
                ci = x->dirs[di].first_child + cj;
                if (ci >= x->nd) { r = X2F_ERR_CORRUPT; goto fail; }
            }
            cj++;
            vfirst = x->dirs[ci].vstart;
        } else {
            size = (uint32_t)e->node.size;
            if (size > 0 && e->node.first_cluster >= 2) {
                uint32_t ncl = (uint32_t)(((uint64_t)size + cs - 1) >> x->cs_shift), ei;
                uint32_t nreal = (uint32_t)(((uint64_t)size + (((uint32_t)1 << (9 + x->rshift)) - 1)) >> (9 + x->rshift));
                if (first) {
                    if (cf + ncl > x->fend) { r = X2F_ERR_FULL; goto fail; }
                    r = ext_append(x, cf, ncl, e->node.first_cluster, nreal, e->node.no_fat_chain);
                    if (r) goto fail;
                    cf += ncl;
                }
                ei = x->dirs[di].first_ext + ej;
                if (ei >= x->next) { r = X2F_ERR_CORRUPT; goto fail; }
                ej++;
                vfirst = x->ext[ei].vstart;
            }
        }
        pos += put_entries(buf + pos, e, ord, vfirst, size, is_dir);
    }
    if (r < 0) { r = X2F_ERR_IO; goto fail; }
    if (first) { x->dnext = align_up(cd); x->fnext = align_up(cf); }
    x->dirs[di].buf = buf;
    x->dirs[di].vused = pos ? (pos + cs - 1) >> x->cs_shift : 1;
    x->dirs[di].generated = 1;
    x->dirs[di].stamp = ++x->stamp;
    x->cache_bytes += cap;
    x->n_dirgen++;
    return X2F_OK;
fail:
    if (first) { x->nd = save_nd; x->next = save_next; }    /* undo the registrations of this attempt */
    xfree(x, buf);
    return r;
}

/* ---------------- virtual FAT ---------------- */

static int fat_value(const x2f_t *x, uint32_t c, uint32_t *out) {
    *out = 0;
    if (c == 0) { *out = 0x0FFFFFF8u; return X2F_OK; }
    if (c == 1) { *out = FAT_EOC; return X2F_OK; }
    if (c < x->fstart) {
        int di = dir_find_v(x, c);
        if (di >= 0) {
            uint32_t k = c - x->dirs[di].vstart;
            *out = (k + 1 < x->dirs[di].vres) ? c + 1 : FAT_EOC;
        }
    } else {
        const x2f_ext *e = ext_find(x, c);
        if (e) *out = (c == e->vstart + e->ncl - 1) ? FAT_EOC : c + 1;
    }
    return X2F_OK;
}

static int synth_fat_sector(const x2f_t *x, uint32_t idx, uint8_t *out) {
    uint32_t i;
    for (i = 0; i < 128; i++) {
        uint32_t val = 0;
        fat_value(x, idx * 128 + i, &val);
        put32(out + 4 * i, val);
    }
    return X2F_OK;
}

static void synth_bpb(x2f_t *x, uint8_t *s) {
    mset(s, 0, SS);
    s[0] = 0xEB; s[1] = 0x58; s[2] = 0x90;
    mcpy(s + 3, "MSDOS5.0", 8);
    put16(s + 11, SS);
    s[13] = (uint8_t)x->spc;
    put16(s + 14, RESERVED);
    s[16] = 2;
    s[21] = 0xF8;
    put16(s + 24, 63); put16(s + 26, 255);
    put32(s + 32, x->total);
    put32(s + 36, x->F);
    put32(s + 44, 2);                       /* root directory = first virtual cluster */
    put16(s + 48, 1); put16(s + 50, 6);
    s[64] = 0x80; s[66] = 0x29;
    put32(s + 67, x->volid);
    mcpy(s + 71, "NO NAME    ", 11);
    mcpy(s + 82, "FAT32   ", 8);
    s[510] = 0x55; s[511] = 0xAA;
}

static void synth_fsinfo(uint8_t *s) {
    mset(s, 0, SS);
    put32(s + 0, 0x41615252u); put32(s + 484, 0x61417272u);
    put32(s + 488, 0xFFFFFFFFu); put32(s + 492, 0xFFFFFFFFu);
    put32(s + 508, 0xAA550000u);
}

/* chain of REAL clusters of a fragmented file (entries past a broken link stay 0) */
static int ext_chain(x2f_t *x, x2f_ext *e) {
    uint32_t i, c = e->rfirst;
    if (e->chain) return X2F_OK;
    e->chain = (uint32_t *)xalloc(x, e->nreal * 4);
    if (!e->chain) return X2F_ERR_NOMEM;
    mset(e->chain, 0, e->nreal * 4);
    for (i = 0; i < e->nreal; i++) {
        uint32_t raw;
        e->chain[i] = c;
        if (i + 1 == e->nreal) break;
        if (exfat_fat_entry(x->v, c, &raw) || raw < 2 || raw >= x->ccount + 2) break;
        c = raw;
    }
    return X2F_OK;
}

/* ---------------- public API ---------------- */

int x2f_open(x2f_t **out, exfat_vol *v, const x2f_mem *mem, uint32_t cache_bytes) {
    x2f_t *x;
    uint32_t cnt, idx, root_res, budget, vshift, nvc = 0, F = 0, i, dnv;
    int r;
    if (v->sector_size != SS) return X2F_ERR_UNSUPPORTED;
    budget = v->volume_sectors ? v->volume_sectors
                               : v->heap_lba + (v->cluster_count << v->cluster_shift);
    /* virtual cluster size: at most 32 KB, never larger than the real one; shrink until the volume
     * is big enough (>= 65600 clusters) to be recognised as FAT32 */
    vshift = v->cluster_shift < 6 ? v->cluster_shift : 6;
    for (;;) {
        uint64_t vspc = (uint64_t)1 << vshift;
        nvc = (uint32_t)((budget - RESERVED) / vspc);
        for (i = 0; i < 4; i++) {                       /* converge: FAT size depends on cluster count */
            F = ((nvc + 2) * 4 + SS - 1) / SS;
            if ((uint64_t)RESERVED + 2 * (uint64_t)F > budget) { nvc = 0; break; }
            nvc = (uint32_t)((budget - RESERVED - 2 * (uint64_t)F) / vspc);
        }
        F = ((nvc + 2) * 4 + SS - 1) / SS;
        while (nvc > 0 && (uint64_t)RESERVED + 2 * (uint64_t)F + (uint64_t)nvc * vspc > budget) nvc--;
        if (nvc >= 65600u || vshift == 0) break;
        vshift--;
    }
    if (nvc < 65536u || nvc + 2 > 0x0FFFFFF0u) return X2F_ERR_UNSUPPORTED;
    x = (x2f_t *)mem->alloc(mem->ctx, (uint32_t)sizeof(*x));
    if (!x) return X2F_ERR_NOMEM;
    mset(x, 0, (uint32_t)sizeof(*x));
    x->v = v; x->mem = *mem;
    x->rshift = v->cluster_shift;
    x->spc_shift = vshift; x->spc = (uint32_t)1 << vshift; x->cs_shift = 9 + vshift;
    x->ccount = v->cluster_count;
    dnv = nvc / 32;                                    /* directory arena: ~3% of the clusters */
    if (dnv < 2048) dnv = 2048;
    if (dnv > (1u << 20)) dnv = 1u << 20;
    x->fstart = align_up(2 + dnv);
    x->fend = nvc + 2;
    F = (x->fend * 4 + SS - 1) / SS;
    x->F = F;
    x->Sd = RESERVED + 2 * F;
    x->total = x->Sd + nvc * x->spc;
    x->cache_cap = cache_bytes;
    x->volid = 0x58444A31u;
    if (x->fstart + 128 >= x->fend) { mem->release(mem->ctx, x); return X2F_ERR_UNSUPPORTED; }
    if (exfat_count_clusters(v, v->root_cluster, &cnt)) { mem->release(mem->ctx, x); return X2F_ERR_IO; }
    root_res = cnt ? cnt : 1;
    /* the root directory reserves as many VIRTUAL clusters as its real size needs */
    root_res = (uint32_t)(((uint64_t)root_res << (9 + v->cluster_shift)) + (((uint32_t)1 << x->cs_shift) - 1)) >> x->cs_shift;
    if (2 + root_res > x->fstart) { mem->release(mem->ctx, x); return X2F_ERR_UNSUPPORTED; }
    r = dir_append(x, 2, root_res, v->root_cluster, root_res << x->cs_shift, 0, 0, 1, &idx);
    if (r) { x2f_close(x); return r; }
    x->dnext = align_up(2 + root_res);
    x->fnext = x->fstart;
    *out = x;
    return X2F_OK;
}

void x2f_close(x2f_t *x) {
    uint32_t i;
    if (!x) return;
    for (i = 0; i < x->nd; i++) xfree(x, x->dirs[i].buf);
    for (i = 0; i < x->next; i++) xfree(x, x->ext[i].chain);
    xfree(x, x->dirs); xfree(x, x->ext);
    x->mem.release(x->mem.ctx, x);
}

uint32_t x2f_total_sectors(const x2f_t *x) { return x->total; }

/* Writes: say whether virtual sector `lba` is the data of an existing file, and where the same bytes live on the real volume.
 * Returns the number of consecutive virtual sectors (1..n) that map to consecutive real sectors starting at *real_out
 * (volume-relative, like x2f_read uses), or 0 if the sector is NOT file data (reserved sectors, the FAT, directories,
 * free space, or the unused tail of a virtual cluster past the file's real allocation). Nothing is allocated or changed. */
uint32_t x2f_total(const x2f_t *x) { return x->total; }

uint32_t x2f_data_map(x2f_t *x, uint32_t lba, uint32_t n, uint32_t *real_out) {
    uint32_t rel, vc, off, fsec, ralloc, left, run;
    x2f_ext *e;
    if (!n || lba < x->Sd || (uint64_t)lba + n > x->total) return 0;
    rel = lba - x->Sd; vc = (rel >> x->spc_shift) + 2; off = rel & (x->spc - 1);
    if (vc < x->fstart) return 0;
    e = ext_find(x, vc);
    if (!e) return 0;
    fsec = ((vc - e->vstart) << x->spc_shift) + off;
    ralloc = e->nreal << x->rshift;
    left = ((e->vstart + e->ncl - vc) << x->spc_shift) - off;
    if (fsec >= ralloc) return 0;
    if (left > ralloc - fsec) left = ralloc - fsec;
    run = n < left ? n : left;
    if (e->nofat) {
        *real_out = x->v->heap_lba + ((e->rfirst - 2) << x->rshift) + fsec;
    } else {
        uint32_t rc, insec = fsec & (((uint32_t)1 << x->rshift) - 1);
        if (ext_chain(x, e)) return 0;
        rc = e->chain[fsec >> x->rshift];
        if (rc < 2) return 0;
        if (run > ((uint32_t)1 << x->rshift) - insec) run = ((uint32_t)1 << x->rshift) - insec;
        *real_out = x->v->heap_lba + ((rc - 2) << x->rshift) + insec;
    }
    return run;
}

int x2f_read(x2f_t *x, uint32_t lba, uint32_t n, void *vbuf) {
    uint8_t *buf = (uint8_t *)vbuf;
    if ((uint64_t)lba + n > x->total) return X2F_ERR_RANGE;
    while (n) {
        if (lba < RESERVED) {
            if (lba == 0 || lba == 6) synth_bpb(x, buf);
            else if (lba == 1 || lba == 7) synth_fsinfo(buf);
            else mset(buf, 0, SS);
            lba++; n--; buf += SS;
        } else if (lba < RESERVED + 2 * x->F) {
            uint32_t idx = lba - RESERVED;
            if (idx >= x->F) idx -= x->F;
            synth_fat_sector(x, idx, buf);
            x->n_fat++;
            lba++; n--; buf += SS;
        } else {
            uint32_t rel = lba - x->Sd, vc = (rel >> x->spc_shift) + 2, off = rel & (x->spc - 1);
            if (vc < x->fstart) {                            /* directory cluster */
                int di = dir_find_v(x, vc);
                mset(buf, 0, SS);
                if (di >= 0) {
                    int r = dir_generate(x, (uint32_t)di);
                    if (r) return r;
                    mcpy(buf, x->dirs[di].buf + ((vc - x->dirs[di].vstart) << x->cs_shift) + off * SS, SS);
                }
                lba++; n--; buf += SS;
            } else {                                         /* file data */
                x2f_ext *e = ext_find(x, vc);
                uint32_t fsec, ralloc, left, run, real;
                if (!e) { mset(buf, 0, SS); lba++; n--; buf += SS; continue; }
                fsec = ((vc - e->vstart) << x->spc_shift) + off;           /* sector offset inside the file */
                ralloc = e->nreal << x->rshift;                            /* real sectors the file occupies */
                left = ((e->vstart + e->ncl - vc) << x->spc_shift) - off;  /* sectors left in the virtual extent */
                if (fsec >= ralloc) { mset(buf, 0, SS); lba++; n--; buf += SS; continue; }
                if (left > ralloc - fsec) left = ralloc - fsec;
                run = n < left ? n : left;
                if (e->nofat) {
                    real = x->v->heap_lba + ((e->rfirst - 2) << x->rshift) + fsec;
                } else {
                    uint32_t rc, insec = fsec & (((uint32_t)1 << x->rshift) - 1);
                    int r = ext_chain(x, e);
                    if (r) return r;
                    rc = e->chain[fsec >> x->rshift];
                    if (rc < 2) return X2F_ERR_CORRUPT;
                    if (run > ((uint32_t)1 << x->rshift) - insec) run = ((uint32_t)1 << x->rshift) - insec;
                    real = x->v->heap_lba + ((rc - 2) << x->rshift) + insec;
                }
                if (exfat_read_sectors(x->v, real, run, buf)) return X2F_ERR_IO;
                x->n_data++;
                lba += run; n -= run; buf += run * SS;
            }
        }
    }
    return X2F_OK;
}

void x2f_stats(const x2f_t *x, uint32_t *dirs, uint32_t *data, uint32_t *fat) {
    *dirs = x->n_dirgen; *data = x->n_data; *fat = x->n_fat;
}

int x2f_prefetch_all(x2f_t *x) {
    uint32_t i;
    for (i = 0; i < x->nd; i++) {
        int r = dir_generate(x, i);
        if (r) return r;
    }
    return X2F_OK;
}
