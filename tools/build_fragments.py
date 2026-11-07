#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
# See the LICENSE file in the top directory.
"""
build_fragments.py - build the firmware code so that it fits into whatever free (0xFF) gaps are left in
the padding area (RAM 0xA8119E80-0xA8120000) of a given XDJ1KMK2.UPD, which may already contain other
patches (for example from djpatcher).

    python3 build_fragments.py BASE.UPD OUTDIR [--margin 16]

Compiles the sources with -Os, packs every function/data section into the gaps (first-fit, largest first),
writes a linker script, links against the firmware's alloc/free/original-I/O addresses, and writes
OUTDIR/frag*.bin + OUTDIR/manifest.json (fragment addresses, entry point) for patch_xdj_exfat.py.
"""
import argparse
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xdjtool as xt

CAVE_LO, CAVE_HI = 0x119E80, 0x120000
CC = "sh4-linux-gnu-gcc"
SOURCES = ["xdj_exfat", "x2f", "exfat_ro"]
CFLAGS = ("-std=gnu99 -Os -m4-nofpu -ml -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector "
          "-fno-asynchronous-unwind-tables -fno-unwind-tables -ffunction-sections -fdata-sections "
          "-fno-zero-initialized-in-bss -fno-tree-loop-distribute-patterns -Wall -Wextra").split()
DEFS = {"fw_alloc": 0x09265F78, "fw_free": 0x09265FBE, "fw_orig_io": 0x09281694}


def free_gaps(upd, min_size=64):
    secs = xt.parse_upd(open(upd, "rb").read())
    img = secs["main"]["image"]
    n = int.from_bytes(img[0x40000:0x40004], "little")
    unp = bytes(xt.lzss_decode(img, 0x40004, 0x40004 + 0 + n))
    gaps, i = [], CAVE_LO
    while i < CAVE_HI:
        if unp[i] == 0xFF:
            j = i
            while j < CAVE_HI and unp[j] == 0xFF:
                j += 1
            if j - i >= min_size:
                gaps.append((i, j - i))
            i = j
        else:
            i += 1
    return gaps


def sections_of(obj):
    out = subprocess.check_output(["sh4-linux-gnu-objdump", "-h", obj], text=True)
    items = []
    for m in re.finditer(r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+[0-9a-f]+\s+[0-9a-f]+\s+2\*\*(\d+)", out, re.M):
        name, size, al = m.group(1), int(m.group(2), 16), 1 << int(m.group(3))
        if size and not name.startswith((".comment", ".note", ".eh_frame", ".gnu", ".debug")):
            items.append((os.path.basename(obj), name, size, al))
    return items


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("upd"); ap.add_argument("outdir")
    ap.add_argument("--margin", type=int, default=16, help="bytes kept free in every gap")
    ap.add_argument("--src", default=HERE)
    ap.add_argument("--quiet", action="store_true", help="release build: no diagnostic status text, no USB print hook")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)

    gaps = free_gaps(a.upd)
    print("free gaps in the padding area:")
    for o, s in gaps:
        print("  0x%08X  %5d bytes" % (0x08000000 + o, s))

    objs = []
    for s in SOURCES:
        o = os.path.join(a.outdir, s + ".o")
        subprocess.check_call([CC] + CFLAGS + (["-DXDJ_QUIET"] if a.quiet else []) + ["-I" + a.src, "-c", os.path.join(a.src, s + ".c"), "-o", o])
        objs.append(o)
    items = [it for o in objs for it in sections_of(o)]
    # pass 1: link everything in one block to learn which input sections survive --gc-sections
    libgcc = subprocess.check_output([CC, "-m4-nofpu", "-ml", "-print-libgcc-file-name"], text=True).strip()
    p1 = os.path.join(a.outdir, "pass1.ld")
    open(p1, "w").write("ENTRY(xdj_io)\nSECTIONS { . = 0x08000000; .blob : { *(.text .text.*) *(.rodata .rodata.*) "
                        "*(.data .data.*) *(.sdata .sdata.*) *(.bss .bss.* COMMON) } "
                        "/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu*) } }\n")
    cmd1 = ["sh4-linux-gnu-ld", "-nostdlib", "-static", "--gc-sections"] + ([] if a.quiet else ["-u", "xdj_pf"]) + ["-u", "xdj_ident", "-u", "xdj_ident2", "-u", "xdj_ident3", "--print-gc-sections", "-T", p1]
    for k, v in DEFS.items():
        cmd1 += ["--defsym", "%s=0x%08X" % (k, v)]
    r1 = subprocess.run(cmd1 + ["-Map", os.path.join(a.outdir, "pass1.map"), "-o", os.path.join(a.outdir, "pass1.elf")] + objs + [libgcc],
                        capture_output=True, text=True)
    if r1.returncode:
        sys.exit("pass-1 link failed:\n" + r1.stderr)
    removed = set()
    for m in re.finditer(r"removing unused section '([^']+)' in file '([^']+)'", r1.stderr):
        removed.add((os.path.basename(m.group(2)), m.group(1)))
    items = [it for it in items if (it[0], it[1]) not in removed]
    print("sections kept after gc: %d of %d" % (len(items), len(items) + len(removed)))
    # libgcc helpers that the compiler pulled in (64-bit shifts, block moves): placed like our own sections
    for line in open(os.path.join(a.outdir, "pass1.map")):
        m = re.match(r"^ (\.text\S*)\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+\S*libgcc\.a\((\S+)\)\s*$", line)
        if m and int(m.group(2), 16):
            items.append(("libgcc.a:" + m.group(3), m.group(1), int(m.group(2), 16), 4))
    items.sort(key=lambda t: -t[2])

    # first-fit decreasing, accounting for alignment padding
    free = [[0x08000000 + o, s - a.margin, 0x08000000 + o] for o, s in gaps]   # start, capacity left, cursor
    placement = {}
    for obj, sec, size, al in items:
        for gi, g in enumerate(free):
            cur = (g[2] + al - 1) & ~(al - 1)
            used = cur - g[0] + size
            if used <= g[1]:
                placement.setdefault(gi, []).append((obj, sec))
                g[2] = cur + size
                break
        else:
            sys.exit("cannot place %s %s (%d bytes): not enough free space in the gaps" % (obj, sec, size))

    ld = ["ENTRY(xdj_io)", "SECTIONS {"]
    names = {}
    for gi, g in enumerate(free):
        if gi not in placement:
            continue
        nm = ".frag%d" % gi
        names[nm] = (g[0], gaps[gi][1])
        ld.append("  %s 0x%08X : {" % (nm, g[0]))
        for obj, sec in placement[gi]:
            ld.append("    *%s(%s)" % (obj if obj.startswith("libgcc.a:") else obj, sec))
        ld.append("  }")
    ld += ["  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu*) }", "}"]
    open(os.path.join(a.outdir, "frags.ld"), "w").write("\n".join(ld) + "\n")

    elf = os.path.join(a.outdir, "xdj_exfat.elf")
    cmd = ["sh4-linux-gnu-ld", "-nostdlib", "-static", "--gc-sections"] + ([] if a.quiet else ["-u", "xdj_pf"]) + ["-u", "xdj_ident", "-u", "xdj_ident2", "-u", "xdj_ident3", "-T", os.path.join(a.outdir, "frags.ld")]
    for k, v in DEFS.items():
        cmd += ["--defsym", "%s=0x%08X" % (k, v)]
    cmd += ["-o", elf] + objs + [libgcc]
    r2 = subprocess.run(cmd, capture_output=True, text=True)
    if r2.returncode:
        sys.exit("final link failed:\n" + r2.stderr)
    und = subprocess.check_output(["sh4-linux-gnu-nm", "-u", elf], text=True).split()
    if und:
        sys.exit("undefined symbols: %s" % und)
    nm = subprocess.check_output(["sh4-linux-gnu-nm", elf], text=True)
    entry = int(re.search(r"^([0-9a-f]+) T xdj_io$", nm, re.M).group(1), 16)

    pf = None if a.quiet else int(re.search(r"^([0-9a-f]+) T xdj_pf$", nm, re.M).group(1), 16)
    ident = int(re.search(r"^([0-9a-f]+) T xdj_ident$", nm, re.M).group(1), 16)
    ident2 = int(re.search(r"^([0-9a-f]+) T xdj_ident2$", nm, re.M).group(1), 16)
    ident3 = int(re.search(r"^([0-9a-f]+) T xdj_ident3$", nm, re.M).group(1), 16)
    manifest = {"entry": "0x%08X" % entry, "ident": "0x%08X" % ident, "ident2": "0x%08X" % ident2, "ident3": "0x%08X" % ident3, "fragments": []}
    if pf is not None: manifest["pf"] = "0x%08X" % pf
    total = 0
    for nmf, (addr, cap) in sorted(names.items()):
        binf = os.path.join(a.outdir, nmf[1:] + ".bin")
        subprocess.check_call(["sh4-linux-gnu-objcopy", "-O", "binary", "-j", nmf, elf, binf])
        size = os.path.getsize(binf)
        if size > cap:
            sys.exit("fragment %s is %d bytes but its gap holds %d" % (nmf, size, cap))
        total += size
        manifest["fragments"].append({"addr": "0x%08X" % addr, "file": os.path.basename(binf), "size": size,
                                      "gap": cap})
        print("  %s: %5d bytes at 0x%08X (gap %d)" % (nmf, size, addr, cap))
    json.dump(manifest, open(os.path.join(a.outdir, "manifest.json"), "w"), indent=1)
    print("entry xdj_io = 0x%08X, %d bytes in %d fragment(s)" % (entry, total, len(manifest["fragments"])))


if __name__ == "__main__":
    main()
