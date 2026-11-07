#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
# See the LICENSE file in the top directory.
"""make_spec.py - write patchspec.json (maintainers only; the finished file is committed as data/patchspec.json).
Run it in a work directory that holds: stock145.UPD (Pioneer's original), stock_img.bin and base_img.bin (the decompressed MAIN images of the
stock file and of djpatcher's all-four-options output; make them with tools/xdjtool.py unpack, then unlz --offset 0x40000), and rel_stock/ and
rel_djp/ (tools/build_fragments.py --quiet output for each base). Original docstring: everything the website needs, as data (no firmware bytes of Pioneer's except the
changed regions themselves, which are my code/djpatcher's edits)."""
import sys, os, json, base64, struct, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import patch_xdj_exfat as px
b64 = lambda b: base64.b64encode(bytes(b)).decode()
stock = open('stock_img.bin', 'rb').read(); djp = open('base_img.bin', 'rb').read()
assert len(stock) == len(djp) == px.EXPECT_SIZE
def w(off, new, old=None, ff=False):
    o = {"o": off, "d": b64(new)}
    if ff: o["ff"] = True
    elif old is not None: o["e"] = b64(old)
    return o
# --- djpatcher bundle: every range where the djpatcher output differs from stock (new bytes only; stock bytes are checked by hash)
ranges = []; i = 0; n = len(stock)
while i < n:
    if stock[i] != djp[i]:
        j = i
        while j < n and (stock[j] != djp[j] or any(stock[k] != djp[k] for k in range(j, min(j + 16, n)))): j += 1
        e = j
        while e > i and stock[e - 1] == djp[e - 1]: e -= 1
        ranges.append((i, e)); i = j
    else: i += 1
djp_ops = [w(a, djp[a:b]) for a, b in ranges]
def layer(manifest_path):
    man = json.load(open(manifest_path)); base = os.path.dirname(os.path.abspath(manifest_path))
    ops = []
    for f in man["fragments"]:
        off = int(f["addr"], 0) - 0x08000000; fb = open(os.path.join(base, f["file"]), "rb").read()
        ops.append(w(off, fb, ff=True))
    def lit(off, old, new): ops.append(w(off, struct.pack("<I", new), struct.pack("<I", old)))
    lit(px.LITERAL_OFF, px.ORIG_CALLBACK, int(man["entry"], 0))
    lit(0x1281708, 0x093A52D2, int(man["ident"], 0)); lit(0x1281714, 0x093A503C, int(man["ident2"], 0)); lit(0x128171C, 0x093A5008, int(man["ident3"], 0))
    name, off, old, new = px.SUPERFLOPPY; ops.append(w(off, new, old))
    assert "pf" not in man
    return ops
hires = [w(off, new, old) for name, off, old, new in px.HIRES]
spec = {
    "v": 1, "unpackedSize": px.EXPECT_SIZE, "streamOff": px.STREAM_OFF,
    "stockSha256": hashlib.sha256(open('stock145.UPD', 'rb').read()).hexdigest(),
    "verFrom": "1.45", "verTo": "2.00", "verOff": 0x740,
    "groups": {"djpatcher": djp_ops, "layer_stock": layer('rel_stock/manifest.json'), "layer_djp": layer('rel_djp/manifest.json'), "hires": hires},
}
json.dump(spec, open('patchspec.json', 'w'), separators=(',', ':'))
print('djpatcher ranges', len(djp_ops), sum(len(o["d"]) * 3 // 4 for o in djp_ops), 'bytes; spec size', os.path.getsize('patchspec.json'))
