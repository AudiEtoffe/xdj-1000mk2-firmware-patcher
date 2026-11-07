#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
# See the LICENSE file in the top directory.
"""mkbase.py - undo the exFAT layer in a 1.50 build, giving back the djpatcher-only base (version text 1.45)."""
import sys, os, json, struct, subprocess, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xdjtool as xt
STREAM_OFF=0x40000; LITERAL_OFF=0x1280D60; ORIG_CB=0x09281694
src, manifest, out = sys.argv[1:4]
data=open(src,'rb').read()
secs=xt.parse_upd(data); m=secs['main']; img=m['image']
n=int.from_bytes(img[STREAM_OFF:STREAM_OFF+4],'little'); end=STREAM_OFF+4+n
unp=bytearray(xt.lzss_decode(img,STREAM_OFF+4,end))
man=json.load(open(manifest)); base=os.path.dirname(os.path.abspath(manifest))
for f in man['fragments']:
    off=int(f['addr'],0)-0x08000000
    fb=open(os.path.join(base,f['file']),'rb').read()
    assert bytes(unp[off:off+len(fb)])==fb, 'fragment %s not present as expected'%f['file']
    unp[off:off+len(fb)]=b'\xff'*len(fb)
assert struct.unpack_from('<I',unp,LITERAL_OFF)[0]==int(man['entry'],0)
struct.pack_into('<I',unp,LITERAL_OFF,ORIG_CB)
v=bytes(unp).find(b'1.50',0x700,0x800); assert v>0 and bytes(unp).count(b'1.50',0x700,0x800)==1
unp[v:v+4]=b'1.45'
nf=bytes(m['name32']); assert nf.count(b'Ver1.50')==1
m['name32']=nf.replace(b'Ver1.50',b'Ver1.45')
comp=xt.find_compressor()
with tempfile.TemporaryDirectory() as td:
    s=os.path.join(td,'u.bin'); d=os.path.join(td,'s.lz'); open(s,'wb').write(unp)
    subprocess.check_call([comp,s,d],stderr=subprocess.DEVNULL); stream=open(d,'rb').read()
stream+=(sum(stream)&0xFFFF).to_bytes(2,'little')
room=len(img)-STREAM_OFF; assert len(stream)<=room
img[STREAM_OFF:STREAM_OFF+len(stream)]=stream
for k in range(STREAM_OFF+len(stream),len(img)): img[k]=0xFF
open(out,'wb').write(xt.build_upd(secs)); print('wrote',out)
