/* SPDX-License-Identifier: MIT */
#ifndef MINEW_KEYBOARD_FRAME_H
#define MINEW_KEYBOARD_FRAME_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
/* Exact pinned ZMK baseline: ID 1 = modifiers/reserved/six keys;
 * ID 2 = six little-endian 12-bit consumer usages. */
#define RF_KEYBOARD_REPORT_SIZE 9
#define RF_CONSUMER_REPORT_SIZE 13
#define RF_REPORT_MAX 18
#define RF_REPORT_PAYLOAD 20
static inline bool rf_keyboard_payload_valid(const uint8_t *p)
{
 unsigned n = p[0];
 if (p[1] || n > RF_REPORT_MAX) return false;
 if (p[2] == 1) {
  if (n != RF_KEYBOARD_REPORT_SIZE || p[4]) return false;
 } else if (p[2] == 2) {
  if (n != RF_CONSUMER_REPORT_SIZE) return false;
  for (unsigned i = 4; i < 2+n; i += 2) if (p[i] & 0xf0) return false;
 } else return false;
 for (unsigned i = 2+n; i < RF_REPORT_PAYLOAD; i++) if (p[i]) return false;
 return true;
}
static inline bool rf_keyboard_payload_make(uint8_t *p, const uint8_t *report, size_t n)
{
 if (n > RF_REPORT_MAX) return false;
 memset(p, 0, RF_REPORT_PAYLOAD); p[0] = n; memcpy(p+2, report, n);
 return rf_keyboard_payload_valid(p);
}
#endif
