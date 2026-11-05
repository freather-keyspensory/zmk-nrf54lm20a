/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include "../src/reliable.h"
int main(void)
{
 uint8_t report[RF_KEYBOARD_REPORT_SIZE]={1,2,0,4,5,6,7,8,9};
 uint8_t consumer[RF_CONSUMER_REPORT_SIZE]={2,0xe9,0};
 uint8_t payload[20], frames[FRAME_SIZE*ARQ_BATCH], decoded[sizeof(frames)], packet[ARQ_MAX_PAYLOAD];
 assert(rf_keyboard_payload_make(payload,report,sizeof(report)));
 for (unsigned i=0;i<ARQ_BATCH;i++) {
  uint8_t *f=frames+i*FRAME_SIZE;
  frame_make_session(f,UINT32_MAX-5+i,123); f[2]=3;
  if (i&1) assert(rf_keyboard_payload_make(f+8,consumer,sizeof(consumer)));
  else assert(rf_keyboard_payload_make(f+8,report,sizeof(report)));
  assert(frame_valid(f,FRAME_SIZE));
 }
 size_t n=arq_pack_data(packet,frames,ARQ_BATCH); assert(n==ARQ_MAX_PAYLOAD && packet[2]==5);
 assert(arq_unpack_data(packet,n,decoded)); assert(!memcmp(decoded,frames,sizeof(frames)));
 packet[2]=4; assert(!arq_valid(packet,n));packet[2]=5;
 for (unsigned pos=0;pos<20;pos++) {
  uint8_t saved=packet[ARQ_HEADER+pos];packet[ARQ_HEADER+pos]=255;
  /* Modifier and key bytes are intentionally opaque; metadata/reserved and
   * zero padding must reject corruption instead of interpreting diagnostics. */
  if (pos==0 || pos==1 || pos==2 || pos==4 || pos>=11) assert(!arq_valid(packet,n));
  packet[ARQ_HEADER+pos]=saved;
 }
 assert(rf_keyboard_payload_make(payload,consumer,sizeof(consumer)));
 payload[4]=0x10; assert(!rf_keyboard_payload_valid(payload));
 assert(!rf_keyboard_payload_make(payload,report,19));
 frame_make_session(frames,0,123); assert(!arq_pack_data(packet,frames,ARQ_BATCH));
 return 0;
}
