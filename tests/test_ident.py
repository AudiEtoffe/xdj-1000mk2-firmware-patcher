# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
# See the LICENSE file in the top directory.
import ctypes,hashlib,sys
lib=ctypes.CDLL("./libxdjtest.so")
lib.xdj_test_cmd.argtypes=[ctypes.c_int,ctypes.c_int,ctypes.c_uint,ctypes.c_void_p,ctypes.c_uint]
lib.xdj_test_set_d44.argtypes=[ctypes.c_uint]
lib.xdj_test_parse_seen.restype=ctypes.POINTER(ctypes.c_ubyte*512)
lib.xdj_test_ident.argtypes=[ctypes.c_char_p]
def sec0(f):
    with open(f,'rb') as g: return g.read(512)
lib.xdj_test_open(b"t4k.img",0); lib.xdj_test_cmd(0,5,0,None,0)
b=ctypes.create_string_buffer(512); lib.xdj_test_cmd(0,1,0,b,1); ref=hashlib.sha256(b.raw).hexdigest()
def seen(): return hashlib.sha256(bytes(lib.xdj_test_parse_seen().contents)).hexdigest()
ok=True
for img,start in [("t4k.img",0),("dev_mbr.img",2048),("dev_mbr2.img",8192),("dev_gpt.img",2048)]:
    for d44 in (0,start,777):
        lib.xdj_test_open(img.encode(),start); lib.xdj_test_set_d44(d44); lib.xdj_test_cmd(0,5,0,None,0)
        lib.xdj_test_ident(sec0(img)); g=(seen()==ref); ok&=g
        print('PASS' if g else 'FAIL',img,'d44=%d'%d44)
lib.xdj_test_open(b"dev_mbr_fat.img",2048); lib.xdj_test_cmd(0,5,0,None,0)
real=sec0("dev_mbr_fat.img"); lib.xdj_test_ident(real); g=(seen()==hashlib.sha256(real).hexdigest()); ok&=g
print('PASS' if g else 'FAIL','FAT32 stick untouched')
print('ALL OK' if ok else 'FAILURES'); sys.exit(0 if ok else 1)
