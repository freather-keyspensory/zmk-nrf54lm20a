/* SPDX-License-Identifier: MIT */
#include "uf2_draft.h"
#include <errno.h>
#include <stddef.h>
static uint32_t le32(const uint8_t *p) {
 return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint32_t crc32(const uint8_t *p, unsigned n) {
 uint32_t c=~0u;
 while(n--) { c^=*p++; for(unsigned j=0;j<8;j++) c=(c>>1)^(0xedb88320u & (0u-(c&1u))); }
 return ~c;
}
int uf2_accept(struct uf2_session *s, const uint8_t b[512]) {
 if(le32(b)!=0x0a324655 || le32(b+4)!=0x9e5d5157) return 0;
 if(le32(b+508)!=0x0ab16f30) return -EINVAL;
 /* Ignore other families completely, including their advertised block count. */
 if(!(le32(b+8)&0x2000) || le32(b+28)!=UF2_FAMILY) return 0;
 if(le32(b+8)!=0x2000 || le32(b+16)!=256) return -EINVAL;
 uint32_t addr=le32(b+12), n=le32(b+20), total=le32(b+24);
 if(!total || total>UF2_BLOCKS || n>=total) return -ERANGE;
 if(addr!=UF2_BASE+n*256u || addr>=UF2_LIMIT) return -ERANGE;
 if(s->total && s->total!=total) return -EINVAL;
 uint32_t crc=crc32(b+32,256);
 if(s->seen[n/8] & (1u<<(n%8))) return s->checksums[n]==crc ? 2 : -EILSEQ;
 s->total=total; s->checksums[n]=crc; s->seen[n/8]|=1u<<(n%8); s->received++;
 return 1;
}
