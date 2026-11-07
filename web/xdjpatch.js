/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
/* xdjpatch.js - patch an XDJ-1000MK2 firmware update (.UPD) entirely in the browser.
 * Unpacks the MAIN section, decompresses it, applies a patch spec (checked byte-for-byte), recompresses with the same
 * LZSS format the player expects, rebuilds the S-record container with correct checksums, and verifies its own output.
 * Works in browsers, web workers and Node (>= 18). No network access. */
(function (root) {
  'use strict';

  const NAME_LEN = 32;
  const CRC = (function () {
    const t = new Uint16Array(256);
    for (let i = 0; i < 256; i++) { let c = i << 8; for (let k = 0; k < 8; k++) c = (c & 0x8000) ? (((c << 1) ^ 0x1021) & 0xFFFF) : ((c << 1) & 0xFFFF); t[i] = c; }
    return t;
  })();
  function crc16(b, s, e) { let c = 0; for (let i = s; i < e; i++) c = ((c << 8) & 0xFFFF) ^ CRC[((c >> 8) ^ b[i]) & 0xFF]; return c; }
  function fail(msg) { const e = new Error(msg); e.userMessage = msg; throw e; }
  const ascii = (b, s, e) => { let r = ''; for (let i = s; i < e; i++) r += String.fromCharCode(b[i]); return r; };
  const hexv = (c) => (c <= 57 ? c - 48 : (c | 32) - 87);
  const HEX = '0123456789ABCDEF';
  function b64d(s) { const bin = atob(s); const o = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) o[i] = bin.charCodeAt(i); return o; }
  function sha256js(m) {                                   /* fallback when crypto.subtle is unavailable (non-secure contexts) */
    const K = [], H = []; let q = 2, cnt = 0;
    while (cnt < 64) { let pr = true; for (let d = 2; d * d <= q; d++) if (q % d === 0) { pr = false; break; }
      if (pr) { if (cnt < 8) H[cnt] = (Math.pow(q, 1 / 2) % 1) * 4294967296 | 0; K[cnt] = (Math.pow(q, 1 / 3) % 1) * 4294967296 | 0; cnt++; } q++; }
    const l = m.length, total = ((l + 9 + 63) >> 6) << 6, buf = new Uint8Array(total); buf.set(m); buf[l] = 0x80;
    const dv = new DataView(buf.buffer); dv.setUint32(total - 8, Math.floor(l / 536870912)); dv.setUint32(total - 4, (l << 3) >>> 0);
    const w = new Int32Array(64); let [a0, b0, c0, d0, e0, f0, g0, h0] = H;
    for (let o = 0; o < total; o += 64) {
      for (let i = 0; i < 16; i++) w[i] = dv.getInt32(o + 4 * i);
      for (let i = 16; i < 64; i++) { const x = w[i - 15], y = w[i - 2];
        w[i] = (w[i - 16] + (((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3)) + w[i - 7] + (((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10))) | 0; }
      let a = a0, b = b0, c = c0, d = d0, e = e0, f = f0, g = g0, h = h0;
      for (let i = 0; i < 64; i++) {
        const t1 = (h + (((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7))) + ((e & f) ^ (~e & g)) + K[i] + w[i]) | 0;
        const t2 = ((((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10))) + ((a & b) ^ (a & c) ^ (b & c))) | 0;
        h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
      }
      a0 = (a0 + a) | 0; b0 = (b0 + b) | 0; c0 = (c0 + c) | 0; d0 = (d0 + d) | 0; e0 = (e0 + e) | 0; f0 = (f0 + f) | 0; g0 = (g0 + g) | 0; h0 = (h0 + h) | 0;
    }
    return [a0, b0, c0, d0, e0, f0, g0, h0].map((x) => (x >>> 0).toString(16).padStart(8, '0')).join('');
  }
  async function sha256hex(bytes) {
    if (typeof crypto !== 'undefined' && crypto.subtle && crypto.subtle.digest) {
      const d = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
      let r = ''; for (const x of d) r += x.toString(16).padStart(2, '0'); return r;
    }
    return sha256js(bytes);
  }
  const tick = () => new Promise((r) => setTimeout(r, 0));

  /* ------------------------------------------------------------------ container */
  function findCRLF(b, from) { for (let i = from; i < b.length - 1; i++) if (b[i] === 13 && b[i + 1] === 10) return i; return -1; }
  function parseUpd(b) {
    const i = findCRLF(b, 0), j = i >= 0 ? findCRLF(b, i + 2) : -1;
    if (i < 0 || j < 0) fail('This does not look like an XDJ update file (no length header).');
    const n1 = parseInt(ascii(b, 0, i), 10), n2 = parseInt(ascii(b, i + 2, j), 10);
    const start = j + 2;
    if (!(n1 > 0 && n2 > 0) || start + n1 + n2 !== b.length) fail('This does not look like an XDJ update file (section lengths do not match the file size).');
    return { main: b.subarray(start, start + n1), panl: b.subarray(start + n1) };
  }

  function parseSection(sec) {
    if (sec.length < NAME_LEN + 4) fail('Update file section too short.');
    const stored = sec[sec.length - 2] | (sec[sec.length - 1] << 8);
    if (crc16(sec, 0, sec.length - 2) !== stored) fail('The update file is damaged (section checksum mismatch). Download it again.');
    const end = sec.length - 2;
    if (sec[end - 2] !== 13 || sec[end - 1] !== 10) fail('Update file section is not in the expected format.');
    const name32 = sec.slice(0, NAME_LEN);
    const head = [], tail = [], recA = [], recN = [], recPos = [];
    let state = 0, pos = NAME_LEN;
    const bodyEnd = end - 2;                      /* body minus the final CRLF */
    while (pos <= bodyEnd) {
      let le = pos; while (le < bodyEnd && !(sec[le] === 13 && sec[le + 1] === 10)) le++;
      if (sec[pos] !== 0x53) fail('Update file contains an unexpected record.');
      const type = sec[pos + 1];
      if (type === 0x32) {
        if (state === 2) fail('Update file records are out of order.');
        state = 1;
        const cnt = (hexv(sec[pos + 2]) << 4) | hexv(sec[pos + 3]);
        let sum = 0; for (let k = 0; k < cnt; k++) sum += (hexv(sec[pos + 2 + 2 * k]) << 4) | hexv(sec[pos + 3 + 2 * k]);
        const chk = (hexv(sec[pos + 2 + 2 * cnt]) << 4) | hexv(sec[pos + 3 + 2 * cnt]);
        if (((~sum) & 0xFF) !== chk || (le - pos) !== 2 * cnt + 4) fail('Update file has a bad record checksum.');
        recA.push((((hexv(sec[pos + 4]) << 4) | hexv(sec[pos + 5])) << 16) | (((hexv(sec[pos + 6]) << 4) | hexv(sec[pos + 7])) << 8) | ((hexv(sec[pos + 8]) << 4) | hexv(sec[pos + 9])));
        recN.push(cnt - 4); recPos.push(pos + 10);
      } else {
        const line = sec.slice(pos, le);
        if (state === 0) head.push(line); else { tail.push(line); state = 2; }
      }
      pos = le + 2;
    }
    /* segments (same grouping as the reference tool, so record boundaries are reproduced exactly) */
    const segs = []; let prevEnd = -1;
    for (let r = 0; r < recA.length; r++) {
      const a = recA[r], n = recN[r];
      if (n <= 0 || a < prevEnd) fail('Update file records are out of order.');
      prevEnd = a + n;
      if (segs.length) { const s = segs[segs.length - 1]; if (a === s[0] + s[2] && s[2] % s[1] === 0 && n <= s[1]) { s[2] += n; continue; } }
      segs.push([a, n, n]);
    }
    const base = recA[0]; let top = base;
    for (let r = 0; r < recA.length; r++) top = Math.max(top, recA[r] + recN[r]);
    const img = new Uint8Array(top - base).fill(0xFF);
    for (let r = 0; r < recA.length; r++) {
      let p = recPos[r], o = recA[r] - base;
      for (let k = 0; k < recN[r]; k++, p += 2) img[o + k] = (hexv(sec[p]) << 4) | hexv(sec[p + 1]);
    }
    return { name32, head, tail, segs, base, img };
  }

  function buildSection(s) {
    let size = NAME_LEN + 2;
    for (const l of s.head) size += l.length + 2;
    for (const l of s.tail) size += l.length + 2;
    const lines = [];
    for (const [a, sz, total] of s.segs) for (let off = 0; off < total; off += sz) { const n = Math.min(sz, total - off); lines.push([a + off, n]); size += 2 + 2 * (n + 5) + 2; }
    const out = new Uint8Array(size);
    out.set(s.name32, 0);
    let o = NAME_LEN;
    const crlf = () => { out[o++] = 13; out[o++] = 10; };
    for (const l of s.head) { out.set(l, o); o += l.length; crlf(); }
    for (const [a, n] of lines) {
      const off = a - s.base;
      if (off < 0 || off + n > s.img.length) fail('Internal error: record outside the image.');
      out[o++] = 0x53; out[o++] = 0x32;
      const put = (v) => { out[o++] = HEX.charCodeAt(v >> 4); out[o++] = HEX.charCodeAt(v & 15); };
      let sum = n + 4; put(n + 4);
      const b0 = (a >> 16) & 255, b1 = (a >> 8) & 255, b2 = a & 255; sum += b0 + b1 + b2; put(b0); put(b1); put(b2);
      for (let k = 0; k < n; k++) { const v = s.img[off + k]; sum += v; put(v); }
      put((~sum) & 255); crlf();
    }
    for (const l of s.tail) { out.set(l, o); o += l.length; crlf(); }
    if (o !== size - 2) fail('Internal error: section size mismatch.');
    const c = crc16(out, 0, o); out[o++] = c & 255; out[o++] = c >> 8;
    return out;
  }

  /* ------------------------------------------------------------------ LZSS (the MAIN payload format) */
  function lzssDecode(data, start, end, outSize) {
    const ring = new Uint8Array(4096).fill(0x20);
    const out = new Uint8Array(outSize);
    let r = 0xFEE, i = start, o = 0;
    while (i < end) {
      const fl = data[i++];
      for (let bit = 0; bit < 8; bit++) {
        if (i >= end) return o === outSize ? out : out.subarray(0, o);
        if ((fl >> bit) & 1) {
          const b = data[i++]; if (o >= outSize) fail('Corrupt compressed data.');
          out[o++] = b; ring[r] = b; r = (r + 1) & 0xFFF;
        } else {
          if (i + 1 >= end) return o === outSize ? out : out.subarray(0, o);
          const b1 = data[i], b2 = data[i + 1]; i += 2;
          const p = b1 | ((b2 & 0xF0) << 4), len = (b2 & 0x0F) + 3;
          if (o + len > outSize) fail('Corrupt compressed data.');
          for (let k = 0; k < len; k++) { const b = ring[(p + k) & 0xFFF]; out[o++] = b; ring[r] = b; r = (r + 1) & 0xFFF; }
        }
      }
    }
    return o === outSize ? out : out.subarray(0, o);
  }

  async function lzssCompress(src, chainLimit, progress) {
    const PRE = 4096, WIN = 4095, MINM = 3, MAXM = 18, MINSRC = PRE - 4078, HBITS = 22;
    const n = src.length;
    const buf = new Uint8Array(PRE + n + 4); buf.fill(0x20, 0, PRE); buf.set(src, PRE);
    const head = new Int32Array(1 << HBITS).fill(-1);
    const prev = new Int32Array(4096);                    /* ring: only positions within the 4095-byte window are ever followed */
    const mlen = new Uint8Array(n + 1), mdist = new Uint16Array(n + 1);
    const K = 0x9E3779B1 | 0, SH = 32 - HBITS;
    for (let p = 0; p + 2 < PRE; p++) {
      const h = Math.imul(buf[p] | (buf[p + 1] << 8) | (buf[p + 2] << 16), K) >>> SH;
      prev[p & 4095] = head[h]; head[h] = p;
    }
    for (let i = 0; i < n; i++) {
      const p = PRE + i, maxl = (n - i < MAXM) ? n - i : MAXM;
      let bestl = 0, bestd = 0;
      if (maxl >= MINM) {
        const h = Math.imul(buf[p] | (buf[p + 1] << 8) | (buf[p + 2] << 16), K) >>> SH;
        let c = head[h], tries = chainLimit;
        while (c >= MINSRC && p - c <= WIN && tries--) {
          let l = 0; while (l < maxl && buf[c + l] === buf[p + l]) l++;
          if (l > bestl) { bestl = l; bestd = p - c; if (l === maxl) break; }
          c = prev[c & 4095];
        }
        prev[p & 4095] = head[h]; head[h] = p;
      } else prev[p & 4095] = -1;
      if (bestl >= MINM) { mlen[i] = bestl; mdist[i] = bestd; }
      if ((i & 0x3FFFF) === 0x3FFFF && progress) { progress(0.7 * i / n); await tick(); }
    }
    /* cheapest parse, backwards (9 bits per literal, 17 per reference); the choice is stored in place of the match length */
    const win = new Uint32Array(32); win[n & 31] = 0;
    for (let i = n - 1; i >= 0; i--) {
      let b = 9 + win[(i + 1) & 31], c = 0; const ml = mlen[i];
      for (let l = MINM; l <= ml; l++) { const t = 17 + win[(i + l) & 31]; if (t < b) { b = t; c = l; } }
      win[i & 31] = b; mlen[i] = c;
      if ((i & 0x7FFFFF) === 0 && progress) { progress(0.7 + 0.1 * (1 - i / n)); }
    }
    const out = new Uint8Array(n + (n >> 3) + 64);
    let o = 4, fpos = 0, nb = 8, flag = 0;
    for (let i = 0; i < n;) {
      if (nb === 8) { if (o > 4) out[fpos] = flag; fpos = o++; flag = 0; nb = 0; }
      const ch = mlen[i];
      if (ch === 0) { flag |= 1 << nb; out[o++] = buf[PRE + i]; i++; }
      else {
        const r = (0xFEE + i) & 0xFFF, pos = (r - mdist[i]) & 0xFFF;
        out[o++] = pos & 0xFF; out[o++] = ((pos >> 4) & 0xF0) | (ch - 3); i += ch;
      }
      nb++;
    }
    out[fpos] = flag;
    const clen = o - 4;
    out[0] = clen & 255; out[1] = (clen >> 8) & 255; out[2] = (clen >> 16) & 255; out[3] = (clen >>> 24) & 255;
    if (progress) progress(0.8);
    return out.subarray(0, o);
  }

  /* ------------------------------------------------------------------ patching */
  function equal(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
  function opMatches(unp, o) { const d = b64d(o.d); return equal(unp.subarray(o.o, o.o + d.length), d); }
  function applyOps(unp, ops, what) {
    for (const o of ops) {
      const d = b64d(o.d), cur = unp.subarray(o.o, o.o + d.length);
      if (o.ff) { for (let i = 0; i < cur.length; i++) if (cur[i] !== 0xFF) fail('Cannot patch: the free space this build needs is already used in your file (' + what + '). Use the stock 1.45 file.'); }
      else if (o.e && !equal(cur, b64d(o.e))) fail('Cannot patch: your file differs from the version this patcher was made for (' + what + ' at 0x' + (0xA8000000 + o.o).toString(16).toUpperCase() + ').');
      unp.set(d, o.o);
    }
  }

  /* spec: the parsed patchspec.json; opts: { djpatcher: bool, hires: bool }; progress(fraction, text) */
  async function patchFirmware(input, spec, opts, progress) {
    opts = opts || {}; const say = (f, t) => { if (progress) progress(f, t); };
    say(0.01, 'Reading the update file…'); await tick();
    const inputSha = await sha256hex(input);
    const parsed = parseUpd(input);
    const main = parseSection(parsed.main), SO = spec.streamOff, img = main.img;
    const n = img[SO] | (img[SO + 1] << 8) | (img[SO + 2] << 16) | (img[SO + 3] << 24 >>> 0);
    const sEnd = SO + 4 + n;
    if (n <= 0 || sEnd + 2 > img.length) fail('This is not the XDJ-1000MK2 firmware this patcher expects.');
    let sum = 0; for (let i = SO; i < sEnd; i++) sum += img[i];
    if ((sum & 0xFFFF) !== (img[sEnd] | (img[sEnd + 1] << 8))) fail('The update file is damaged (stream checksum mismatch). Download it again.');
    say(0.04, 'Unpacking the firmware…'); await tick();
    const unp = new Uint8Array(lzssDecode(img, SO + 4, sEnd, spec.unpackedSize));
    if (unp.length !== spec.unpackedSize) fail('This is not the XDJ-1000MK2 1.45 firmware (unpacked size differs).');

    const G = spec.groups, isStock = inputSha === spec.stockSha256;
    let layout, kind;
    if (isStock) {
      kind = 'stock 1.45';
      if (opts.djpatcher) { applyOps(unp, G.djpatcher, 'extra features'); layout = 'layer_djp'; } else layout = 'layer_stock';
    } else {
      if (!G.djpatcher.every((o) => opMatches(unp, o))) fail('This file is not the original XDJ-1000MK2 1.45 update, and it is not a firmware this patcher knows. Use the unmodified 1.45 file from Pioneer DJ.');
      kind = 'firmware with the extra features'; layout = 'layer_djp';
    }
    say(0.08, 'Applying the changes…'); await tick();
    applyOps(unp, G[layout], 'exFAT support');
    if (opts.hires) applyOps(unp, G.hires, 'hi-res audio');
    applyOps(unp, [{ o: spec.verOff, d: btoa(spec.verTo), e: btoa(spec.verFrom) }], 'version');
    const nm = main.name32, oldN = 'Ver' + spec.verFrom, newN = 'Ver' + spec.verTo;
    let at = -1; const nmS = ascii(nm, 0, nm.length); at = nmS.indexOf(oldN);
    if (at < 0 || nmS.indexOf(oldN, at + 1) >= 0) fail('Unexpected update file name field.');
    for (let i = 0; i < newN.length; i++) nm[at + i] = newN.charCodeAt(i);

    say(0.1, 'Compressing (this takes a minute)…'); await tick();
    const stream = await lzssCompress(unp, 256, (f) => say(0.1 + 0.8 * f, 'Compressing (this takes a minute)…'));
    if (stream.length + 2 > img.length - SO) fail('The patched firmware does not fit in the update file.');
    say(0.92, 'Checking the result…'); await tick();
    const check = lzssDecode(stream, 4, stream.length, spec.unpackedSize);
    if (!equal(check, unp)) fail('Internal error: the compressed data did not verify. Nothing was produced.');
    let ssum = 0; for (let i = 0; i < stream.length; i++) ssum += stream[i];
    img.set(stream, SO); img[SO + stream.length] = ssum & 255; img[SO + stream.length + 1] = (ssum >> 8) & 255;
    img.fill(0xFF, SO + stream.length + 2);
    const mainOut = buildSection(main);
    const hdr = new TextEncoder().encode(mainOut.length + '\r\n' + parsed.panl.length + '\r\n');
    const out = new Uint8Array(hdr.length + mainOut.length + parsed.panl.length);
    out.set(hdr, 0); out.set(mainOut, hdr.length); out.set(parsed.panl, hdr.length + mainOut.length);
    /* re-read what we are about to hand over */
    const again = parseSection(parseUpd(out).main);
    const n2 = again.img[SO] | (again.img[SO + 1] << 8) | (again.img[SO + 2] << 16) | (again.img[SO + 3] << 24 >>> 0);
    if (!equal(lzssDecode(again.img, SO + 4, SO + 4 + n2, spec.unpackedSize), unp)) fail('Internal error: the output did not verify. Nothing was produced.');
    say(1, 'Done.');
    return { bytes: out, layout, kind, inputSha, outputSha: await sha256hex(out) };
  }

  root.XDJPatch = { patchFirmware, sha256hex, sha256js, parseUpd, parseSection, buildSection, lzssDecode, lzssCompress };
  if (typeof module !== 'undefined' && module.exports) module.exports = root.XDJPatch;
})(typeof globalThis !== 'undefined' ? globalThis : this);
