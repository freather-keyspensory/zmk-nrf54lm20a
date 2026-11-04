/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <string.h>
#include "../src/frame.h"
int main(void)
{
 uint8_t frame[FRAME_SIZE];
 const uint32_t sequences[] = {0, 1, 255, 256, 0x12345678, UINT32_MAX};
 for (unsigned s = 0; s < sizeof(sequences) / sizeof(sequences[0]); s++) {
  frame_make(frame, sequences[s]);
  assert(frame_get32(frame + 4) == sequences[s]);
  assert(frame_valid(frame, sizeof(frame)));
  for (size_t len = 0; len < FRAME_SIZE; len++) assert(!frame_valid(frame, len));
  assert(!frame_valid(frame, FRAME_SIZE + 1));
  for (unsigned i = 0; i < FRAME_SIZE; i++) {
   /* Sequence high bytes are deliberately not authenticated by this codec. */
   if (i >= 5 && i <= 7) continue;
   frame[i] ^= 1; assert(!frame_valid(frame, sizeof(frame))); frame[i] ^= 1;
  }
 }
 assert(frame_forward_delta(10, 10) == 0);
 assert(frame_forward_delta(10, 9) == 0);
 assert(frame_forward_delta(10, 15) == 5);
 assert(frame_forward_delta(UINT32_MAX, 0) == 1);
 assert(frame_forward_delta(UINT32_MAX - 1, 1) == 3);
 assert(frame_forward_delta(0, UINT32_C(0x80000000)) == 0);
 return 0;
}
