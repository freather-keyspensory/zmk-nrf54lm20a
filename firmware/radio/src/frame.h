/* SPDX-License-Identifier: MIT */
#ifndef MINEW_RADIO_FRAME_H
#define MINEW_RADIO_FRAME_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#if defined(CONFIG_MINEW_RADIO_KEYBOARD)
#include "keyboard_frame.h"
#endif
/* Diagnostic only: fixed 32-byte payload, explicitly little endian. */
#define FRAME_SIZE 32
/* 0: duplicate/backward; positive: forward distance (wrap-aware). */
static inline uint32_t frame_forward_delta(uint32_t previous, uint32_t current)
{
 uint32_t delta = current - previous;
 return delta < UINT32_C(0x80000000) ? delta : 0;
}
static inline void frame_put32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static inline uint32_t frame_get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void frame_make(uint8_t *p, uint32_t seq)
{
 p[0] = 'M'; p[1] = '8'; p[2] = 1; p[3] = 0;
 frame_put32(p + 4, seq);
 for (unsigned i = 8; i < FRAME_SIZE; i++) p[i] = (uint8_t)(seq + i * 17);
}
static inline bool frame_valid(const uint8_t *p, size_t len)
{
 #if defined(CONFIG_MINEW_RADIO_KEYBOARD)
 if (len == FRAME_SIZE && p[0] == 'M' && p[1] == '8' && p[2] == 3 && !p[3])
  return frame_get32(p+28) && rf_keyboard_payload_valid(p+8);
#endif
 if (len != FRAME_SIZE || p[0] != 'M' || p[1] != '8' || (p[2] != 1 && p[2] != 2) || p[3]) return false;
 uint32_t seq = frame_get32(p + 4);
 unsigned end = p[2] == 2 ? 28 : FRAME_SIZE;
 if (p[2] == 2 && !frame_get32(p + 28)) return false;
 for (unsigned i = 8; i < end; i++) if (p[i] != (uint8_t)(seq + i * 17)) return false;
 return true;
}
static inline void frame_make_session(uint8_t *p, uint32_t seq, uint32_t epoch)
{
 frame_make(p, seq); p[2] = 2; frame_put32(p + 28, epoch);
}
#endif
