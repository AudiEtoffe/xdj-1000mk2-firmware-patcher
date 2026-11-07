#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
# See the LICENSE file in the top directory.
"""apply_spec.py - reference implementation of what the website does. apply_spec.py IN.UPD OUT.UPD [--djp] [--hires]"""
import sys, os, json, base64, struct, hashlib, argparse, tempfile, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xdjtool as xt
ap = argparse.ArgumentParser(); ap.add_argument("inp"); ap.add_argument("out"); ap.add_argument("--djp", action="store_true"); ap.add_argument("--hires", action="store_true")
ap.add_argument("--spec", default="patchspec.json"); a = ap.parse_args()
spec = json.load(open(a.spec)); G = spec["groups"]; d64 = lambda s: base64.b64decode(s)
data = open(a.inp, "rb").read()
secs = xt.parse_upd(data); m = secs["main"]; img = m["image"]; SO = spec["streamOff"]
n = int.from_bytes(img[SO:SO + 4], "little"); r = xt.check_stream(img, SO)
assert r and r[1] == r[2], "input stream checksum"
unp = bytearray(xt.lzss_decode(img, SO + 4, SO + 4 + n)); assert len(unp) == spec["unpackedSize"]
is_stock = hashlib.sha256(data).hexdigest() == spec["stockSha256"]
def has_djp(): return all(bytes(unp[o["o"]:o["o"] + len(d64(o["d"]))]) == d64(o["d"]) for o in G["djpatcher"])
def apply(ops):
    for o in ops:
        off, new = o["o"], d64(o["d"]); cur = bytes(unp[off:off + len(new)])
        if o.get("ff"):
            assert all(b == 0xFF for b in cur), "target not free at 0x%X" % off
        elif "e" in o:
            assert cur == d64(o["e"]), "unexpected bytes at 0x%X" % off
        unp[off:off + len(new)] = new
if is_stock:
    if a.djp: apply(G["djpatcher"])
    layout = "layer_djp" if a.djp else "layer_stock"
else:
    assert has_djp(), "input is neither the stock 1.45 file nor the djpatcher (all four options) output"
    layout = "layer_djp"
apply(G[layout])
if a.hires: apply(G["hires"])
apply([{"o": spec["verOff"], "d": base64.b64encode(spec["verTo"].encode()).decode(), "e": base64.b64encode(spec["verFrom"].encode()).decode()}])
nf = bytes(m["name32"]); old = b"Ver" + spec["verFrom"].encode(); assert nf.count(old) == 1
m["name32"] = nf.replace(old, b"Ver" + spec["verTo"].encode())
comp = xt.find_compressor()
with tempfile.TemporaryDirectory() as td:
    s = os.path.join(td, "u"); d = os.path.join(td, "s"); open(s, "wb").write(unp)
    subprocess.check_call([comp, s, d], stderr=subprocess.DEVNULL); stream = open(d, "rb").read()
stream += (sum(stream) & 0xFFFF).to_bytes(2, "little")
assert len(stream) <= len(img) - SO, "stream does not fit"
img[SO:SO + len(stream)] = stream
for k in range(SO + len(stream), len(img)): img[k] = 0xFF
open(a.out, "wb").write(xt.build_upd(secs)); print("wrote", a.out, "layout", layout, "stock-input" if is_stock else "djpatcher-input")
