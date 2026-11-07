# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
# See the LICENSE file in the top directory.
import hashlib, json, sys
from pyfatfs.PyFat import PyFat
img, manifest = sys.argv[1], sys.argv[2]
exp = json.load(open(manifest))
fs = PyFat(); fs.open(img, read_only=True)
from pyfatfs.PyFatFS import PyFatFS
ffs = PyFatFS(img, read_only=True)
print("FAT type:", fs.fat_type, "| clusters:", fs.bpb_header["BPB_TotSec32"] // fs.bpb_header["BPB_SecPerClus"])
root = fs.root_dir
def walk(d, prefix, out):
    for e in d.get_entries()[0] + d.get_entries()[1] + d.get_entries()[2]:
        pass
# pyfatfs API: walk via root_dir.get_entries() -> (dirs, files, specials)
def listing(d, prefix, out):
    dirs, files, specials = d.get_entries()
    for sd in dirs:
        name = sd.get_long_name()
        if name in (".", ".."): continue
        p = prefix + "/" + name
        out[p] = ("D", 0, sd)
        listing(sd, p, out)
    for f in files:
        out[prefix + "/" + f.get_long_name()] = ("F", f.filesize, f)
out = {}
listing(root, "", out)
want = {e["path"]: e for e in exp}
missing = set(want) - set(out); extra = set(out) - set(want)
print("entries: expected %d, found %d, missing %d, extra %d" % (len(want), len(out), len(missing), len(extra)))
for p in list(missing)[:5]: print("  missing:", p)
for p in list(extra)[:5]: print("  extra:", p)
bad = 0; checked = 0
for p, e in want.items():
    if e["kind"] != "F" or p not in out: continue
    kind, size, ent = out[p]
    if size != e["size"]: bad += 1; print("  size mismatch", p, size, e["size"]); continue
    with ffs.openbin(p, 'r') as fh: data = fh.read()
    checked += 1
    if hashlib.sha256(data).hexdigest() != e["sha256"]:
        bad += 1; print("  CONTENT MISMATCH", p)
print("files compared by sha256: %d, mismatches: %d" % (checked, bad))
sys.exit(1 if (missing or extra or bad) else 0)
