/* SPDX-License-Identifier: MIT */
#include "uf2_draft.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void put(uint8_t *p,uint32_t x){for(unsigned i=0;i<4;i++)p[i]=x>>(8*i);}
static void block(uint8_t *b,unsigned n,unsigned total){
 memset(b,0,512);put(b,0x0a324655);put(b+4,0x9e5d5157);put(b+8,0x2000);
 put(b+12,UF2_BASE+256*n);put(b+16,256);put(b+20,n);put(b+24,total);
 put(b+28,UF2_FAMILY);put(b+508,0x0ab16f30);
}
int main(int argc,char **argv){
 if(argc==3){
  struct uf2_session s={0};uint8_t b[512];FILE *f=fopen(argv[1],"rb");assert(f);
  while(fread(b,512,1,f)==1){assert(uf2_accept(&s,b)==1);assert(uf2_accept(&s,b)==2);}
  assert(feof(f));assert(s.total && s.total==s.received);assert(fclose(f)==0);
  printf("Full signed UF2: %u unique blocks accepted\n",s.received);return 0;
 }
 if(argc==2){uint8_t sector[512];FILE *f=fopen(argv[1],"wb");assert(f);
 for(unsigned i=0;i<UF2_DISK_SECTORS;i++){uf2_disk_read(i,sector);assert(fwrite(sector,512,1,f)==1);}
 assert(fclose(f)==0);return 0;}
 struct uf2_session s={0};uint8_t b[512];
 block(b,1,2);assert(uf2_accept(&s,b)==1);assert(s.received==1);
 assert(uf2_accept(&s,b)==2);assert(s.received==1);
 b[32]=1;assert(uf2_accept(&s,b)<0);assert(s.received==1);
 block(b,0,2);assert(uf2_accept(&s,b)==1);assert(s.received==2);
 block(b,0,3);assert(uf2_accept(&s,b)<0);
 memset(&s,0,sizeof(s));block(b,0,0);assert(uf2_accept(&s,b)<0);
 block(b,0,UF2_BLOCKS+1);assert(uf2_accept(&s,b)<0);
 block(b,2,2);assert(uf2_accept(&s,b)<0);
 block(b,0,1);put(b+12,0);assert(uf2_accept(&s,b)<0);
 put(b+12,0xfffffff0);assert(uf2_accept(&s,b)<0);
 block(b,0,1);put(b+16,476);assert(uf2_accept(&s,b)<0);
 block(b,0,1);put(b+8,0x2001);assert(uf2_accept(&s,b)<0);
 block(b,0,1);put(b+28,UF2_FAMILY+1);assert(uf2_accept(&s,b)==0);assert(!s.total);
 block(b,0,1);b[508]=0;assert(uf2_accept(&s,b)<0);
 memset(b,0,512);assert(uf2_accept(&s,b)==0);
 uf2_disk_read(0,b);assert(b[510]==0x55 && b[511]==0xaa);
 assert(b[11]==0 && b[12]==2 && b[16]==2 && b[22]==32);
 uf2_disk_read(65,b);assert(!memcmp(b+32,"INFO_UF2TXT",11));
 uf2_disk_read(69,b);assert(strstr((char*)b,"VALIDATION ONLY"));
 puts("UF2 ordering, duplicates, malformed input, bounds, and FAT geometry passed");
}
