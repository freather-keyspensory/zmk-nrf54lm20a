/* SPDX-License-Identifier: MIT */
#ifndef MINEW_UF2_DRAFT_H
#define MINEW_UF2_DRAFT_H
#include <stdint.h>
#define UF2_BASE 0x20000u
#define UF2_LIMIT 0xf0000u
#define UF2_BLOCKS ((UF2_LIMIT-UF2_BASE)/256u)
/* Private experimental identifier; not an upstream registered family ID. */
#define UF2_FAMILY 0x9b7316e2u
struct uf2_session {
 uint32_t total, received;
 uint8_t seen[UF2_BLOCKS/8];
 uint32_t checksums[UF2_BLOCKS];
};
/* 0: ordinary disk data; 1: new block; 2: duplicate; negative: invalid.
 * This parser validates structure only. It never programs or authenticates images.
 */
int uf2_accept(struct uf2_session *s, const uint8_t block[512]);
void uf2_disk_read(uint32_t sector, uint8_t block[512]);
#define UF2_DISK_SECTORS 8192u
#endif
