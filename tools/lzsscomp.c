/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
/*
 * lzsscomp - LZSS compressor for the XDJ-1000MK2 MAIN stream format.
 *
 * Output: 4-byte little-endian compressed length, then the stream.
 *   flag byte, LSB first: 1 = literal byte, 0 = 2-byte reference
 *   reference: b1 = pos & 0xFF, b2 = ((pos >> 4) & 0xF0) | (len - 3)
 *              pos = absolute position in a 4096-byte ring, len 3..18
 *   ring[0..4077] filled with 0x20 (ring[4078..4095] are NOT initialised on the device, so
 *   they are never referenced), first output byte goes to ring[0xFEE]
 *   unused bits of the last flag byte are 0 (as in the original stream)
 *
 * Uses optimal parsing (9 bits per literal, 17 per reference) over the longest match
 * found at each position, so the result is normally no larger than the original encoder's.
 *
 * Build:  gcc -O2 -o lzsscomp lzsscomp.c
 * Usage:  lzsscomp INPUT OUTPUT [chain_limit]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define PRE 4096
#define WIN 4095
#define MINM 3
#define MAXM 18
#define MINSRC (PRE - 4078)  /* device only fills ring[0..4077] with 0x20 */
#define HBITS 22

static uint32_t hash3(const uint8_t *p) {
    uint32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
    return (v * 2654435761u) >> (32 - HBITS);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s INPUT OUTPUT [chain_limit]\n", argv[0]); return 2; }
    int chain_limit = argc > 3 ? atoi(argv[3]) : 256;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(PRE + n + 4);
    if (!buf) { fprintf(stderr, "out of memory\n"); return 1; }
    memset(buf, 0x20, PRE);
    if (fread(buf + PRE, 1, n, f) != (size_t)n) { fprintf(stderr, "read error\n"); return 1; }
    fclose(f);
    memset(buf + PRE + n, 0, 4);

    int32_t *head = malloc(sizeof(int32_t) << HBITS);
    int32_t *prev = malloc(sizeof(int32_t) * (PRE + n));
    uint8_t *mlen = calloc(n + 1, 1);
    uint16_t *mdist = calloc(n + 1, sizeof(uint16_t));
    if (!head || !prev || !mlen || !mdist) { fprintf(stderr, "out of memory\n"); return 1; }
    for (uint32_t i = 0; i < (1u << HBITS); i++) head[i] = -1;

    /* the 0x20 prefix is part of the window: index it too */
    for (long p = 0; p + 2 < PRE; p++) {
        uint32_t h = hash3(buf + p);
        prev[p] = head[h]; head[h] = p;
    }
    for (long i = 0; i < n; i++) {
        long p = PRE + i;
        int maxl = (n - i < MAXM) ? (int)(n - i) : MAXM;
        int bestl = 0; int bestd = 0;
        if (maxl >= MINM) {
            uint32_t h = hash3(buf + p);
            int32_t c = head[h]; int tries = chain_limit;
            while (c >= MINSRC && p - c <= WIN && tries--) {
                int l = 0;
                while (l < maxl && buf[c + l] == buf[p + l]) l++;
                if (l > bestl) { bestl = l; bestd = (int)(p - c); if (l == maxl) break; }
                c = prev[c];
            }
            prev[p] = head[h]; head[h] = p;
        } else {
            prev[p] = -1;
        }
        if (bestl >= MINM) { mlen[i] = bestl; mdist[i] = bestd; }
        if ((i & 0xFFFFF) == 0) { fprintf(stderr, "\rmatching %ld%%", i * 100 / n); }
    }
    fprintf(stderr, "\rmatching done   \n");
    free(head); free(prev);

    /* backward DP for the cheapest parse */
    uint32_t *best = malloc(sizeof(uint32_t) * (n + 1));
    uint8_t *ch = malloc(n + 1);
    if (!best || !ch) { fprintf(stderr, "out of memory\n"); return 1; }
    best[n] = 0;
    for (long i = n - 1; i >= 0; i--) {
        uint32_t b = 9 + best[i + 1]; uint8_t c = 0;
        for (int l = MINM; l <= mlen[i]; l++) {
            uint32_t t = 17 + best[i + l];
            if (t < b) { b = t; c = l; }
        }
        best[i] = b; ch[i] = c;
    }
    free(best);

    /* emit */
    uint8_t *out = malloc((size_t)n + n / 8 + 64);
    size_t o = 4, fpos = 0; int nb = 8; uint8_t flag = 0;
    for (long i = 0; i < n;) {
        if (nb == 8) {
            if (o > 4) out[fpos] = flag;
            fpos = o++; flag = 0; nb = 0;
        }
        if (ch[i] == 0) {
            flag |= 1 << nb; out[o++] = buf[PRE + i]; i++;
        } else {
            int l = ch[i], d = mdist[i];
            int r = (0xFEE + i) & 0xFFF;
            int pos = (r - d) & 0xFFF;
            out[o++] = pos & 0xFF;
            out[o++] = ((pos >> 4) & 0xF0) | (l - 3);
            i += l;
        }
        nb++;
    }
    out[fpos] = flag;
    uint32_t clen = (uint32_t)(o - 4);
    out[0] = clen & 0xFF; out[1] = (clen >> 8) & 0xFF; out[2] = (clen >> 16) & 0xFF; out[3] = clen >> 24;
    f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    fwrite(out, 1, o, f); fclose(f);
    fprintf(stderr, "%ld bytes -> %u compressed bytes (%.2f%%)\n", n, clen, clen * 100.0 / n);
    return 0;
}
