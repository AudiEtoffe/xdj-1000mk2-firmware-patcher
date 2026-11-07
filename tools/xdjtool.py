#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
# See the LICENSE file in the top directory.
"""
xdjtool - unpack / repack / verify Pioneer XDJ-1000MK2 style .UPD firmware files.

Container format (as observed in XDJ1KMK2.UPD, MAIN v1.45 / PANL v1.00):

    "<len1>\r\n"  "<len2>\r\n"              ASCII decimal byte lengths of the two sections
    section 1 (MAIN) : 32-byte name field | S-record lines (CRLF) | 2-byte CRC
    section 2 (PANL) : 32-byte name field | S-record lines (CRLF) | 2-byte CRC

  * Records are Motorola S-records: S0 header, S2 data (24-bit address), S7/S8 entry.
  * CRC = CRC-16/CCITT (poly 0x1021, init 0, no reflection, i.e. XMODEM) computed over the
    whole section except the CRC itself, stored little-endian.

Commands
    unpack  IN.UPD OUTDIR            -> OUTDIR/main.bin, panl.bin, layout.json
    repack  DIR OUT.UPD              -> rebuilds a valid .UPD (recomputes checksums, CRCs, lengths)
    verify  FILE.UPD                 -> checks lengths, CRCs and every S-record checksum
    info    FILE.UPD                 -> prints sections and address ranges
    diff    A.UPD B.UPD              -> lists changed address ranges per section
    addseg  DIR SECTION ADDR LEN     -> declare a new address range (extends the .bin if needed)
    sections FILE.UPD                -> list sections of any container (also the 4-section NXS2 file)
    extract FILE.UPD OUTDIR          -> dump every section (+ flat image for S-record sections)
    streams main.bin                 -> find the checksummed LZSS streams inside a flat image
    unlz    main.bin OUT.bin         -> decompress the big MAIN stream at 0x40000 (~29 MB result)
    relz    main.bin UNPACKED.bin    -> recompress a (patched) unpacked image back into main.bin
                                        (also writes the 16-bit byte-sum the stage-1 loader checks)
                                        (needs lzsscomp; built automatically if gcc is installed)

main.bin / panl.bin are flat images: file offset N holds the byte for address (base + N).
"base" is stored in layout.json (0 for MAIN). Bytes outside the declared segments are not
written back to the .UPD (gaps are 0xFF filler in the image).

Nothing here knows whether the player's bootloader performs extra checks (signatures, address
validity). It only guarantees the file is structurally valid and identical where unmodified.
"""
import argparse
import binascii
import json
import os
import sys

NAME_LEN = 32
KEYS = ("main", "panl")


class UpdError(Exception):
    pass


def crc16(data):
    # binascii.crc_hqx is CRC-CCITT (0x1021), no reflection; init 0 == XMODEM
    return binascii.crc_hqx(data, 0)


# ---------------------------------------------------------------- S-records

def parse_srec(line):
    if len(line) < 4 or line[:1] != b"S":
        raise UpdError("not an S-record: %r" % line[:20])
    try:
        raw = bytes.fromhex(line[2:].decode("ascii"))
    except ValueError:
        raise UpdError("bad hex in record: %r" % line[:20])
    if raw[0] != len(raw) - 1:
        raise UpdError("bad byte count in record: %r" % line[:20])
    if ((~sum(raw[:-1])) & 0xFF) != raw[-1]:
        raise UpdError("bad record checksum: %r" % line[:20])
    return line[1:2], raw[1:-1]


def make_s2(addr, data):
    if not data or len(data) > 251:
        raise UpdError("bad S2 record length %d" % len(data))
    if addr < 0 or addr + len(data) > 0x1000000:
        raise UpdError("address 0x%X out of S2 (24-bit) range" % addr)
    p = bytes([len(data) + 4]) + addr.to_bytes(3, "big") + data
    return b"S2" + (p + bytes([(~sum(p)) & 0xFF])).hex().upper().encode("ascii")


# ---------------------------------------------------------------- segments

def build_segments(recs):
    """Group records into runs [addr, rec_size, total_len] so the original
    record boundaries can be regenerated exactly."""
    segs = []
    prev_end = -1
    for a, d in recs:
        n = len(d)
        if n == 0:
            raise UpdError("empty data record at 0x%X" % a)
        if a < prev_end:
            raise UpdError("records not in ascending, non-overlapping order at 0x%X" % a)
        prev_end = a + n
        if segs:
            s = segs[-1]
            if a == s[0] + s[2] and s[2] % s[1] == 0 and n <= s[1]:
                s[2] += n
                continue
        segs.append([a, n, n])
    return segs


def expand(segs):
    for a, size, total in segs:
        for off in range(0, total, size):
            yield a + off, min(size, total - off)


# ---------------------------------------------------------------- sections

def parse_section(sec):
    if len(sec) < NAME_LEN + 2:
        raise UpdError("section too short")
    stored = int.from_bytes(sec[-2:], "little")
    calc = crc16(sec[:-2])
    body = sec[NAME_LEN:-2]
    if not body.endswith(b"\r\n"):
        raise UpdError("section body does not end with CRLF")
    head, tail, recs = [], [], []
    state = 0
    for ln in body[:-2].split(b"\r\n"):
        t, rest = parse_srec(ln)
        if t == b"2":
            if state == 2:
                raise UpdError("S2 record after trailer records")
            state = 1
            recs.append((int.from_bytes(rest[:3], "big"), rest[3:]))
        else:
            if state == 0:
                head.append(ln.decode("ascii"))
            else:
                tail.append(ln.decode("ascii"))
                state = 2
    segs = build_segments(recs)
    base = recs[0][0] if recs else 0
    end = max((a + len(d) for a, d in recs), default=base)
    img = bytearray(b"\xff" * (end - base))
    for a, d in recs:
        img[a - base:a - base + len(d)] = d
    return {
        "name32": bytes(sec[:NAME_LEN]),
        "head": head,
        "tail": tail,
        "segments": segs,
        "base": base,
        "image": img,
        "stored_crc": stored,
        "calc_crc": calc,
    }


def stray_bytes(sec):
    """Count non-0xFF bytes in the image that lie outside every segment."""
    base, img = sec["base"], sec["image"]
    pos = base
    n = 0
    for a, _, total in sec["segments"]:
        if a > pos:
            chunk = img[pos - base:a - base]
            n += len(chunk) - chunk.count(0xFF)
        pos = max(pos, a + total)
    chunk = img[pos - base:]
    n += len(chunk) - chunk.count(0xFF)
    return n


def build_section(sec):
    base, img = sec["base"], sec["image"]
    lines = [h.encode("ascii") for h in sec["head"]]
    for a, n in expand(sec["segments"]):
        off = a - base
        if off < 0 or off + n > len(img):
            raise UpdError("segment at 0x%X (+%d) lies outside the image; "
                           "use 'addseg' to extend it" % (a, n))
        lines.append(make_s2(a, bytes(img[off:off + n])))
    lines += [t.encode("ascii") for t in sec["tail"]]
    out = bytes(sec["name32"]) + b"".join(l + b"\r\n" for l in lines)
    return out + crc16(out).to_bytes(2, "little")


# ---------------------------------------------------------------- whole file

def parse_upd(data):
    i = data.find(b"\r\n")
    j = data.find(b"\r\n", i + 2) if i >= 0 else -1
    if i < 0 or j < 0:
        raise UpdError("missing length header lines")
    try:
        n1, n2 = int(data[:i]), int(data[i + 2:j])
    except ValueError:
        raise UpdError("length header is not decimal ASCII")
    start = j + 2
    if start + n1 + n2 != len(data):
        raise UpdError("header lengths (%d + %d) do not match file size" % (n1, n2))
    return {
        "main": parse_section(data[start:start + n1]),
        "panl": parse_section(data[start + n1:]),
    }


def build_upd(secs):
    s1, s2 = build_section(secs["main"]), build_section(secs["panl"])
    return (str(len(s1)).encode() + b"\r\n" + str(len(s2)).encode() + b"\r\n" + s1 + s2)


# ---------------------------------------------------------------- directory I/O

def save_dir(secs, outdir):
    os.makedirs(outdir, exist_ok=True)
    layout = {"format": 1, "sections": {}}
    for k in KEYS:
        s = secs[k]
        with open(os.path.join(outdir, k + ".bin"), "wb") as f:
            f.write(s["image"])
        layout["sections"][k] = {
            "name32_hex": s["name32"].hex(),
            "head": s["head"],
            "tail": s["tail"],
            "base": s["base"],
            "segments": s["segments"],
        }
    with open(os.path.join(outdir, "layout.json"), "w") as f:
        json.dump(layout, f, indent=1)


def load_dir(d):
    with open(os.path.join(d, "layout.json")) as f:
        layout = json.load(f)
    secs = {}
    for k in KEYS:
        m = layout["sections"][k]
        with open(os.path.join(d, k + ".bin"), "rb") as f:
            img = bytearray(f.read())
        secs[k] = {
            "name32": bytes.fromhex(m["name32_hex"]),
            "head": m["head"],
            "tail": m["tail"],
            "base": m["base"],
            "segments": m["segments"],
            "image": img,
        }
    return secs



# ---------------------------------------------------------------- LZSS (MAIN payload)

LZ_N = 4096


def lzss_decode(data, start, end, ring_init=0x20, ring_pos=0xFEE):
    """Flag-byte LZSS as used by the MAIN payload: flags LSB-first, 1 = literal byte,
    0 = 2-byte reference (b1 | (b2 & 0xF0) << 4 = absolute ring position,
    (b2 & 0x0F) + 3 = length); 4096-byte ring, initially filled with `ring_init`,
    write position starting at `ring_pos`."""
    ring = bytearray([ring_init]) * LZ_N
    out = bytearray()
    r, i = ring_pos, start
    while i < end:
        fl = data[i]
        i += 1
        for bit in range(8):
            if i >= end:
                return out
            if (fl >> bit) & 1:
                b = data[i]
                i += 1
                out.append(b)
                ring[r] = b
                r = (r + 1) & 0xFFF
            else:
                if i + 1 >= end:
                    return out
                b1, b2 = data[i], data[i + 1]
                i += 2
                p = b1 | ((b2 & 0xF0) << 4)
                for k in range((b2 & 0x0F) + 3):
                    b = ring[(p + k) & 0xFFF]
                    out.append(b)
                    ring[r] = b
                    r = (r + 1) & 0xFFF
    return out


# ---------------------------------------------------------------- reporting

def merged_ranges(segs):
    out = []
    for a, _, total in segs:
        if out and out[-1][1] == a:
            out[-1][1] = a + total
        else:
            out.append([a, a + total])
    return out


def describe(secs, data_len=None):
    for k in KEYS:
        s = secs[k]
        ok = "OK" if s["stored_crc"] == s["calc_crc"] else "BAD (stored %04X, calc %04X)" % (
            s["stored_crc"], s["calc_crc"])
        name = s["name32"].split(b"\0")[0].decode("ascii", "replace").strip()
        rng = merged_ranges(s["segments"])
        total = sum(b - a for a, b in rng)
        print("[%s] %s  | CRC16 %s | %d bytes of data in %d range(s), %d record run(s)" % (
            k, name, ok, total, len(rng), len(s["segments"])))
        for a, b in rng[:12]:
            print("      0x%06X - 0x%06X  (%d bytes)" % (a, b, b - a))
        if len(rng) > 12:
            print("      ... %d more ranges" % (len(rng) - 12))
        if s["tail"]:
            print("      trailer records:", ", ".join(s["tail"]))


# ---------------------------------------------------------------- commands

def cmd_unpack(a):
    data = open(a.input, "rb").read()
    secs = parse_upd(data)
    for k in KEYS:
        if secs[k]["stored_crc"] != secs[k]["calc_crc"]:
            print("warning: %s CRC in input file is wrong" % k)
    if build_upd(secs) != data:
        raise UpdError("internal check failed: repacking the parsed file does not reproduce "
                       "the input byte-for-byte; refusing to unpack")
    save_dir(secs, a.outdir)
    describe(secs)
    print("unpacked to %s (round-trip check: byte-identical)" % a.outdir)


def cmd_repack(a):
    secs = load_dir(a.dir)
    for k in KEYS:
        n = stray_bytes(secs[k])
        if n:
            print("warning: %s.bin has %d non-0xFF bytes outside declared segments; they will "
                  "NOT be written (use 'addseg' to declare the range)" % (k, n))
    out = build_upd(secs)
    with open(a.output, "wb") as f:
        f.write(out)
    chk = parse_upd(out)
    for k in KEYS:
        assert chk[k]["stored_crc"] == chk[k]["calc_crc"]
    print("wrote %s (%d bytes); lengths, record checksums and CRCs recomputed and re-verified"
          % (a.output, len(out)))


def cmd_verify(a):
    data = open(a.input, "rb").read()
    secs = parse_upd(data)  # raises on bad lengths / record checksums
    bad = False
    for k in KEYS:
        if secs[k]["stored_crc"] != secs[k]["calc_crc"]:
            bad = True
    describe(secs)
    print("lengths OK, all S-record checksums OK" + (", CRC MISMATCH" if bad else ", CRCs OK"))
    m = secs["main"]
    for name, off in (("bootloader stream", 0x10000), ("MAIN stream", 0x40000)):
        r = check_stream(m["image"], off - m["base"]) if m["base"] <= off else None
        if r is None:
            continue
        n, stored, calc = r
        ok = stored == calc
        print("%s @0x%X: length %d, loader byte-sum stored 0x%04X calculated 0x%04X  %s" % (
            name, off, n, stored, calc, "OK" if ok else "MISMATCH - the player would not start MAIN"))
        bad = bad or not ok
    return 1 if bad else 0


def cmd_info(a):
    secs = parse_upd(open(a.input, "rb").read())
    describe(secs)


def cmd_diff(a):
    A = parse_upd(open(a.a, "rb").read())
    B = parse_upd(open(a.b, "rb").read())
    for k in KEYS:
        x, y = A[k], B[k]
        print("[%s]" % k)
        if x["name32"] != y["name32"] or x["head"] != y["head"] or x["tail"] != y["tail"]:
            print("  header/trailer fields differ")
        if x["base"] != y["base"] or x["segments"] != y["segments"]:
            print("  segment layout differs (A: %d runs, B: %d runs)" % (
                len(x["segments"]), len(y["segments"])))
            continue
        ia, ib = x["image"], y["image"]
        if ia == ib:
            print("  identical")
            continue
        runs = []
        blk = 65536
        for off in range(0, max(len(ia), len(ib)), blk):
            if ia[off:off + blk] == ib[off:off + blk]:
                continue
            for i in range(off, min(off + blk, max(len(ia), len(ib)))):
                p = ia[i] if i < len(ia) else None
                q = ib[i] if i < len(ib) else None
                if p != q:
                    if runs and runs[-1][1] == i:
                        runs[-1][1] = i + 1
                    else:
                        runs.append([i, i + 1])
        print("  %d changed range(s)" % len(runs))
        for s, e in runs[:50]:
            print("    0x%06X  len %-5d %s -> %s" % (
                x["base"] + s, e - s, ia[s:min(e, s + 16)].hex(), ib[s:min(e, s + 16)].hex()))
        if len(runs) > 50:
            print("    ... %d more" % (len(runs) - 50))



def cmd_unlz(a):
    d = open(a.input, "rb").read()
    off = int(a.offset, 0)
    n = int.from_bytes(d[off:off + 4], "little")
    end = off + 4 + n
    if end > len(d):
        raise UpdError("length field 0x%X at 0x%X runs past the end of the file" % (n, off))
    out = lzss_decode(d, off + 4, end)
    with open(a.output, "wb") as f:
        f.write(out)
    print("stream at 0x%X: %d compressed bytes -> %d bytes, written to %s" % (off, n, len(out), a.output))
    if end < len(d):
        print("(%d bytes follow the stream in the input)" % (len(d) - end))


def check_stream(img, off):
    """Stage-1 loader check: 16-bit sum of (4-byte length + payload) must equal the
    little-endian 16-bit value stored immediately after the stream (else it falls back
    to the bootloader instead of starting MAIN). Returns (length, stored, calculated)."""
    n = int.from_bytes(img[off:off + 4], "little")
    end = off + 4 + n
    if end + 2 > len(img):
        return None
    return n, int.from_bytes(img[end:end + 2], "little"), sum(img[off:end]) & 0xFFFF


def find_compressor():
    """Locate (or build) the lzsscomp helper next to this script or on PATH."""
    import shutil
    import subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    exe = "lzsscomp.exe" if os.name == "nt" else "lzsscomp"
    for cand in (os.path.join(here, exe), shutil.which("lzsscomp")):
        if cand and os.path.exists(cand):
            return cand
    src = os.path.join(here, "lzsscomp.c")
    cc = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
    if os.path.exists(src) and cc:
        out = os.path.join(here, exe)
        print("building the compressor with %s ..." % os.path.basename(cc))
        subprocess.check_call([cc, "-O2", "-o", out, src])
        return out
    raise UpdError("compressor not found: put lzsscomp(.exe) next to xdjtool.py, or keep "
                   "lzsscomp.c there and install gcc (Linux/WSL: apt install gcc; Windows: MSYS2 gcc)")


def cmd_relz(a):
    import subprocess
    import tempfile
    d = bytearray(open(a.main, "rb").read())
    off = int(a.offset, 0)
    comp = find_compressor()
    with tempfile.TemporaryDirectory() as td:
        tmp = os.path.join(td, "stream.lz")
        subprocess.check_call([comp, a.unpacked, tmp])
        blob = open(tmp, "rb").read()
    blob += (sum(blob) & 0xFFFF).to_bytes(2, "little")   # the loader's byte-sum check value
    room = len(d) - off
    if len(blob) > room:
        raise UpdError("recompressed stream is %d bytes too large for the space in main.bin "
                       "(%d available)" % (len(blob) - room, room))
    d[off:off + len(blob)] = blob
    for k in range(off + len(blob), len(d)):
        d[k] = 0xFF
    out = a.out or a.main
    with open(out, "wb") as f:
        f.write(d)
    print("wrote %s: stream %d bytes + 2-byte checksum 0x%04X, %d spare bytes in the original space" % (
        out, len(blob) - 6, int.from_bytes(blob[-2:], "little"), room - len(blob)))


def split_container(data):
    """Generic container split: N ASCII length lines, then N sections (works for 2-section XDJ
    files and the 4-section CDJ-2000NXS2 file)."""
    pos, nums = 0, []
    while True:
        j = data.find(b"\r\n", pos)
        if j < 0 or j - pos > 10 or not data[pos:j].isdigit():
            break
        nums.append(int(data[pos:j]))
        pos = j + 2
    if not nums or pos + sum(nums) != len(data):
        raise UpdError("not a recognised container (length lines do not add up to the file size)")
    secs, off = [], pos
    for n in nums:
        secs.append(data[off:off + n])
        off += n
    return secs


def sec_name(sec):
    return sec[:NAME_LEN].split(b"\0")[0].decode("ascii", "replace").strip()


def cmd_sections(a):
    secs = split_container(open(a.input, "rb").read())
    for i, sec in enumerate(secs):
        kind = "S-records" if sec[NAME_LEN:NAME_LEN + 2] == b"S0" else "binary"
        crc_ok = crc16(sec[:-2]) == int.from_bytes(sec[-2:], "little")
        print("[%d] %-28s %9d bytes  %-9s  trailing CRC16 %s" % (
            i, sec_name(sec), len(sec), kind, "OK" if crc_ok else "no/other"))


def cmd_extract(a):
    secs = split_container(open(a.input, "rb").read())
    os.makedirs(a.outdir, exist_ok=True)
    for i, sec in enumerate(secs):
        tag = "".join(c if c.isalnum() else "_" for c in sec_name(sec))
        raw = os.path.join(a.outdir, "%d_%s.sec" % (i, tag))
        open(raw, "wb").write(sec)
        line = "wrote %s" % raw
        if sec[NAME_LEN:NAME_LEN + 2] == b"S0":
            p = parse_section(sec)
            flat = os.path.join(a.outdir, "%d_%s.bin" % (i, tag))
            open(flat, "wb").write(p["image"])
            line += " and flat image %s (base 0x%X, %d bytes)" % (flat, p["base"], len(p["image"]))
        print(line)


def cmd_streams(a):
    img = open(a.input, "rb").read()
    found = 0
    for off in range(0, len(img) - 8, 0x20):
        n = int.from_bytes(img[off:off + 4], "little")
        if 1000 < n < len(img) - off - 6:
            r = check_stream(img, off)
            if r and r[1] == r[2]:
                print("LZSS stream at 0x%X: %d bytes, ends 0x%X, byte-sum 0x%04X OK" % (off, n, off + 4 + n, r[1]))
                found += 1
    if not found:
        print("no checksummed LZSS streams found")


def cmd_addseg(a):
    secs = load_dir(a.dir)
    if a.section not in KEYS:
        raise UpdError("section must be one of %s" % (KEYS,))
    s = secs[a.section]
    addr, length = int(a.addr, 0), int(a.length, 0)
    if length <= 0 or a.rec_size < 1 or a.rec_size > 251:
        raise UpdError("bad length or record size")
    for sa, _, st in s["segments"]:
        if addr < sa + st and sa < addr + length:
            raise UpdError("range overlaps existing segment 0x%X-0x%X" % (sa, sa + st))
    if addr + length > 0x1000000:
        raise UpdError("S2 records only address 24 bits (< 0x1000000)")
    # grow / rebase the image so it covers the new range
    if addr < s["base"]:
        s["image"] = bytearray(b"\xff" * (s["base"] - addr)) + s["image"]
        s["base"] = addr
    need = addr + length - s["base"]
    if need > len(s["image"]):
        s["image"].extend(b"\xff" * (need - len(s["image"])))
    s["segments"].append([addr, a.rec_size, length])
    s["segments"].sort()
    save_dir(secs, a.dir)
    print("added 0x%X-0x%X to %s (image now %d bytes, base 0x%X)" % (
        addr, addr + length, a.section, len(s["image"]), s["base"]))
    print("note: this only edits the file layout; whether the player's loader accepts or "
          "maps that address range is unknown.")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("unpack"); s.add_argument("input"); s.add_argument("outdir"); s.set_defaults(f=cmd_unpack)
    s = sub.add_parser("repack"); s.add_argument("dir"); s.add_argument("output"); s.set_defaults(f=cmd_repack)
    s = sub.add_parser("verify"); s.add_argument("input"); s.set_defaults(f=cmd_verify)
    s = sub.add_parser("info"); s.add_argument("input"); s.set_defaults(f=cmd_info)
    s = sub.add_parser("diff"); s.add_argument("a"); s.add_argument("b"); s.set_defaults(f=cmd_diff)
    s = sub.add_parser("unlz"); s.add_argument("input"); s.add_argument("output")
    s.add_argument("--offset", default="0x40000", help="where the 4-byte length + LZSS stream starts in main.bin")
    s.set_defaults(f=cmd_unlz)
    s = sub.add_parser("relz"); s.add_argument("main"); s.add_argument("unpacked")
    s.add_argument("--out", help="write here instead of overwriting main.bin")
    s.add_argument("--offset", default="0x40000"); s.set_defaults(f=cmd_relz)
    s = sub.add_parser("sections"); s.add_argument("input"); s.set_defaults(f=cmd_sections)
    s = sub.add_parser("extract"); s.add_argument("input"); s.add_argument("outdir"); s.set_defaults(f=cmd_extract)
    s = sub.add_parser("streams"); s.add_argument("input"); s.set_defaults(f=cmd_streams)
    s = sub.add_parser("addseg"); s.add_argument("dir"); s.add_argument("section")
    s.add_argument("addr"); s.add_argument("length")
    s.add_argument("--rec-size", type=int, default=32); s.set_defaults(f=cmd_addseg)
    a = p.parse_args(argv)
    try:
        return a.f(a) or 0
    except UpdError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
