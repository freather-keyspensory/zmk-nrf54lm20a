/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <string.h>
#include "../src/reliable.h"

static void data(uint8_t *p, uint32_t epoch, uint32_t seq, uint8_t count)
{
 uint8_t frames[ARQ_BATCH * FRAME_SIZE], decoded[ARQ_BATCH * FRAME_SIZE];
 for (unsigned i = 0; i < count; i++) frame_make_session(frames + i * FRAME_SIZE, seq + i, epoch);
 size_t size = arq_pack_data(p, frames, count);
 assert(size == ARQ_HEADER + (size_t)count * ARQ_SAMPLE_BYTES);
 assert(arq_unpack_data(p, size, decoded));
 assert(!memcmp(frames, decoded, (size_t)count * FRAME_SIZE));
}
int main(void)
{
 uint8_t p[ARQ_MAX_PAYLOAD];
 data(p, 123, 0, ARQ_BATCH);
 assert(arq_valid(p, sizeof(p)));
 for (size_t n = 0; n < sizeof(p); n++) assert(!arq_valid(p, n));
 assert(!arq_valid(p, sizeof(p) + 1));
 p[12] = ARQ_BATCH + 1; assert(!arq_valid(p, sizeof(p))); p[12] = ARQ_BATCH;
 p[14] = 1; assert(!arq_valid(p, sizeof(p))); p[14] = 0;
 p[ARQ_HEADER + FRAME_SIZE + 4] ^= 1;
 assert(!arq_valid(p, sizeof(p)));
 data(p, 123, UINT32_MAX - 3, ARQ_BATCH);
 assert(arq_valid(p, sizeof(p)));
 p[ARQ_HEADER + 28] ^= 1; assert(!arq_valid(p, sizeof(p)));
 arq_header(p, ARQ_ACK, 123, 7, 7, ARQ_COMMITTED);
 frame_put32(p + 16, 456); assert(arq_valid(p, 20));
 p[13] = 9; assert(!arq_valid(p, 20)); p[13] = ARQ_COMMITTED;
 frame_put32(p + 16, 0); assert(!arq_valid(p, 20));
 arq_header(p, ARQ_HELLO, 123, 0, 0, 0); assert(arq_valid(p, 16));
 p[12] = 1; assert(!arq_valid(p, 16));
 arq_header(p, ARQ_HELLO, 0, 0, 0, 0); assert(!arq_valid(p, 16));

 struct arq_receiver r = {0};
 assert(!arq_begin(&r, 0)); assert(arq_begin(&r, 123));
 unsigned delivered = 0, retries = 0;
 /* Deterministic data/ACK loss and USB backpressure. Commit only accepted
  * batches; retries after a lost ACK must never redeliver an accepted sample. */
 for (uint32_t base = 0; base < ARQ_BATCH * 1000; base += ARQ_BATCH) {
  bool accepted = false, acknowledged = false;
  unsigned attempt = 0;
  while (!acknowledged) {
   attempt++; retries += attempt > 1;
   if (attempt == 1 && base % 3 == 0) continue; /* DATA lost */
   int state = arq_classify(&r, base, ARQ_BATCH);
   if (state == ARQ_EXPECTED) {
    if (attempt == 1 && base % 5 == 0) continue; /* USB busy */
    assert(!accepted); accepted = true;
    arq_commit(&r, base, ARQ_BATCH); delivered += ARQ_BATCH;
   } else {
    assert(state == ARQ_DUPLICATE && accepted);
   }
   if (attempt == 1 && base % 7 == 0) continue; /* ACK lost */
   acknowledged = true;
  }
 }
 assert(delivered == ARQ_BATCH * 1000 && retries > 0 && r.expected == ARQ_BATCH * 1000);
 assert(arq_begin(&r, 123) && r.expected == ARQ_BATCH * 1000); /* repeated HELLO */
 assert(arq_begin(&r, 456) && r.expected == 0);   /* TX restart */
 assert(!arq_begin(&r, 123) && r.epoch == 456);   /* delayed old HELLO */
 assert(arq_classify(&r, 7, 7) == ARQ_WRONG_SEQUENCE);
 r.expected = UINT32_MAX - 3;
 arq_commit(&r, r.expected, ARQ_BATCH); assert(r.expected == ARQ_BATCH - 4);
 assert(arq_classify(&r, UINT32_MAX - 3, ARQ_BATCH) == ARQ_DUPLICATE);
 struct arq_receiver rebooted = {0};
 assert(arq_classify(&rebooted, 100, 7) == ARQ_WRONG_SEQUENCE);
 assert(arq_begin(&rebooted, 789) && rebooted.expected == 0);
 struct arq_receive_window w = {0, 2000, false};
 assert(!arq_receive_expired(&w, 1900, true, 1000));
 assert(!arq_receive_expired(&w, 2200, true, 1000)); /* past old idle deadline */
 assert(!arq_receive_expired(&w, 2899, true, 1000));
 assert(arq_receive_expired(&w, 2900, true, 1000)); /* ADDRESS cannot extend forever */
 w = (struct arq_receive_window){UINT32_MAX - 99, 200, false};
 assert(!arq_receive_expired(&w, 50, false, 1000));
 assert(arq_receive_expired(&w, 100, false, 1000));
 return 0;
}
