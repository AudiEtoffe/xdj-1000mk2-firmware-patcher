# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
# See the LICENSE file in the top directory.
"""A minimal lazy FAT32 reader that behaves like an embedded driver with a sector cache:
FAT sectors are cached forever and never invalidated. Used to test the x2f layer."""
import struct

class LazyFAT:
    def __init__(self, dev):
        self.dev = dev                    # dev(lba, count) -> bytes (partition-relative)
        self.cache = {}                   # FAT sector cache, never invalidated
        b = dev(0, 1)
        assert b[510:512] == b"\x55\xAA" and b[82:90] == b"FAT32   ", "not a FAT32 boot sector"
        self.bps, self.spc, self.rsv, self.nfat = struct.unpack_from("<HBHB", b, 11)
        self.fatsz = struct.unpack_from("<I", b, 36)[0]
        self.root = struct.unpack_from("<I", b, 44)[0]
        self.data0 = self.rsv + self.nfat * self.fatsz
        assert self.bps == 512

    def fat_get(self, c):
        sec = self.rsv + (c * 4) // 512
        if sec not in self.cache:
            self.cache[sec] = self.dev(sec, 1)
        return struct.unpack_from("<I", self.cache[sec], (c * 4) % 512)[0] & 0x0FFFFFFF

    def chain(self, c):
        out = []
        while c < 0x0FFFFFF8:
            if c < 2: raise IOError("free/reserved cluster %d inside a chain" % c)
            out.append(c)
            if len(out) > 50_000_000: raise IOError("chain loop")
            c = self.fat_get(c)
        return out

    def read_cluster(self, c):
        return self.dev(self.data0 + (c - 2) * self.spc, self.spc)

    def read_dir(self, first):
        data = b"".join(self.read_cluster(c) for c in self.chain(first))
        entries, lfn = [], []
        for i in range(0, len(data), 32):
            e = data[i:i + 32]
            if e[0] == 0x00: break
            if e[0] == 0xE5: lfn = []; continue
            attr = e[11]
            if attr == 0x0F:
                lfn.append((e[0] & 0x3F, e[1:11] + e[14:26] + e[28:32], e[13]))
                continue
            if attr & 0x08: lfn = []; continue
            sfn = e[:11]
            name = None
            if lfn:
                csum = 0
                for ch in sfn: csum = (((csum & 1) << 7) + (csum >> 1) + ch) & 0xFF
                if all(c == csum for _, _, c in lfn):
                    raw = b"".join(p for _, p, _ in sorted(lfn))
                    u = struct.unpack("<%dH" % (len(raw) // 2), raw)
                    out = []
                    for v in u:
                        if v in (0, 0xFFFF): break
                        out.append(v)
                    name = struct.pack("<%dH" % len(out), *out).decode("utf-16-le")
            if name is None:
                name = sfn[:8].decode("latin-1").rstrip() + ("." + sfn[8:].decode("latin-1").rstrip() if sfn[8:].strip() else "")
            lfn = []
            first_c = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
            entries.append((name, attr, first_c, struct.unpack_from("<I", e, 28)[0]))
        return entries

    def walk(self, first=None, prefix=""):
        first = self.root if first is None else first
        for name, attr, c, size in self.read_dir(first):
            if name in (".", ".."): continue
            p = prefix + "/" + name
            if attr & 0x10:
                yield (p, "D", 0, c)
                yield from self.walk(c, p)
            else:
                yield (p, "F", size, c)

    def read_file(self, first, size):
        if size == 0: return b""
        data = b"".join(self.read_cluster(c) for c in self.chain(first))
        return data[:size]
