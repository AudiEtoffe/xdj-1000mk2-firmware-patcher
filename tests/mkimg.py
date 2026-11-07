#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
# See the LICENSE file in the top directory.
"""
Build a populated exFAT test image: mkfs.exfat makes the empty volume, this script adds
directories and files by writing the on-disk structures directly (no mounting needed).
Independent check: run `fsck.exfat -n` on the result. Writes <img>.json (expected tree).
"""
import hashlib
import json
import random
import struct
import subprocess
import sys

EOC = 0xFFFFFFFF


def utf16_units(name):
    b = name.encode("utf-16-le")
    return list(struct.unpack("<%dH" % (len(b) // 2), b))


def upcase_unit(u):
    c = chr(u)
    up = c.upper()
    if len(up) == 1 and ord(up) < 0x10000 and not (0xD800 <= u <= 0xDFFF):
        return ord(up)
    return u


def name_hash(units):
    h = 0
    for u in units:
        c = upcase_unit(u)
        for b in (c & 0xFF, c >> 8):
            h = ((((h & 1) << 15) | (h >> 1)) + b) & 0xFFFF
    return h


def set_checksum(entries):
    s = 0
    for ei, e in enumerate(entries):
        for i in range(32):
            if ei == 0 and i in (2, 3):
                continue
            s = ((((s & 1) << 15) | (s >> 1)) + e[i]) & 0xFFFF
    return s


class Vol:
    def __init__(self, path):
        self.f = open(path, "r+b")
        b = self.rd(0, 512)
        assert b[3:11] == b"EXFAT   "
        self.ss = 1 << b[108]
        self.spc = 1 << b[109]
        self.fat_off, self.fat_len, self.heap_off, self.ccount, self.root = struct.unpack_from("<IIIII", b, 80)
        self.csz = self.ss * self.spc
        # find the allocation bitmap in the root directory
        bm = None
        for off in self.slots(self.root):
            e = self.rd(off, 32)
            if e[0] == 0x81:
                bm = (struct.unpack_from("<I", e, 20)[0], struct.unpack_from("<Q", e, 24)[0])
            if e[0] == 0x00:
                break
        assert bm, "no bitmap entry"
        self.bm_first, self.bm_len = bm
        self.bitmap = bytearray(self.rd(self.coff(self.bm_first), self.bm_len))

    def rd(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def wr(self, off, data):
        self.f.seek(off)
        self.f.write(data)

    def coff(self, c):
        return (self.heap_off + (c - 2) * self.spc) * self.ss

    def fat_get(self, c):
        return struct.unpack("<I", self.rd(self.fat_off * self.ss + 4 * c, 4))[0]

    def fat_set(self, c, v):
        self.wr(self.fat_off * self.ss + 4 * c, struct.pack("<I", v))

    def chain(self, first):
        out, c = [], first
        while c != EOC and c >= 2:
            out.append(c)
            c = self.fat_get(c)
        return out

    def slots(self, first):
        res = []
        for c in self.chain(first):
            base = self.coff(c)
            res += [base + i * 32 for i in range(self.csz // 32)]
        return res

    # --- allocation ---
    def is_used(self, c):
        i = c - 2
        return bool(self.bitmap[i >> 3] & (1 << (i & 7)))

    def mark(self, c):
        i = c - 2
        self.bitmap[i >> 3] |= 1 << (i & 7)

    def alloc(self, n, contiguous=True):
        got = []
        c = 2
        if contiguous:
            run = []
            while c < self.ccount + 2:
                if not self.is_used(c):
                    run.append(c)
                    if len(run) == n:
                        break
                else:
                    run = []
                c += 1
            assert len(run) == n, "volume full"
            got = run
        else:
            while c < self.ccount + 2 and len(got) < n:
                if not self.is_used(c) and (not got or c > got[-1] + 1):
                    got.append(c)
                c += 1
            assert len(got) == n, "volume full"
        for c in got:
            self.mark(c)
        return got

    def flush_bitmap(self):
        self.wr(self.coff(self.bm_first), bytes(self.bitmap))

    def zero_cluster(self, c):
        self.wr(self.coff(c), b"\0" * self.csz)

    # --- entries ---
    @staticmethod
    def entry_set(name, is_dir, first, size, valid, nofat):
        units = utf16_units(name)
        assert 1 <= len(units) <= 255
        nn = (len(units) + 14) // 15
        e0 = bytearray(32)
        e0[0] = 0x85
        e0[1] = 1 + nn
        struct.pack_into("<H", e0, 4, 0x10 if is_dir else 0x20)
        ts = (44 << 25) | (6 << 21) | (15 << 16) | (12 << 11)
        struct.pack_into("<III", e0, 8, ts, ts, ts)
        e1 = bytearray(32)
        e1[0] = 0xC0
        e1[1] = 0x01 | (0x02 if nofat else 0)
        e1[3] = len(units)
        struct.pack_into("<H", e1, 4, name_hash(units))
        struct.pack_into("<Q", e1, 8, valid)
        struct.pack_into("<I", e1, 20, first)
        struct.pack_into("<Q", e1, 24, size)
        ents = [e0, e1]
        for i in range(nn):
            e = bytearray(32)
            e[0] = 0xC1
            chunk = units[i * 15:(i + 1) * 15]
            for k, u in enumerate(chunk):
                struct.pack_into("<H", e, 2 + 2 * k, u)
            ents.append(e)
        struct.pack_into("<H", e0, 2, set_checksum(ents))
        return [bytes(e) for e in ents]

    def add_to_dir(self, d, ents):
        """Append entries to directory d (dict: first, entry_slots); extends the chain if full."""
        slots = self.slots(d["first"])
        idx = len(slots)
        for i, o in enumerate(slots):
            if self.rd(o, 1)[0] == 0x00:
                idx = i
                break
        while idx + len(ents) > len(slots):
            new = self.alloc(1, contiguous=False)[0]
            self.zero_cluster(new)
            last = self.chain(d["first"])[-1]
            self.fat_set(last, new)
            self.fat_set(new, EOC)
            self.flush_bitmap()
            slots = self.slots(d["first"])
            if d.get("entry_slots"):
                self.update_dir_size(d, len(self.chain(d["first"])) * self.csz)
        used = slots[idx:idx + len(ents)]
        for o, e in zip(used, ents):
            self.wr(o, e)
        return used

    def update_dir_size(self, d, size):
        es = [bytearray(self.rd(o, 32)) for o in d["entry_slots"]]
        struct.pack_into("<Q", es[1], 8, size)
        struct.pack_into("<Q", es[1], 24, size)
        es[1][1] = 0x01
        struct.pack_into("<H", es[0], 2, set_checksum(es))
        for o, e in zip(d["entry_slots"], es):
            self.wr(o, bytes(e))

    def mkdir(self, parent, name):
        c = self.alloc(1, contiguous=True)[0]
        self.zero_cluster(c)
        self.fat_set(c, EOC)
        self.flush_bitmap()
        slots = self.add_to_dir(parent, self.entry_set(name, True, c, self.csz, self.csz, False))
        return {"first": c, "entry_slots": slots}

    def mkfile(self, parent, name, data, mode="contig", valid=None):
        n = (len(data) + self.csz - 1) // self.csz
        valid = len(data) if valid is None else valid
        if n == 0:
            first, nofat = 0, False
            clusters = []
        elif mode == "contig":
            clusters = self.alloc(n, contiguous=True)
            first, nofat = clusters[0], True
        else:
            clusters = self.alloc(n, contiguous=False)
            first, nofat = clusters[0], False
            for i, c in enumerate(clusters):
                self.fat_set(c, clusters[i + 1] if i + 1 < len(clusters) else EOC)
        for i, c in enumerate(clusters):
            chunk = data[i * self.csz:(i + 1) * self.csz][:max(0, valid - i * self.csz)]
            self.wr(self.coff(c), chunk.ljust(self.csz, b"\0"))
        self.flush_bitmap()
        self.add_to_dir(parent, self.entry_set(name, False, first, len(data), valid, nofat))


def main(path, size_mb=256, cluster=4096, seed=1, nmany=2500):
    rnd = random.Random(seed)
    subprocess.check_call(["truncate", "-s", "%dM" % size_mb, path])
    subprocess.check_call(["mkfs.exfat", "-c", str(cluster), "-L", "XDJTEST", path],
                          stdout=subprocess.DEVNULL)
    v = Vol(path)
    root = {"first": v.root, "entry_slots": None}
    tree = []  # expected: dicts with kind, path, size, sha256 (of bytes a reader must return)

    def rec_dir(p):
        tree.append({"kind": "D", "path": p, "size": 0})

    def rec_file(p, data, valid=None):
        exp = data if valid is None else data[:valid] + b"\0" * (len(data) - valid)
        tree.append({"kind": "F", "path": p, "size": len(data), "sha256": hashlib.sha256(exp).hexdigest()})

    def blob(n):
        return bytes(rnd.getrandbits(8) for _ in range(n)) if n < 4096 else rnd.randbytes(n)

    def mk(parent, ppath, name):
        d = v.mkdir(parent, name)
        rec_dir(ppath + "/" + name)
        return d, ppath + "/" + name

    # rekordbox-like layout
    pioneer, pp = mk(root, "", "PIONEER")
    rb, rbp = mk(pioneer, pp, "rekordbox")
    data = blob(300_000)
    v.mkfile(rb, "export.pdb", data, "contig"); rec_file(rbp + "/export.pdb", data)
    data = blob(5_000)
    v.mkfile(rb, "exportExt.pdb", data, "frag"); rec_file(rbp + "/exportExt.pdb", data)
    anlz, ap = mk(pioneer, pp, "USBANLZ")
    for i in range(6):
        d1, p1 = mk(anlz, ap, "P%03X" % i)
        d2, p2 = mk(d1, p1, "%08X" % rnd.getrandbits(32))
        for nm in ("ANLZ0000.DAT", "ANLZ0000.EXT"):
            data = blob(rnd.randint(1, 20000))
            v.mkfile(d2, nm, data, rnd.choice(["contig", "frag"])); rec_file(p2 + "/" + nm, data)

    contents, cp = mk(root, "", "Contents")
    d1, p1 = mk(contents, cp, "Artist Name")
    d2, p2 = mk(d1, p1, "Album Name With A Long Title")
    long_name = "01 - A track name that is considerably longer than fifteen characters (Extended Mix).mp3"
    data = blob(3_000_000)
    v.mkfile(d2, long_name, data, "contig"); rec_file(p2 + "/" + long_name, data)
    data = blob(2_100_001)
    v.mkfile(d2, "02 fragmented.flac", data, "frag"); rec_file(p2 + "/02 fragmented.flac", data)
    data = b""
    v.mkfile(d2, "empty.txt", data, "contig"); rec_file(p2 + "/empty.txt", data)
    data = blob(100_000)
    v.mkfile(d2, "partial valid.wav", data, "contig", valid=40_000); rec_file(p2 + "/partial valid.wav", data, 40_000)
    # exact cluster-multiple and one-byte-over sizes
    for n in (cluster, cluster * 3, cluster * 3 + 1, 1):
        data = blob(n)
        nm = "size_%d.bin" % n
        v.mkfile(d2, nm, data, "contig"); rec_file(p2 + "/" + nm, data)
    # unicode names
    for dirname, fname in (("Mötley Crüe", "01 Take Me to the Top.flac"), ("日本語アーティスト", "曲 01.mp3"),
                           ("Привет", "Файл.mp3"), ("emoji 🎧 dir", "track 🎵.mp3")):
        dd, dp = mk(contents, cp, dirname)
        data = blob(rnd.randint(1000, 50000))
        v.mkfile(dd, fname, data, rnd.choice(["contig", "frag"])); rec_file(dp + "/" + fname, data)
    # a very large directory: forces a multi-cluster, fragmented directory chain
    many, mp = mk(contents, cp, "many")
    for i in range(nmany):
        data = blob(rnd.randint(1, 1500))
        nm = "track_%04d.mp3" % i
        v.mkfile(many, nm, data, "contig"); rec_file(mp + "/" + nm, data)
    # name of maximum length
    longest = ("x" * 250) + ".mp3"
    data = blob(777)
    v.mkfile(contents, longest, data, "contig"); rec_file(cp + "/" + longest, data)

    v.flush_bitmap()
    v.f.close()
    json.dump(tree, open(path + ".json", "w"), ensure_ascii=False, indent=0)
    print("built %s: %d entries" % (path, len(tree)))


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 256,
         int(sys.argv[3]) if len(sys.argv) > 3 else 4096, 1,
         int(sys.argv[4]) if len(sys.argv) > 4 else 2500)
