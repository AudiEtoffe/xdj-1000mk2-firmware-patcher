# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
# See the LICENSE file in the top directory.
import random, subprocess, sys, os
import shutil
src=sys.argv[1]; img=src+".fuzzcopy"; shutil.copyfile(src,img)   # never corrupt the original in place
iters=int(sys.argv[2]); rnd=random.Random(int(sys.argv[3]))
env=dict(os.environ, ASAN_OPTIONS="exitcode=99:detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1:exitcode=98")
f=open(img,"r+b"); stats={"ok":0,"error-exit":0,"FAIL":0}; fails=[]
for it in range(iters):
    saved=[]
    for _ in range(rnd.randint(1,30)):
        off=rnd.choice([rnd.randrange(0,512),rnd.randrange(0,6*1024*1024),rnd.randrange(0,6*1024*1024)])
        f.seek(off); old=f.read(1); saved.append((off,old)); f.seek(off); f.write(bytes([rnd.getrandbits(8)]))
    f.flush()
    try:
        r=subprocess.run(["./x2f_dump_asan",img,"/dev/null","8192"],capture_output=True,timeout=60,env=env)
        rc=r.returncode; err=r.stderr[-300:]
    except subprocess.TimeoutExpired: rc="timeout"; err=b""
    if rc==0: stats["ok"]+=1
    elif rc==1: stats["error-exit"]+=1
    else: stats["FAIL"]+=1; fails.append((it,rc,err))
    for off,old in saved: f.seek(off); f.write(old)
    f.flush()
print(stats); os.remove(img)
for x in fails[:3]: print(x)
