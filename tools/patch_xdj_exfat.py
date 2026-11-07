#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
# See the LICENSE file in the top directory.
"""
patch_xdj_exfat.py - build an XDJ-1000MK2 v1.45 update with the exFAT layer installed.

    python3 patch_xdj_exfat.py XDJ1KMK2.UPD xdj_exfat.bin OUT/XDJ1KMK2.UPD [--entry 0x08119EB8]

What it does to the decompressed MAIN image (addresses are RAM addresses, base 0xA8000000):
  1. writes the wrapper/exFAT/FAT32-layer blob into the 0xFF padding at 0xA8119E80..0xA8120000
  2. changes ONE word, the disk I/O callback address that attach_drive passes to ini_disk_sect
     (file offset 0x1280D60, 0x09281694 -> wrapper entry)
then recompresses the image, rewrites the byte-sum checksum the stage-1 loader checks, and rebuilds
the .UPD (S-record checksums, section CRC-16 and lengths). It refuses to run if anything it expects
(firmware version, original bytes, free cave) is not exactly as analysed.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xdjtool as xt

CAVE_LO, CAVE_HI = 0x119E80, 0x120000         # offsets into the decompressed image
LITERAL_OFF = 0x1280D60                       # word holding the I/O callback address
ORIG_CALLBACK = 0x09281694
EXPECT_SIZE = 29275684
STREAM_OFF = 0x40000


# ---- hi-res audio patch: accept 88.2 and 96 kHz in the FLAC and WAV validators, as the CDJ-2000NXS2 (1.87) does.
# Offsets are into the decompressed MAIN image (RAM address 0xA8000000 + offset). Each site is checked against the
# exact original words before anything is written.
def _w(*ws):
    return b"".join(struct.pack("<H", x) for x in ws)

HIRES = [
    # FLAC info check (function at 0xA938632C). After 44100/48000 fail: try 96000 (= 2 x 48000, r6) and 88200
    # (= 2 x 44100, reloaded into r6). The sample-rate error message then shares the bit-depth error's tail
    # (identical code), which frees the room.
    ("FLAC 88.2/96 kHz (0xA9386462)", 0x1386462,
     _w(0x2F26, 0xD134, 0xD32D, 0x430B, 0x2F16, 0x7F08, 0xED54, 0x3DEC, 0xD52B, 0xE404, 0xE2FF, 0x2D52,
        0x84DE, 0x6603, 0xE00F, 0x2409, 0x4408, 0x6063, 0x4408, 0xC90F, 0x204B, 0xA089, 0x80DE),
     _w(0x4600,            # 462 shll r6          r6 = 96000
        0x3260,            # 464 cmp/eq r6,r2
        0x8913,            # 466 bt 0xA9386490    (accepted)
        0xD62D,            # 468 mov.l @(0xA9386520),r6   r6 = 44100
        0x4600,            # 46A shll r6          r6 = 88200
        0x3260,            # 46C cmp/eq r6,r2
        0x890F,            # 46E bt 0xA9386490    (accepted)
        0x2F26,            # 470 mov.l r2,@-r15   error: log message
        0xD131,            # 472 mov.l @(0xA9386538),r1
        0xD329,            # 474 mov.l @(0xA938651C),r3
        0x430B,            # 476 jsr @r3
        0x2F16,            # 478 mov.l r1,@-r15
        0x7F08,            # 47A add #8,r15
        0xA014,            # 47C bra 0xA93864A8   shared error tail (same code as the old inline tail)
        0x0009,            # 47E nop
        *([0x0009] * 8))), # 480-48E unreachable filler
    # WAV header check (function at 0xA9386770), non-DISC branch only (the DISC branch stays 44.1/48 like the NXS2)
    ("WAV 88.2/96 kHz (0xA9386D36)", 0x1386D36,
     _w(0xE04C, 0x1FB7, 0x0FE6, 0x2F26, 0xD235, 0xD333, 0x430B, 0x2F26, 0x7F08, 0xE050, 0xE2FE, 0xA45C, 0x0F26),
     _w(0x4A00,            # D36 shll r10         r10 = 96000 (r10 = 48000 here)
        0x32A0,            # D38 cmp/eq r10,r2
        0x8909,            # D3A bt 0xA9386D50    (accepted)
        0xDA32,            # D3C mov.l @(0xA9386E08),r10  r10 = 44100
        0x4A00,            # D3E shll r10         r10 = 88200
        0x32A0,            # D40 cmp/eq r10,r2
        0x8905,            # D42 bt 0xA9386D50    (accepted)
        0xAFE5,            # D44 bra 0xA9386D12   existing sample-rate error path
        0x0009,            # D46 nop
        *([0x0009] * 4))), # D48-D4E unreachable filler
]


# ---- exFAT stick with no partition table (the usual state of a factory-formatted USB stick reformatted in Windows).
# fs_get_partition (0xA93A4A8C) treats a 0x55AA sector whose OEM name (offset 3) is "MSDOS", "MSWIN4" or "BSD  4.4" as a
# whole-disk volume (no partition table) and parses anything else as an MBR, so an "EXFAT   " boot sector is rejected before
# the I/O callback is ever installed. The comparison string for "BSD  4.4" is an 8-byte slot with its own pointer; "EXFAT   "
# is also exactly 8 bytes, so it takes that slot's place. (Side effect: a whole-disk "BSD  4.4" volume - rare, raw newfs_msdos -
# is no longer recognised; those on macOS normally have an MBR and are unaffected.)
SUPERFLOPPY = ("exFAT without partition table (string at 0xA809B9E0)", 0x9B9E0, b"BSD  4.4", b"EXFAT   ")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("upd"); ap.add_argument("manifest", help="manifest.json from build_fragments.py (or a single .bin with --entry)"); ap.add_argument("out")
    ap.add_argument("--entry", default=None, help="address of xdj_io (single .bin mode)")
    ap.add_argument("--marker", action="store_true", help="recolor the hot-cue A letter yellow (visible sign that this build is running)")
    ap.add_argument("--no-superfloppy", action="store_true", help="do not accept exFAT volumes without a partition table")
    ap.add_argument("--hires", action="store_true", help="also accept 88.2 and 96 kHz in the FLAC and WAV validators (as the CDJ-2000NXS2 does)")
    ap.add_argument("--version", default=None, help="new version text, e.g. 1.46 (replaces 1.45 in the image header and the update file name field)")
    a = ap.parse_args()
    import json
    if a.manifest.endswith(".json"):
        man = json.load(open(a.manifest))
        base = os.path.dirname(os.path.abspath(a.manifest))
        frags = [(int(f["addr"], 0) - 0x08000000, open(os.path.join(base, f["file"]), "rb").read()) for f in man["fragments"]]
        entry = int(man["entry"], 0)
    else:
        frags = [(CAVE_LO, open(a.manifest, "rb").read())]
        entry = int(a.entry, 0)
    data = open(a.upd, "rb").read()

    secs = xt.parse_upd(data)
    m = secs["main"]
    img = m["image"]
    n = int.from_bytes(img[STREAM_OFF:STREAM_OFF + 4], "little")
    end = STREAM_OFF + 4 + n
    r = xt.check_stream(img, STREAM_OFF)
    if not r or r[1] != r[2]:
        sys.exit("refusing: MAIN stream checksum is not valid in the input")
    unp = bytearray(xt.lzss_decode(img, STREAM_OFF + 4, end))

    # ---- sanity checks: this must be exactly the firmware the patch was analysed on
    if len(unp) != EXPECT_SIZE:
        sys.exit("refusing: unpacked MAIN is %d bytes, expected %d (different firmware version?)" % (len(unp), EXPECT_SIZE))
    if b"XDJ-1000MK2" not in bytes(unp[0x600:0x800]) or b"1.45" not in bytes(unp[0x600:0x800]):
        sys.exit("refusing: version string not found at 0x700 (expected XDJ-1000MK2 1.45)")
    if struct.unpack_from("<I", unp, LITERAL_OFF)[0] != ORIG_CALLBACK:
        sys.exit("refusing: literal at 0x%X is not 0x%08X" % (LITERAL_OFF, ORIG_CALLBACK))
    for off, fb in frags:
        if off < CAVE_LO or off + len(fb) > CAVE_HI:
            sys.exit("refusing: fragment at 0x%X (+%d) is outside the padding area" % (off, len(fb)))
        if any(b != 0xFF for b in unp[off:off + len(fb)]):
            sys.exit("refusing: target of the fragment at 0x%X is not all 0xFF in this image (already used?)" % off)
    if not any(off <= entry - 0x08000000 < off + len(fb) for off, fb in frags):
        sys.exit("refusing: entry 0x%08X is not inside any fragment" % entry)

    if a.version:
        if len(a.version) != 4:
            sys.exit("refusing: --version must be 4 characters like 1.46")
        vpos = bytes(unp).find(b"1.45", 0x700, 0x800)
        if vpos < 0 or bytes(unp).count(b"1.45", 0x700, 0x800) != 1:
            sys.exit("refusing: version text 1.45 not found exactly once in the image header")
        unp[vpos:vpos + 4] = a.version.encode()
        nf = bytes(m["name32"])
        if nf.count(b"Ver1.45") != 1:
            sys.exit("refusing: 'Ver1.45' not found in the update file name field")
        m["name32"] = nf.replace(b"Ver1.45", b"Ver" + a.version.encode())
        print("  version text changed 1.45 -> %s (image header at 0xA8%06X and the update file's name field)" % (a.version, vpos))
    if a.marker:
        import collections
        base, W, r0, r1, x0, x1 = 0x1000000, 152, 596, 670, 50, 114
        def getpx(r, x): return struct.unpack_from("<H", unp, base + (r * W + x) * 2)[0]
        cnt = collections.Counter(getpx(r, x) for r in range(r0, r1 + 1) for x in range(x0, x1 + 1))
        bg = cnt.most_common(1)[0][0]
        white = cnt.get(0xFFFF, 0)
        if not (0x0300 <= bg <= 0x0420) or not (800 < white < 2500):
            sys.exit("refusing: the hot-cue A sprite does not look as expected (bg 0x%04X, white %d)" % (bg, white))
        br, bgc, bb = (bg >> 11) & 31, (bg >> 5) & 63, bg & 31
        changed = 0
        for r in range(r0, r1 + 1):
            for x in range(x0, x1 + 1):
                p = getpx(r, x)
                if p in (bg, 0xF81F):
                    continue
                pr = (p >> 11) & 31
                al = max(0.0, min(1.0, (pr - br) / float(31 - br))) if br < 31 else 0.0
                nr = round(31 * al); ng = round(bgc + al * (63 - bgc)); nb = round(bb * (1 - al))
                struct.pack_into("<H", unp, base + (r * W + x) * 2, (nr << 11) | (ng << 5) | nb)
                changed += 1
        print("  marker: recolored %d pixels of the hot-cue A letter to yellow (background 0x%04X)" % (changed, bg))
    if not a.no_superfloppy:
        name, off, old, new = SUPERFLOPPY
        if bytes(unp[off:off + len(old)]) != old:
            sys.exit("refusing: %s: the original bytes are not what the patch was written for" % name)
        unp[off:off + len(new)] = new
        print("  superfloppy: %s: replaced" % name)
    if a.hires:
        for name, off, old, new in HIRES:
            if bytes(unp[off:off + len(old)]) != old:
                sys.exit("refusing: %s: the original bytes are not what the patch was written for" % name)
            assert len(new) == len(old)
            unp[off:off + len(new)] = new
            print("  hires: %s: %d bytes replaced" % (name, len(new)))
    orig_sha = hashlib.sha256(unp).hexdigest()
    for off, fb in frags:
        unp[off:off + len(fb)] = fb
    struct.pack_into("<I", unp, LITERAL_OFF, entry)
    if "fgp" in man:                  # hook the attach routine's fs_get_partition call (literal 0x093A4A8C at 0xA9280D40)
        if struct.unpack_from("<I", unp, 0x1280D40)[0] != 0x093A4A8C:
            sys.exit("refusing: literal at 0x1280D40 is not fs_get_partition")
        struct.pack_into("<I", unp, 0x1280D40, int(man["fgp"], 0))
        print("  fs_get_partition hook: 0x093A4A8C -> %s" % man["fgp"])
    if "pf" in man:                   # redirect the print-function literal inside the USB mass-storage code to the breadcrumb hook
        n = 0
        for off in range(0x127E000, 0x1285000, 4):
            if struct.unpack_from("<I", unp, off)[0] == 0x0926B064:
                struct.pack_into("<I", unp, off, int(man["pf"], 0)); n += 1
        print("  print hook: %d literal(s) in the USB code redirected to %s" % (n, man["pf"]))
        if n == 0: sys.exit("refusing: no print literals found")
    if "ident" in man:                # hook the media-identify routine's call to the FAT classifier (literal 0x093A52D2 at 0xA9281708)
        if struct.unpack_from("<I", unp, 0x1281708)[0] != 0x093A52D2:
            sys.exit("refusing: literal at 0x1281708 is not the filesystem classifier")
        struct.pack_into("<I", unp, 0x1281708, int(man["ident"], 0))
        print("  identify hook: 0x093A52D2 -> %s" % man["ident"])
    for key, off, orig in (("ident2", 0x1281714, 0x093A503C), ("ident3", 0x128171C, 0x093A5008)):
        if key in man:                # the identify routine's second and third classifier calls
            if struct.unpack_from("<I", unp, off)[0] != orig:
                sys.exit("refusing: literal at 0x%X is not 0x%08X" % (off, orig))
            struct.pack_into("<I", unp, off, int(man[key], 0))
            print("  %s hook: 0x%08X -> %s" % (key, orig, man[key]))
    if "ge" in man:                   # hook the attach routine's first call (literal 0x0928331C at 0xA9280D14)
        if struct.unpack_from("<I", unp, 0x1280D14)[0] != 0x0928331C:
            sys.exit("refusing: literal at 0x1280D14 is not usbh_msc_get_entry")
        struct.pack_into("<I", unp, 0x1280D14, int(man["ge"], 0))
        print("  attach-entry hook: 0x0928331C -> %s" % man["ge"])
    print("decompressed MAIN: %d bytes (sha256 %s...)" % (len(unp), orig_sha[:16]))
    print("  wrote %d fragment(s), %d bytes in total, into the padding area" % (len(frags), sum(len(f) for _, f in frags)))
    print("  callback literal at 0x%X: 0x%08X -> 0x%08X" % (LITERAL_OFF, ORIG_CALLBACK, entry))

    # ---- recompress into the same space (+ the loader's byte-sum) and rebuild the update
    comp = xt.find_compressor()
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "unpacked.bin"); dst = os.path.join(td, "stream.lz")
        open(src, "wb").write(unp)
        subprocess.check_call([comp, src, dst], stderr=subprocess.DEVNULL)
        stream = open(dst, "rb").read()
    stream += (sum(stream) & 0xFFFF).to_bytes(2, "little")
    room = len(img) - STREAM_OFF
    if len(stream) > room:
        sys.exit("refusing: recompressed stream does not fit (%d > %d)" % (len(stream), room))
    img[STREAM_OFF:STREAM_OFF + len(stream)] = stream
    for k in range(STREAM_OFF + len(stream), len(img)):
        img[k] = 0xFF
    out = xt.build_upd(secs)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    open(a.out, "wb").write(out)
    print("  recompressed stream %d bytes (+2 checksum), %d bytes spare; wrote %s (%d bytes)"
          % (len(stream) - 2, room - len(stream), a.out, len(out)))

    # ---- independent re-verification of what was written
    chk = xt.parse_upd(out)
    cm = chk["main"]
    assert cm["stored_crc"] == cm["calc_crc"] and chk["panl"]["stored_crc"] == chk["panl"]["calc_crc"]
    rr = xt.check_stream(cm["image"], STREAM_OFF)
    assert rr and rr[1] == rr[2], "stream checksum"
    back = xt.lzss_decode(cm["image"], STREAM_OFF + 4, STREAM_OFF + 4 + rr[0])
    assert bytes(back) == bytes(unp), "decompressed result differs from what was patched"
    print("  verified: section CRCs, S-record checksums, stream byte-sum, and a full decode of the new image")
    print("  also verified: bootloader stream and PANL section are byte-identical to the original:",
          cm["image"][0x10000:0x31680] == secs_orig_img(data)[0x10000:0x31680] and chk["panl"]["image"] == xt.parse_upd(data)["panl"]["image"])


def secs_orig_img(data):
    return xt.parse_upd(data)["main"]["image"]


if __name__ == "__main__":
    main()
