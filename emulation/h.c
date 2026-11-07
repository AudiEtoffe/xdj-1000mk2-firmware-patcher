/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
extern void finish(void);
extern int run_flac(uint32_t entry, void *st, uint32_t r5val);
extern int run_wav(uint32_t entry, void *st, uint32_t r10val, uint32_t mode);
static void put(uint32_t a, const uint16_t *w, int n){ memcpy((void*)a,w,2*n); }
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); fseek(f,0x1386000,SEEK_SET);
  void*m=mmap((void*)0x09386000,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(m==MAP_FAILED){perror("mmap");return 2;}
  if(fread(m,1,0x2000,f)!=0x2000){puts("short");return 2;}
  uint32_t fin=(uint32_t)(uintptr_t)finish;
  /* each stub: <mov #v,r0> mov.l @(1,pc),r1 ; jmp @r1 ; <delay> ; .long finish */
  uint16_t ok[]  ={0xE001,0xD101,0x412B,0x0009}, er0[]={0xE000,0xD101,0x412B,0x0009}, erf[]={0xD101,0x412B,0x6023}, ret[]={0x000B,0x0009};
  put(0x09386490,ok,4);  memcpy((void*)0x09386498,&fin,4);
  put(0x093865A2,erf,3); memcpy((void*)0x093865A8,&fin,4);
  put(0x09387E58,ret,2);
  put(0x09386D50,ok,4);  memcpy((void*)0x09386D58,&fin,4);
  put(0x09387608,er0,4); memcpy((void*)0x09387610,&fin,4);
  uint32_t rates[]={8000,11025,16000,22050,32000,44100,48000,64000,88200,96000,88201,176400,192000,0,50000};
  uint32_t st[64];
  printf("rate     FLAC   WAV(normal)  WAV(DISC)\n");
  for(unsigned i=0;i<sizeof rates/4;i++){
    memset(st,0,sizeof st); st[21]=rates[i];
    int fl=run_flac(0x09386456,st,44100);
    memset(st,0,sizeof st); st[21]=rates[i];
    int w0=run_wav(0x09386CFE,st,44100,0);
    memset(st,0,sizeof st); st[21]=rates[i];
    int w1=run_wav(0x09386CFE,st,44100,1);
    printf("%-8u %-6s %-12s %-6s\n",rates[i],fl==1?"accept":"reject",w0==1?"accept":"reject",w1==1?"accept":"reject");
  }
  return 0;
}
