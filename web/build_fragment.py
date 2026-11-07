#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
# See the LICENSE file in the top directory.
"""Build the two pages from web/ + data/patchspec.json:
     index.html                                  standalone page (what GitHub Pages serves; also works offline)
     embed/xdj-patcher-PASTE-INTO-BODY.html      body-only fragment for site builders that want content between <body> tags
Run from anywhere:  python3 web/build_fragment.py"""
import os, zlib, base64
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
rd = lambda p: open(os.path.join(HERE, p), encoding='utf-8').read()
lib = rd('xdjpatch.js')
spec = open(os.path.join(ROOT, 'data', 'patchspec.json'), 'rb').read()
c = zlib.compressobj(9, zlib.DEFLATED, -15); b64 = base64.b64encode(c.compress(spec) + c.flush()).decode()   # raw deflate, unpacked in the page with DecompressionStream
assert '</script' not in lib.lower()
note = ('<!-- XDJ-1000MK2 firmware patcher. SPDX-License-Identifier: GPL-2.0-only. Copyright (C) 2026 Audi Etoffe.\n'
        '     Source and licence: https://github.com/AudiEtoffe/xdj-1000mk2-firmware-patcher -->\n')
frag = note + rd('frag_parts.html') + '<script id="xdjp-lib">\n' + lib + '\n</script>\n<script id="xdjp-spec" type="text/plain">' + b64 + '</script>\n' + rd('frag_js.js') + '\n'
full = ('<!DOCTYPE html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n<meta name="viewport" content="width=device-width, initial-scale=1">\n'
        '<title>XDJ-1000MK2 Firmware 2.0 Patcher</title>\n</head>\n<body style="margin:0">\n' + frag + '</body>\n</html>\n')
os.makedirs(os.path.join(ROOT, 'embed'), exist_ok=True)
open(os.path.join(ROOT, 'index.html'), 'w', encoding='utf-8').write(full)
open(os.path.join(ROOT, 'embed', 'xdj-patcher-PASTE-INTO-BODY.html'), 'w', encoding='utf-8').write(frag)
print('index.html', len(full), 'bytes | embed fragment', len(frag), 'bytes')
