/* SPDX-License-Identifier: MIT */
#include "uf2_draft.h"
#include <string.h>
static void p16(uint8_t *p,unsigned n){ p[0]=n; p[1]=n>>8; }
static void p32(uint8_t *p,uint32_t n){ p16(p,n); p16(p+2,n>>16); }
#ifdef MINEW_MCUBOOT
static const char info[]="UF2 Bootloader 0.1 experimental\r\nModel: Minew MX54BE13 nRF54LM20A\r\nBoard-ID: MINEW-MX54BE13-EXPERIMENTAL\r\nCopy a signed, slot-linked UF2 update. MCUboot validates before boot.\r\n";
#else
static const char info[]="UF2 Bootloader draft\r\nModel: Minew MX54BE13 nRF54LM20A\r\nBoard-ID: MINEW-MX54BE13-DRAFT\r\nVALIDATION ONLY: no firmware is written or activated.\r\n";
#endif
void uf2_disk_read(uint32_t sector,uint8_t b[512]) {
 memset(b,0,512);
 if(sector==0) {
  b[0]=0xeb;b[1]=0x3c;b[2]=0x90;memcpy(b+3,"MSDOS5.0",8);
  p16(b+11,512);b[13]=1;p16(b+14,1);b[16]=2;p16(b+17,64);
  p16(b+19,UF2_DISK_SECTORS);b[21]=0xf8;p16(b+22,32);
  p16(b+24,32);p16(b+26,64);b[36]=0x80;b[38]=0x29;
  p32(b+39,0x54323041);memcpy(b+43,"MINEWBOOT  ",11);memcpy(b+54,"FAT16   ",8);
  b[510]=0x55;b[511]=0xaa;
 } else if(sector==1 || sector==33) {
  p16(b,0xfff8);p16(b+2,0xffff);p16(b+4,0xffff);
 } else if(sector==65) {
  memcpy(b,"MINEWBOOT  ",11);b[11]=8;
  memcpy(b+32,"INFO_UF2TXT",11);b[43]=0x21;
  p16(b+58,2);p32(b+60,sizeof(info)-1);
 } else if(sector==69) memcpy(b,info,sizeof(info)-1);
}
