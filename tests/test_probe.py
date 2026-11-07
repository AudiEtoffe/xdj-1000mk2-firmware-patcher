# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
# See the LICENSE file in the top directory.
import ctypes,sys,hashlib
lib=ctypes.CDLL("./libxdjtest.so")
lib.xdj_test_cmd.argtypes=[ctypes.c_int,ctypes.c_int,ctypes.c_uint,ctypes.c_void_p,ctypes.c_uint]
lib.xdj_test_get_ver.restype=ctypes.c_char_p
lib.xdj_test_set_ver.argtypes=[ctypes.c_char_p]
lib.xdj_test_set_d44.argtypes=[ctypes.c_uint]
def ver(): return lib.xdj_test_get_ver()[:4].decode()
def rd(lba,n):
    b=ctypes.create_string_buffer(512*n); r=lib.xdj_test_cmd(0,1,lba,b,n); return r,b.raw
ok=True
def chk(name,got,want):
    global ok
    g=(got==want); ok&=g; print(('PASS' if g else 'FAIL'),name,'got',got,'want',want)
# reference: superfloppy exFAT at 0
lib.xdj_test_open(b"t4k.img",0); lib.xdj_test_set_ver(b"1.51"); lib.xdj_test_cmd(0,5,0,None,0)
r,ref=rd(0,1); assert r==0; refboot=hashlib.sha256(ref).hexdigest()
cases=[("dev_mbr.img",2048,"A1"),("dev_mbr2.img",8192,"A1"),("dev_gpt.img",2048,"A2")]
for img,start,_ in cases:
    for d44name,d44 in [("d44=partition start",start),("d44=0 (reset by ct_mount_disk)",0),("d44=stale 777",777)]:
        lib.xdj_test_open(img.encode(),start); lib.xdj_test_set_d44(d44)
        lib.xdj_test_set_ver(b"1.51"); lib.xdj_test_cmd(0,5,0,None,0)
        r,b=rd(0,1)
        tag=f"{img} {d44name}"
        chk(tag+": first read ok",r,0)
        chk(tag+": served virtual FAT32 boot sector identical to superfloppy",hashlib.sha256(b).hexdigest(),refboot)
        v=ver()[2:]; chk(tag+": status is first-read digits",v,"01")
        # now emulate the firmware resetting/setting disk[44] mid-attach and keep reading: FAT, root dir, data
        lib.xdj_test_set_d44(0)
        r,_=rd(32,2); chk(tag+": FAT read after d44 reset",r,0)
        r,fs=rd(1,1)
        chk(tag+": reads keep working with d44=0",r,0)
        lib.xdj_test_set_d44(start)
        r,_=rd(32,2); chk(tag+": and with d44=start",r,0)
# non-exFAT drives are passed through, with the new P digit
for img,start,want in [("dev_mbr_fat.img",2048,"P1")]:
    for d44 in (start,0):
        lib.xdj_test_open(img.encode(),start); lib.xdj_test_set_d44(d44); lib.xdj_test_set_ver(b"1.51"); lib.xdj_test_cmd(0,5,0,None,0)
        rd(0,1); chk(f"{img} d44={d44} passthrough",ver()[2:],want)
lib.xdj_test_open(b"t4k.img",0); lib.xdj_test_set_d44(0); lib.xdj_test_set_ver(b"1.51"); lib.xdj_test_cmd(0,5,0,None,0); rd(0,1); chk("superfloppy exFAT still works",ver()[2:],"01")
print('ALL OK' if ok else 'FAILURES'); sys.exit(0 if ok else 1)
