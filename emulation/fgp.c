/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
static uint8_t g_sector[512]; static FILE *g_dev; static uint8_t g_buf[8192];
static void *t_alloc(unsigned n){ (void)n; memset(g_buf,0,sizeof g_buf); return g_buf; }
static void t_free(void *p){ (void)p; }
static int t_read(void *disk, void *buf, unsigned lba, unsigned cnt){ (void)disk; fseek(g_dev,(long long)lba*512,SEEK_SET); return fread(buf,1,512*(cnt?cnt:1),g_dev)>0?0:-1; }
static void stub(uint32_t at, void *target){
  static const uint8_t code[6]={0x01,0xD0,0x2B,0x40,0x09,0x00};
  uint32_t lit=((at+4)&~3u)+4,t=(uint32_t)(uintptr_t)target; memcpy((void*)(uintptr_t)at,code,6); memcpy((void*)(uintptr_t)lit,&t,4);
}
static void *map(uint32_t at,size_t len){ void*p=mmap((void*)(uintptr_t)at,len,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(p==MAP_FAILED){perror("mmap");exit(3);} return p; }
static void load(FILE*f,uint32_t ramaddr,size_t len){ uint32_t off=ramaddr-0x08000000u; fseek(f,off,SEEK_SET); if(fread((void*)(uintptr_t)ramaddr,1,len,f)!=len){puts("short read");exit(4);} }
int main(int argc,char**argv){
  uint32_t hook=0;
  FILE*f=fopen(argv[1],"rb"); g_dev=fopen(argv[2],"rb");
  map(0x093A0000,0x10000); map(0x093E8000,0x1000); map(0x095A6000,0x1000); map(0x0809B000,0x1000); map(0x09265000,0x1000); map(0x09282000,0x1000);
  load(f,0x093A0000,0x10000); load(f,0x093E8000,0x1000); load(f,0x095A6000,0x1000); load(f,0x0809B000,0x1000);
  for(uint32_t *w=(uint32_t*)0x093A4D00; w<(uint32_t*)0x093A5020; w++) if((*w>>24)==0xA8) *w&=0x1FFFFFFFu;   /* P2 alias -> P0 view of the same RAM (user-mode emulation has no P2) */
  stub(0x09265F78,t_alloc); stub(0x09265FBE,t_free); stub(0x092825EC,t_read);
  if(argc>3){ hook=strtoul(argv[3],0,0); map(0x08000000,0x1000); map(0x08119000,0x7000); memcpy((void*)0x08000740,"1.54",4);
    for(int i=4;i+1<argc;i+=2){ uint32_t a=strtoul(argv[i],0,0); FILE*bf=fopen(argv[i+1],"rb"); fseek(bf,0,SEEK_END); long n=ftell(bf); fseek(bf,0,SEEK_SET); if(fread((void*)(uintptr_t)a,1,n,bf)!=(size_t)n)return 5; } }
  int (*fgp)(void*,void*)=(int(*)(void*,void*))(hook?hook:0x093A4A8C);
  uint32_t out[256]; memset(out,0,sizeof out); out[0]=0xDEAD; uint8_t disk[512]={0};
  int r=fgp(disk,out);
  if(hook) printf("version text: %.4s  ",(char*)0x08000740);
  printf("ret=%d  count_word=%s (%u)\n",r,out[0]==0xDEAD?"UNTOUCHED (=> whole-disk volume, no partition table)":"written",out[0]);
  if(out[0]!=0xDEAD && out[0]<16 && out[0]>0) for(unsigned i=0;i<out[0];i++) printf("   partition %u: start=%u size=%u\n",i,out[1+i*2],out[2+i*2]);
  return 0;
}
