/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
extern int call_pf(uint32_t fmt, uint32_t hook);
static void *map(uint32_t at,size_t len){ void*p=mmap((void*)(uintptr_t)at,len,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(p==MAP_FAILED){perror("mmap");exit(3);} return p; }
int main(int argc,char**argv){
  uint32_t hook=strtoul(argv[1],0,0);
  map(0x08000000,0x1000); map(0x08119000,0x7000); map(0x0926B000,0x1000);
  memcpy((void*)0x08000740,"1.56",4);
  for(int i=2;i+1<argc;i+=2){ uint32_t a=strtoul(argv[i],0,0); FILE*bf=fopen(argv[i+1],"rb"); fseek(bf,0,SEEK_END); long n=ftell(bf); fseek(bf,0,SEEK_SET); if(fread((void*)(uintptr_t)a,1,n,bf)!=(size_t)n)return 5; }
  uint16_t fake[]={0x000B,0xE04D};                      /* the "real" print function: rts ; mov #77,r0 */
  memcpy((void*)0x0926B064,fake,4);
  struct {uint32_t fmt; const char*name;} t[]={{0xA808F350,"Device Type = USB Memory"},{0xA808F2C0,"Inquiry fail"},{0xA808F37C,"not support device"},{0xA808F464,"read capacity10 fail"},{0xA808E840,"det_func BOT"},{0xA808EEE8,"fs_get_partition failed"},{0xA808EF54,"ini_disk_sect failed"},{0xA808F068,"ct_mount_disk failed"},{0xA808F040,"refresh_cache failed"},{0xA8091234,"some other message"},{0x0808F350,"P0 alias of Device Type"}};
  for(unsigned i=0;i<sizeof t/sizeof t[0];i++){ int r=call_pf(t[i].fmt,hook); printf("%-28s -> ret=%d (77 = reached real print)  text=%.4s\n",t[i].name,r,(char*)0x08000740); }
  return 0;
}
