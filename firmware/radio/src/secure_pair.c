/* SPDX-License-Identifier: MIT */
/* Local wired provisioning only. No record or key is returned by telemetry. */
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <mbedtls/platform_util.h>
#include <psa/crypto.h>
#include <string.h>
#include <errno.h>
#include "secure_pair.h"
#include "secure_dtls.h"

#define PAIR_NAME "minew/rf/pair"
volatile struct rf_pair_mailbox radio_pair_mailbox;
static uint8_t active[64];
static atomic_t paired, backend_ready, generation;
static uint32_t queued_token;
volatile uint32_t radio_pair_completed_token;
static int record_valid(const uint8_t *r)
{
 if (memcmp(r, "S8PK", 4) || sys_get_le32(r+4) != 1 ||
     sys_get_le32(r+56) != crc32_ieee(r, 56) || sys_get_le32(r+60)) return -EINVAL;
 bool id = false, secret = false;
 for (unsigned i = 8; i < 24; i++) id |= r[i] != 0;
 for (unsigned i = 24; i < 56; i++) secret |= r[i] != 0;
 return id && secret ? 0 : -EINVAL;
}
static int load_pair(const char *name, size_t len, settings_read_cb read, void *arg, void *param)
{
 uint8_t *record = param;
 if (strcmp(name, "pair")) return 0;
 if (len != 64 || read(arg, record, 64) != 64) return -EIO;
 return record_valid(record);
}
int rf_pair_init(void)
{
 int rc = settings_subsys_init(); if (rc) return rc;
 atomic_set(&backend_ready, true);
 uint8_t record[64] = {0};
 rc = settings_load_subtree_direct("minew/rf", load_pair, record);
 if (!rc && !record_valid(record)) { memcpy(active, record, 64); atomic_set(&paired, true); atomic_set(&generation, 1); }
 mbedtls_platform_zeroize(record, sizeof(record));
 radio_pair_mailbox.generation = atomic_get(&generation);
 /* Missing or corrupt credentials stay unpaired; no fabricated key or erase. */
 return rc;
}
bool rf_pair_is_ready(void) { return atomic_get(&paired); }
uint32_t rf_pair_generation(void) { return atomic_get(&generation); }
uint32_t rf_pair_id_crc(void) { return atomic_get(&paired) ? crc32_ieee(active+8, 16) : 0; }
bool rf_pair_snapshot(uint8_t id[16], uint8_t secret[32], uint32_t *version)
{
 k_sched_lock();
 bool available = atomic_get(&paired);
 if (available) { memcpy(id, active+8, 16); memcpy(secret, active+24, 32); *version = atomic_get(&generation); }
 k_sched_unlock(); return available;
}
int rf_pair_queue(const uint8_t record[64], bool replace, uint32_t token)
{
 int rc = record_valid(record); if (rc) return rc;
 if (!token) return -EINVAL;
 if (!atomic_get(&backend_ready)) return -EIO;
 if (radio_pair_mailbox.request || radio_pair_mailbox.response == 1) return -EBUSY;
 radio_pair_mailbox.response = 1; queued_token = token;
 for (unsigned i = 0; i < 64; i++) radio_pair_mailbox.record[i] = record[i];
 radio_pair_mailbox.request = replace ? RF_PAIR_REPLACE : RF_PAIR_SET;
 return 0;
}
struct digest { uint8_t hash[32]; uint32_t count; };
static int hash_existing(const char *name, size_t len, settings_read_cb read, void *arg, void *param)
{
 if (!strncmp(name, "minew/rf/", 9)) return 0;
 /* Bound the preservation audit. An unknown larger value blocks writes. */
 uint8_t value[2048], hash[32], header[8]; size_t written;
 struct digest *d = param;
 if (len > sizeof(value) || read(arg, value, len) != (ssize_t)len) return -EIO;
 sys_put_le32(strlen(name), header); sys_put_le32(len, header+4);
 psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
 k_mutex_lock(&radio_crypto_mutex, K_FOREVER);
 psa_status_t rc = psa_hash_setup(&op, PSA_ALG_SHA_256);
 if (!rc) rc = psa_hash_update(&op, header, sizeof(header));
 if (!rc) rc = psa_hash_update(&op, (const uint8_t *)name, strlen(name));
 if (!rc) rc = psa_hash_update(&op, value, len);
 if (!rc) rc = psa_hash_finish(&op, hash, sizeof(hash), &written);
 psa_hash_abort(&op); k_mutex_unlock(&radio_crypto_mutex);
 mbedtls_platform_zeroize(value, sizeof(value));
 if (rc || written != 32) return -EIO;
 /* Each current key is unique; XOR makes this independent of backend order. */
 for (unsigned i = 0; i < 32; i++) d->hash[i] ^= hash[i];
 d->count++; mbedtls_platform_zeroize(hash, sizeof(hash)); return 0;
}
void rf_pair_service(void)
{
 uint32_t request = radio_pair_mailbox.request;
 if (request != RF_PAIR_SET && request != RF_PAIR_REPLACE) return;
 radio_pair_mailbox.response = 1; radio_pair_mailbox.request = 0;
 uint32_t completed_token = queued_token; queued_token = 0;
 uint8_t record[64], verified[64] = {0};
 for (unsigned i = 0; i < 64; i++) { record[i] = radio_pair_mailbox.record[i]; radio_pair_mailbox.record[i] = 0; }
 int rc = record_valid(record);
 bool storage_attempted = false;
 if (!atomic_get(&backend_ready)) rc = -EIO;
 if (!rc && atomic_get(&paired) && memcmp(active, record, 64) && request != RF_PAIR_REPLACE) rc = -EEXIST;
 if (!rc && (!atomic_get(&paired) || memcmp(active, record, 64))) {
  struct digest before = {0}, after = {0};
  rc = settings_load_subtree_direct(NULL, hash_existing, &before);
  if (!rc) { storage_attempted = true; rc = settings_save_one(PAIR_NAME, record, sizeof(record)); }
  if (!rc) rc = settings_load_subtree_direct("minew/rf", load_pair, verified);
  if (!rc && memcmp(record, verified, 64)) rc = -EIO;
  if (!rc) rc = settings_load_subtree_direct(NULL, hash_existing, &after);
  if (!rc && memcmp(&before, &after, sizeof(before))) rc = -EIO;
  if (!rc) {
   memcpy(active, record, 64); atomic_set(&paired, true);
   if ((uint32_t)atomic_inc(&generation) == UINT32_MAX) atomic_set(&generation, 1);
  }
 }
 if (rc && storage_attempted) { atomic_set(&paired, false); mbedtls_platform_zeroize(active, sizeof(active)); atomic_inc(&generation); }
 mbedtls_platform_zeroize(record, sizeof(record)); mbedtls_platform_zeroize(verified, sizeof(verified));
 radio_pair_mailbox.generation = atomic_get(&generation);
 radio_pair_completed_token = completed_token;
 radio_pair_mailbox.response = rc ? (uint32_t)rc : 2;
}
