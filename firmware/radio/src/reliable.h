/* SPDX-License-Identifier: MIT */
#ifndef MINEW_RELIABLE_H
#define MINEW_RELIABLE_H
#include "frame.h"
#include <string.h>
#ifndef ARQ_BATCH
#if defined(CONFIG_MINEW_RADIO_SECURE)
#define ARQ_BATCH 10
#else
#define ARQ_BATCH 8
#endif
#endif
#ifndef ARQ_WIRE_VERSION
#if defined(CONFIG_MINEW_RADIO_SECURE)
#if defined(CONFIG_MINEW_RADIO_KEYBOARD)
#define ARQ_WIRE_VERSION 5
#else
#define ARQ_WIRE_VERSION 4
#endif
#else
#define ARQ_WIRE_VERSION 2
#endif
#endif
#if ARQ_WIRE_VERSION >= 4
#define ARQ_WINDOW 4
#else
#define ARQ_WINDOW 1
#endif
#define ARQ_HEADER 16
#define ARQ_SAMPLE_BYTES 20
#define ARQ_MAX_PAYLOAD (ARQ_HEADER + ARQ_BATCH * ARQ_SAMPLE_BYTES)
#define ARQ_TX_DWELL_MS 90
enum {
 LS_LOCAL_EPOCH, LS_REMOTE_EPOCH, LS_STATE, LS_ACK_SENT, LS_DATA_PACKETS,
 LS_RETRIES, LS_BUSY, LS_CHANNEL, LS_HOPS, LS_INJECT_DATA, LS_INJECT_ACK,
 LS_RESTARTS, LS_BACKPRESSURE, LS_TIMEOUTS, LS_DISCARDED, LS_QUEUE_OVERFLOW,
 LS_SAMPLES, LS_MAX_QUEUE, LS_MAX_ACK_US, LS_DATA_SENT, LS_ACK_RECEIVED,
 LS_LINK_LOSS, LS_SAMPLE_LATE, LS_RESERVED, LS_COUNT
};
enum { ARQ_DATA = 1, ARQ_ACK = 2, ARQ_HELLO = 3, ARQ_POLL = 4 };
#define ARQ_ACK_REQUEST 1
enum { ARQ_COMMITTED = 0, ARQ_BUSY = 1, ARQ_RESET = 2, ARQ_READY = 3 };
enum { ARQ_EXPECTED, ARQ_DUPLICATE, ARQ_WRONG_SEQUENCE };
static inline void arq_header(uint8_t *p, uint8_t type, uint32_t epoch,
                              uint32_t seq, uint8_t count, uint8_t status)
{
 memset(p, 0, ARQ_HEADER); p[0] = 'R'; p[1] = '8'; p[2] = ARQ_WIRE_VERSION; p[3] = type;
 frame_put32(p + 4, epoch); frame_put32(p + 8, seq); p[12] = count; p[13] = status;
}
static inline bool arq_valid(const uint8_t *p, size_t len)
{
 if (len < ARQ_HEADER || len > ARQ_MAX_PAYLOAD || p[0] != 'R' || p[1] != '8' ||
     p[2] != ARQ_WIRE_VERSION || !frame_get32(p + 4) || p[14] || p[15]) return false;
 if (p[3] == ARQ_HELLO) return len == ARQ_HEADER && !p[12] && !p[13] && !frame_get32(p + 8);
 if (ARQ_WIRE_VERSION >= 4 && p[3] == ARQ_POLL) return len == ARQ_HEADER && !p[12] && !p[13];
 if (p[3] == ARQ_ACK) return len == 20 && (ARQ_WIRE_VERSION >= 4 ? !p[12] : p[12] <= ARQ_BATCH) && p[13] <= ARQ_READY && frame_get32(p + 16);
 if (p[3] != ARQ_DATA || !p[12] || p[12] > ARQ_BATCH || (ARQ_WIRE_VERSION >= 4 && p[12] != ARQ_BATCH) || (p[13] & ~(ARQ_WIRE_VERSION >= 4 ? ARQ_ACK_REQUEST : 0)) ||
     len != ARQ_HEADER + (size_t)p[12] * ARQ_SAMPLE_BYTES) return false;
 for (unsigned i = 0; i < p[12]; i++) {
  const uint8_t *sample = p + ARQ_HEADER + i * ARQ_SAMPLE_BYTES;
  uint32_t seq = frame_get32(p + 8) + i;
#if defined(CONFIG_MINEW_RADIO_KEYBOARD)
  (void)seq;
  if (!rf_keyboard_payload_valid(sample)) return false;
#else
  for (unsigned j = 0; j < ARQ_SAMPLE_BYTES; j++) {
   if (sample[j] != (uint8_t)(seq + (j + 8) * 17)) return false;
  }
#endif
 }
 return true;
}
/* Shared constant metadata is encoded once. All 20 sample data bytes remain
 * opaque on transmission and are copied back, never regenerated from sequence. */
static inline size_t arq_pack_data(uint8_t *p, const uint8_t *frames, uint8_t count)
{
 if (!count || count > ARQ_BATCH || (ARQ_WIRE_VERSION >= 4 && count != ARQ_BATCH)) return 0;
 uint32_t epoch = frame_get32(frames + 28), seq = frame_get32(frames + 4);
 for (unsigned i = 0; i < count; i++) {
  const uint8_t *f = frames + i * FRAME_SIZE;
  if (!frame_valid(f, FRAME_SIZE) || f[2] != (ARQ_WIRE_VERSION == 5 ? 3 : 2) || frame_get32(f + 4) != seq + i ||
      frame_get32(f + 28) != epoch) return 0;
 }
 arq_header(p, ARQ_DATA, epoch, seq, count, 0);
 for (unsigned i = 0; i < count; i++) memcpy(p + ARQ_HEADER + i * ARQ_SAMPLE_BYTES,
                                           frames + i * FRAME_SIZE + 8, ARQ_SAMPLE_BYTES);
 return ARQ_HEADER + (size_t)count * ARQ_SAMPLE_BYTES;
}
static inline bool arq_unpack_data(const uint8_t *p, size_t len, uint8_t *frames)
{
 if (!arq_valid(p, len) || p[3] != ARQ_DATA) return false;
 for (unsigned i = 0; i < p[12]; i++) {
  uint8_t *f = frames + i * FRAME_SIZE;
  f[0] = 'M'; f[1] = '8'; f[2] = ARQ_WIRE_VERSION == 5 ? 3 : 2; f[3] = 0;
  frame_put32(f + 4, frame_get32(p + 8) + i);
  memcpy(f + 8, p + ARQ_HEADER + i * ARQ_SAMPLE_BYTES, ARQ_SAMPLE_BYTES);
  frame_put32(f + 28, frame_get32(p + 4));
 }
 return true;
}
struct arq_receiver {
 uint32_t epoch, expected, last_base, retired[4];
 uint8_t last_count, retired_index, history_count;
};
struct arq_receive_window { uint32_t start, limit; bool packet_started; };
/* An idle-listen deadline must not truncate a frame whose address matched
 * before the deadline. Extend once, with a separate bounded packet budget. */
static inline bool arq_receive_expired(struct arq_receive_window *w, uint32_t now,
                                       bool address, uint32_t packet_limit)
{
 if (address && !w->packet_started) {
  w->packet_started = true; w->start = now; w->limit = packet_limit;
 }
 return (uint32_t)(now - w->start) >= w->limit;
}
/* HELLO establishes a new diagnostic epoch; old epochs cannot switch it back. */
static inline bool arq_begin(struct arq_receiver *r, uint32_t epoch)
{
 if (!epoch) return false;
 if (epoch == r->epoch) return true;
 for (unsigned i = 0; i < 4; i++) if (r->retired[i] == epoch) return false;
 if (r->epoch) r->retired[r->retired_index++ % 4] = r->epoch;
 r->epoch = epoch; r->expected = 0; r->last_count = r->history_count = 0;
 return true;
}
static inline int arq_classify(const struct arq_receiver *r, uint32_t base, uint8_t count)
{
 if (base == r->expected) return ARQ_EXPECTED;
 if (ARQ_WIRE_VERSION >= 4 && count == ARQ_BATCH && r->last_count) {
  uint32_t behind = r->expected - base;
  if (behind && behind <= (uint32_t)r->history_count * ARQ_BATCH && !(behind % ARQ_BATCH)) return ARQ_DUPLICATE;
 }
 if (r->last_count == count && r->last_count && r->last_base == base) return ARQ_DUPLICATE;
 return ARQ_WRONG_SEQUENCE;
}
/* Call only after all samples have been accepted into the USB delivery queue. */
static inline void arq_commit(struct arq_receiver *r, uint32_t base, uint8_t count)
{
 r->last_base = base; r->last_count = count; r->expected = base + count;
 if (r->history_count < ARQ_WINDOW) r->history_count++;
}
static inline int arq_window_progress(uint32_t first, uint32_t acknowledged)
{
 uint32_t advance = acknowledged-first;
 return advance <= ARQ_WINDOW * ARQ_BATCH && !(advance % ARQ_BATCH) ? (int)(advance/ARQ_BATCH) : -1;
}
#endif
