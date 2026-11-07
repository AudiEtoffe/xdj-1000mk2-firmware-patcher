/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
static void *map(uint32_t at,size_t len){ void*p=mmap((void*)(uintptr_t)at,len,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(p==MAP_FAILED){perror("mmap");exit(3);} return p; }
static void load(FILE*f,uint32_t ramaddr,size_t len){ fseek(f,ramaddr-0x08000000u,SEEK_SET); if(fread((void*)(uintptr_t)ramaddr,1,len,f)!=len){puts("short");exit(4);} }
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); FILE*s=fopen(argv[2],"rb");
  map(0x093A0000,0x10000); map(0x095A6000,0x1000); map(0x0809B000,0x1000); map(0x0C40B000,0x2000);
  load(f,0x093A0000,0x10000); load(f,0x095A6000,0x1000); load(f,0x0809B000,0x1000);
  for(uint32_t *w=(uint32_t*)0x093A5000; w<(uint32_t*)0x093A5A00; w++) if((*w>>24)==0xA8) *w&=0x1FFFFFFFu;
  uint8_t sec[4096]; memset(sec,0,sizeof sec); if(fread(sec,1,512,s)!=512){puts("sector");return 2;}
  uint8_t disk[512]; memset(disk,0,sizeof disk); *(uint16_t*)(disk+96)=1;       /* state 1 = first sector */
  int (*parse)(void*,void*)=(int(*)(void*,void*))0x093A52D2;
  int r=0, guard=0; int16_t st;
  /* phase 1 of the driver's loop (0xA93A704E): repeat until the state word is 1 or 4, failing if it goes negative */
  for(;;){ r=parse(disk,sec); st=*(int16_t*)(disk+96); printf("  p1 pass: ret=%d state=%d disk88=%u disk98=%d\n",r,(int)st,*(uint32_t*)(disk+88),(int)*(int16_t*)(disk+98)); if(r!=0 && st<0) goto done; if(st==1||st==4) break; if(++guard>20) {r=-99; goto done;} }
  *(uint32_t*)(disk+88)=0;
  /* phase 2: repeat until the state word is 1 or 7 */
  for(guard=0;;){ r=parse(disk,sec); st=*(int16_t*)(disk+96); printf("  p2 pass: ret=%d state=%d disk88=%u disk98=%d\n",r,(int)st,*(uint32_t*)(disk+88),(int)*(int16_t*)(disk+98)); if(r!=0 && st<0) break; if(st==1||st==7) break; if(++guard>20){ r=-99; break; } }
  done:
  if(argc>4){   /* steps 2 and 3 of the identify routine: boot sector at the partition start, then the sector at start + disk[54] */
    uint8_t vbr[4096],fat[4096]; memset(vbr,0,sizeof vbr); memset(fat,0,sizeof fat);
    FILE*a=fopen(argv[3],"rb"); FILE*b=fopen(argv[4],"rb"); if(fread(vbr,1,512,a)!=512||fread(fat,1,512,b)!=512){puts("sec");return 2;}
    int (*parse2)(void*,void*)=(int(*)(void*,void*))0x093A503C; int (*parse3)(void*,void*)=(int(*)(void*,void*))0x093A5008;
    int r2=parse2(disk,vbr);
    printf("  classifier2(boot sector at start): ret=%d  disk[54]=%u fs_type=%u\n",r2,*(uint16_t*)(disk+54),disk[16]);
    if(r2>=0){ int r3=parse3(disk,fat); printf("  classifier3(sector start+disk[54]): ret=%d\n",r3); }
  }
  printf("ret=%d  fs_type_byte[disk+16]=%u  state[disk+96]=%d  partition_start[disk+44]=%u\n",r,disk[16],(int)*(int16_t*)(disk+96),*(uint32_t*)(disk+44));
  return 0;
}
