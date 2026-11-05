/* SPDX-License-Identifier: MIT */
#ifndef MINEW_SECURE_RECORD_H
#define MINEW_SECURE_RECORD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Framing only, never authentication. Reject empty/truncated datagrams before
 * BIO can mistake zero bytes for EOF. Inspect every coalesced record so an
 * appended clear ClientHello cannot bypass the established-session gate. */
static inline bool rf_dtls_framing(const uint8_t *p, size_t length, bool reject_clear)
{
 if (length < 13 || length > 255) return false;
 size_t offset = 0;
 while (offset < length) {
  if (length-offset < 13) return false;
  const uint8_t *r = p+offset;
  bool clear = r[3] == 0 && r[4] == 0;
  if (r[0] < 20 || r[0] > 23 || r[1] != 0xfe ||
      (r[2] != 0xfd && !(clear && r[2] == 0xff)) || (clear && reject_clear)) return false;
  size_t n = ((size_t)r[11] << 8) | r[12];
  if (!n || n > length-offset-13) return false;
  offset += 13+n;
 }
 return offset == length;
}
#endif
