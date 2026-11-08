# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
# See the LICENSE file in the top directory.
import ctypes, hashlib, io, json, os, struct, subprocess, sys
import pyfatfs.PyFat as P
from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS
lib = ctypes.CDLL("./libxdjtest.so")
lib.xdj_test_cmd.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_void_p, ctypes.c_uint]
lib.xdj_test_orig_calls.restype = ctypes.c_ulong
lib.xdj_test_get_ver.restype = ctypes.c_char_p
def ver(): return lib.xdj_test_get_ver()[:4].decode()

def cmd(idx, c, lba, count):
    buf = ctypes.create_string_buffer(512 * max(count, 1))
    r = lib.xdj_test_cmd(idx, c, lba, buf, count)
    return r, buf.raw[:512 * count]

class VDisk(io.RawIOBase):
    def __init__(self, idx):
        self.idx, self.pos = idx, 0
        r, b = cmd(idx, 1, 0, 1)
        assert r == 0
        self.size = struct.unpack_from("<I", b, 32)[0] * 512
    def seekable(self): return True
    def readable(self): return True
    def tell(self): return self.pos
    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.size + off
        return self.pos
    def read(self, n=-1):
        if n < 0 or self.pos + n > self.size: n = max(0, self.size - self.pos)
        out = b""; end = self.pos + n
        while self.pos < end:
            s = self.pos // 512; e = min((end + 511) // 512, s + 2048)
            r, data = cmd(self.idx, 1, s, e - s)
            if r != 0: raise IOError("virtual read failed at sector %d" % s)
            chunk = data[self.pos - s * 512: min(end, e * 512) - s * 512]
            out += chunk; self.pos += len(chunk)
        return out
    def readinto(self, b):
        d = self.read(len(b)); b[:len(d)] = d; return len(d)
    def write(self, *_): raise IOError("read-only")
    def close(self): pass

def devfn(idx):
    def rd(lba, count):
        r, d = cmd(idx, 1, lba, count)
        if r != 0: raise IOError("read failed at lba %d" % lba)
        return d
    return rd

def run_lazy(image, part_start, manifest):
    """Player-style access: lazy walk with a never-invalidated FAT sector cache."""
    from lazyfat import LazyFAT
    assert lib.xdj_test_open(image.encode(), part_start) == 0
    r, _ = cmd(0, 5, 0, 0); assert r == 0                      # attach: init command
    assert ver()[2:] == 'I0', ver()
    fat = LazyFAT(devfn(0))
    r, b0 = cmd(0, 1, 0, 1)
    import os
    total = struct.unpack_from('<I', b0, 32)[0]; spc = b0[13]
    real_sectors = (os.path.getsize(image) // 512) - part_start
    assert total <= real_sectors, 'virtual volume (%d sectors) is larger than the real one (%d)' % (total, real_sectors)
    exp = json.load(open(manifest)); want = {e["path"]: e for e in exp}
    got = {}; bad = 0; checked = 0
    for p, kind, size, c in fat.walk():
        got[p] = (kind, size)
        if kind == "F" and p in want:
            data = fat.read_file(c, size); checked += 1
            if len(data) != want[p]["size"] or hashlib.sha256(data).hexdigest() != want[p]["sha256"]: bad += 1
    missing = set(want) - set(got); extra = set(got) - set(want)
    v = ver()
    assert v[2] == 'D', 'diagnostic status after a full read should be D, got %r' % v
    print("  lazy driver, partition start %d: entries %d/%d (missing %d, extra %d); files sha256-checked %d, mismatches %d; virtual cluster %d KB, FAT sectors cached %d"
          % (part_start, len(got), len(want), len(missing), len(extra), checked, bad, spc // 2, len(fat.cache)))
    return not (missing or extra or bad)

def run_pyfatfs(image, part_start, manifest):
    """Independent parser (pyfatfs, preloads the whole FAT) after a test-only prefetch."""
    assert lib.xdj_test_open(image.encode(), part_start) == 0
    r, _ = cmd(0, 5, 0, 0); assert r == 0
    VDisk(0); assert lib.xdj_test_prefetch(0) == 0
    P.open = lambda *a, **k: VDisk(0)
    exp = json.load(open(manifest)); want = {e["path"]: e for e in exp}
    fs = PyFat(); fs.open("virtual", read_only=True); ffs = PyFatFS("virtual", read_only=True)
    out = {}
    def listing(d, prefix):
        dirs, files, _ = d.get_entries()
        for sd in dirs:
            n = sd.get_long_name()
            if n in (".", ".."): continue
            out[prefix + "/" + n] = ("D", 0); listing(sd, prefix + "/" + n)
        for f in files: out[prefix + "/" + f.get_long_name()] = ("F", f.filesize)
    listing(fs.root_dir, "")
    missing = set(want) - set(out); extra = set(out) - set(want)
    bad = checked = 0
    for p, e in want.items():
        if e["kind"] != "F" or p not in out: continue
        with ffs.openbin(p, "r") as fh: data = fh.read()
        checked += 1
        if len(data) != e["size"] or hashlib.sha256(data).hexdigest() != e["sha256"]: bad += 1
    print("  pyfatfs after prefetch, partition start %d: entries %d/%d (missing %d, extra %d); files checked %d, mismatches %d"
          % (part_start, len(out), len(want), len(missing), len(extra), checked, bad))
    return not (missing or extra or bad)

lib.xdj_test_prefetch.argtypes = [ctypes.c_int]

ok = True
# 1. lazy, player-style access (no prefetch), partition at sector 0 and at an offset
ok &= run_lazy("t4k.img", 0, "t4k.img.json")
subprocess.check_call("dd if=/dev/zero of=off.img bs=512 count=2048 status=none && cat t4k.img >> off.img", shell=True)
ok &= run_lazy("off.img", 2048, "t4k.img.json")
ok &= run_lazy("t32k.img", 0, "t32k.img.json")
ok &= run_lazy("t128k.img", 0, "t128k.img.json")
ok &= run_lazy("t256k.img", 0, "t256k.img.json")
ok &= run_pyfatfs("t4k.img", 0, "t4k.img.json")
# 2. a non-exFAT volume must pass through untouched
subprocess.check_call("rm -f fat.img && truncate -s 64M fat.img && mkfs.fat -F32 fat.img >/dev/null", shell=True)
assert lib.xdj_test_open(b"fat.img", 0) == 0
cmd(1, 5, 0, 0)
raw = open("fat.img", "rb").read()
same = True
for lba in (0, 1, 2, 32, 1000, 5000):
    r, d = cmd(1, 1, lba, 1)
    same &= (r == 0 and d == raw[lba * 512:(lba + 1) * 512])
print("  FAT32 volume passes through unchanged:", same, "| status text", repr(ver())); ok &= same and ver()[2] == "P"
# 3. writes to exFAT never reach the device unless they land in file data (stage B; see test_write.py), other commands/unknown disks are forwarded
assert lib.xdj_test_open(b"t4k.img", 0) == 0
cmd(2, 5, 0, 0); cmd(2, 1, 0, 1)
w0 = lib.xdj_test_orig_calls(2)
r, _ = cmd(2, 2, 100, 1); print("  metadata write to exFAT accepted, kept off the device:", r == 0 and lib.xdj_test_orig_calls(2) == w0); ok &= (r == 0 and lib.xdj_test_orig_calls(2) == w0)
r, _ = cmd(2, 7, 0, 0); print("  other commands forwarded:", r == 0); ok &= (r == 0)
r, _ = cmd(9, 1, 0, 1); print("  unknown disk pointer forwarded:", r == 0); ok &= (r == 0)
print("ALL OK" if ok else "FAILED"); sys.exit(0 if ok else 1)
