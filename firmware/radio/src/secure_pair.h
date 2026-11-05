/* SPDX-License-Identifier: MIT */
#ifndef MINEW_SECURE_PAIR_H
#define MINEW_SECURE_PAIR_H
#include <stdint.h>
#include <stdbool.h>
#define RF_PAIR_RECORD_SIZE 64
#define RF_PAIR_SET 0x31503853u
#define RF_PAIR_REPLACE 0x32503853u
struct rf_pair_mailbox {
 volatile uint32_t request, response;
 volatile uint8_t record[RF_PAIR_RECORD_SIZE];
 volatile uint32_t generation, reserved;
};
extern volatile struct rf_pair_mailbox radio_pair_mailbox;
int rf_pair_init(void);
void rf_pair_service(void);
extern volatile uint32_t radio_pair_completed_token;
int rf_pair_queue(const uint8_t record[64], bool replace, uint32_t token);
bool rf_pair_snapshot(uint8_t id[16], uint8_t secret[32], uint32_t *generation);
uint32_t rf_pair_generation(void);
uint32_t rf_pair_id_crc(void);
bool rf_pair_is_ready(void);
#endif
