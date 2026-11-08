# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
# See the LICENSE file in the top directory.
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
"""Stage B (save in place): what reaches the real image when the player writes to the virtual FAT32 view?"""
import ctypes, hashlib, json, os, shutil, struct, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lazyfat import LazyFAT
lib = ctypes.CDLL("./libxdjtest.so")
lib.xdj_test_cmd.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_void_p, ctypes.c_uint]
lib.xdj_test_open_rw.argtypes = [ctypes.c_char_p, ctypes.c_uint]
lib.xdj_test_orig_calls.restype = ctypes.c_ulong
ok = True
def chk(name, good, extra=""):
    global ok; ok &= bool(good); print(("PASS " if good else "FAIL ") + name + (" " + str(extra) if extra and not good else ""))
def rd(lba, n):
    b = ctypes.create_string_buffer(512 * n); r = lib.xdj_test_cmd(0, 1, lba, b, n); return r, b.raw
def wr(lba, data):
    b = ctypes.create_string_buffer(data, len(data)); return lib.xdj_test_cmd(0, 2, lba, b, len(data) // 512)
def dev(lba, n):
    r, b = rd(lba, n); assert r == 0; return b
def sector_diffs(a_path, b_path):
    out = []; off = 0
    with open(a_path, 'rb') as fa, open(b_path, 'rb') as fb:
        while True:
            a = fa.read(1 << 22); b = fb.read(1 << 22)
            if not a: break
            if a != b:
                for i in range(0, len(a), 512):
                    if a[i:i + 512] != b[i:i + 512]: out.append(off + i)
            off += 1 << 22
    return out

def fsck(work, start):
    if start:
        subprocess.run(['dd', 'if=' + work, 'of=/tmp/part.img', 'bs=1M', 'skip=%d' % (start * 512 // (1 << 20)), 'status=none'], check=True); work = '/tmp/part.img'
    r = subprocess.run(['fsck.exfat', '-n', work], capture_output=True, text=True); out = (r.stdout + r.stderr)
    return ('clean' in out.lower()) and r.returncode == 0, 'rc=%d %s' % (r.returncode, out[-300:])

def run(img, start):
    work = "/tmp/w_" + img
    shutil.copyfile(img, work); orig = open(img, "rb")
    lib.xdj_test_open_rw(work.encode(), start); lib.xdj_test_cmd(0, 5, 0, None, 0)
    fs = LazyFAT(dev)
    files = [(p, sz, c) for p, t, sz, c in fs.walk() if t == "F" and sz >= 20000]
    path, size, c0 = files[0]; spc = fs.spc
    print("--", img, "start", start, "| test file", path, size, "bytes, virtual cluster", c0, "spc", spc)
    lba0 = fs.data0 + (c0 - 2) * spc
    before = fs.read_file(c0, size)
    # T1: overwrite the first 2 sectors of the file's data
    pat = bytes((i * 7 + 3) & 255 for i in range(1024))
    chk("in-place write accepted", wr(lba0, pat) == 0)
    _, got = rd(lba0, 2); chk("read back through the layer returns the new data", got == pat)
    # T2: metadata writes go to the overlay only
    fatlba = fs.rsv + 5; dirlba = fs.data0 + (fs.root - 2) * spc; free_vc = max(max(fs.chain(cc)) for _, t_, _, cc in fs.walk() if t_ in 'FD' and cc >= 2) + 50
    junk = bytes([0xEE]) * 512
    for name, l in (("FAT sector", fatlba), ("FSInfo sector", 1), ("root directory sector", dirlba), ("unallocated (free) cluster", fs.data0 + (free_vc - 2) * spc)):
        chk("write to %s accepted" % name, wr(l, junk) == 0)
        _, g = rd(l, 1); chk("  ... and reads back from the RAM overlay", g == junk)
    lib.xdj_test_cmd(0, 5, 0, None, 0)                                   # new attach: overlay must be gone
    fs2 = LazyFAT(dev); tree2 = {p_: (sz_, c_) for p_, t_, sz_, c_ in fs2.walk() if t_ == "F"}   # the player lists directories before reading files
    _, g = rd(1, 1); chk("after re-attach the FSInfo sector is the synthesized one again", g != junk)
    size2, c2 = tree2[path]; after = fs2.read_file(c2, size2)
    chk("after re-attach the file shows the in-place change (persisted)", after[:1024] == pat and after[1024:] == before[1024:])
    # raw image: ONLY the file's own bytes changed
    diffs = sector_diffs(img, work)
    chk("real image: exactly the sectors inside the file changed (%d sector(s))" % len(diffs), 1 <= len(diffs) <= 2, diffs[:4])
    good, msg = fsck(work, start); chk("fsck.exfat reports the stick clean after the writes", good, msg)
    # other files untouched
    names = {p: (s_, c) for p, t, s_, c in fs2.walk() if t == "F"}
    # T3: overlay capacity: more than OVL_CAP distinct metadata sectors -> clean refusal, image unchanged
    res = [wr(fs.rsv + 100 + i, junk) for i in range(60)]
    chk("overlay full: first writes succeed, later ones fail cleanly (no crash)", res[0] == 0 and res.count(0) < 60 and res[-1] != 0, res[:3] + res[-3:])
    chk("write beyond the end of the volume refused", wr(0x7FFFFFF0, junk) != 0)
    # T4: a write spanning file data and the unallocated space after its last virtual cluster
    lib.xdj_test_cmd(0, 5, 0, None, 0); fs3 = LazyFAT(dev); t3 = {p_: (sz_, c_) for p_, t_, sz_, c_ in fs3.walk() if t_ == "F"}
    lastcl = max(fs3.chain(c_)[-1] for p_, (sz_, c_) in t3.items() if sz_ > 0)       # the file that ends last in the layout: the next cluster is free
    last_lba = fs3.data0 + (lastcl - 2) * spc + spc - 2
    span = bytes([0x5A]) * (512 * 4)
    chk("write spanning end of file data into free space accepted", wr(last_lba, span) == 0)
    st = os.stat(work); same = True
    for off in (0,):
        pass
    d2 = sector_diffs(img, work)
    chk("spanning write: only the 2 in-file sectors reached the stick (total changed sectors %d)" % len(d2), len(d2) == len(diffs) + 2, d2[:6])
    good, msg = fsck(work, start); chk("stick still clean after the spanning write", good, msg)
    os.remove(work)
run("t4k.img", 0)
run("dev_mbr.img", 2048)
print("ALL OK" if ok else "FAILURES"); sys.exit(0 if ok else 1)
