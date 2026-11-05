/* SPDX-License-Identifier: MIT */
/* PUBLIC TEST KEYS ONLY. This measures PSA; RF remains the unencrypted ARQv2. */
#include <zephyr/kernel.h>
#include <psa/crypto.h>
#include <string.h>
#include <errno.h>

/* magic, ABI, status(1 running/2 passed/3 failed), stage, PSA error, iterations,
 * then two groups of six: encrypt176 ns, decrypt176 ns, encrypt20 ns,
 * decrypt20 ns, tamper mask, known-answer passed (AES-CCM, then ChaChaPoly).
 * reserved[6]. Resolve from the exact flashed ELF. */
volatile uint32_t radio_crypto_probe_results[24];
static const uint8_t aad[] = "Minew secure RF v1";
static const uint8_t expected[] =
 "\x16\x35\xb6\x8b\x57\x0c\xfc\x85\x52\x9e\x39\xac\x91\x39\x10\xd7\xf3\x11\x16\x31\x62\x38\x67\xf1\x34\xe6\xe4\x41\x90\x4f\xd5\x04"
 "\xf5\x74\x6d\x6b\xf1\x89\x81\x5f\x51\xa4\x8f\x07\xc9\x94\xed\x9e\xee\x24\xe8\xd2\xea\xda\x7e\x42\x18\xb5\x88\xf6\x7a\x3a\xf1\xa3"
 "\x8d\xfe\xed\x70\xab\x5a\x15\x36\x28\x34\x0b\xa1\xf9\xf1\xee\x95\xc3\x2e\xa4\xf7\xa7\x98\x2f\x12\xee\xcc\x80\xb0\x3f\x0f\x2b\x66"
 "\x20\xcf\xa6\xc9\x47\x03\x89\xaf\xee\x56\xa5\x2b\x2f\x39\xc0\xc9\x48\xf5\x9c\x3d\xb0\x2e\x68\xdc\x18\x15\x52\x24\x0c\x47\x71\x42"
 "\x25\xf5\x67\x07\xfa\x4a\x96\xe2\x3e\x89\x70\xc9\x96\x1f\xdb\x4c\xe4\x73\x1e\xe7\xb0\xd2\x96\x98\xb7\x2d\xd1\x20\x15\x2d\x52\xfc"
 "\x18\x4d\x46\x02\x3c\x6d\x20\xfb\x6b\x6a\x44\xff\xf9\x78\xd7\x45\x52\xf8\x87\xb7\xee\xd5\x11\xcf\xdb\xaf\xfb\x8e\xcf\x72\xc3\xd5";
static const uint8_t chacha_expected[] =
 "\x89\xfa\x0a\x03\x2d\x12\xa3\x47\xbf\x8a\x35\xf8\x94\x10\x00\x6c\xd9\x61\xa0\xf4\x45\x61\xbb\xae\xfe\x8e\x35\xde\x69\xdd\xb8\x23"
 "\xcc\xa1\x0e\xd0\xc2\x3b\x97\xbf\x1f\x1b\x5c\xf3\x49\xb9\xa1\x0c\x4e\xb5\x9b\x47\xc9\x1d\x8e\xac\x2a\x81\xe3\x3c\xac\x72\xa0\xe9"
 "\x39\x39\xfe\x8e\xa1\x51\x6a\xae\x8c\x5f\x07\xf7\x54\x31\x92\xbe\x8a\x8f\x15\x61\x3b\x3f\xa6\x69\x56\x0e\xaa\x55\x84\x20\x5c\xe0"
 "\x2d\xbf\x0e\x90\x93\xbc\x41\x93\xd9\x32\x99\xdc\xce\xfc\xd9\xef\x99\x1e\x24\x4b\xfd\x28\x36\x8f\x37\x14\x4e\xa4\x05\x42\xc1\x39"
 "\x21\xe7\x41\x3e\x2d\xb6\x59\xeb\xc1\x9a\xf5\xf1\xa0\xb1\xe5\xb5\x24\xe0\x15\xe1\x9f\xfa\x5a\xc9\xaa\x78\x3d\xaa\x38\x86\x3e\xa6"
 "\x9f\x18\x23\x4b\x19\xe1\x32\x41\x26\xd7\xcc\xbb\xcb\x1c\x41\xaf\x19\x90\x1f\xc4\x06\x0b\xb4\xc7\xc2\xbc\x0a\xcf\x7b\x1c\x46\x38";
static psa_algorithm_t algorithm;
static size_t nonce_length;
static uint8_t key_bytes[32], nonce[13], plain[176], cipher[192], decoded[176];
static mbedtls_svc_key_id_t key;
#define CHECK(call) do { psa_status_t rc = (call); if (rc != PSA_SUCCESS) { radio_crypto_probe_results[4] = (uint32_t)rc; goto fail; } } while (0)

static psa_status_t seal(size_t n)
{
 size_t used;
 psa_status_t rc = psa_aead_encrypt(key, algorithm, nonce, nonce_length,
   aad, sizeof(aad)-1, plain, n, cipher, sizeof(cipher), &used);
 return rc ? rc : used == n + 16 ? PSA_SUCCESS : PSA_ERROR_CORRUPTION_DETECTED;
}
static psa_status_t open_packet(mbedtls_svc_key_id_t k, size_t n, const uint8_t *a)
{
 size_t used;
 psa_status_t rc = psa_aead_decrypt(k, algorithm, nonce, nonce_length,
   a, sizeof(aad)-1, cipher, n+16, decoded, sizeof(decoded), &used);
 return rc ? rc : used == n ? PSA_SUCCESS : PSA_ERROR_CORRUPTION_DETECTED;
}
static psa_status_t measure(bool encrypting, size_t n, unsigned result)
{
 nonce[0] = (uint8_t)n;
 CHECK(seal(n));
 uint64_t start = k_cycle_get_64();
 for (unsigned i = 0; i < 1000; i++) {
  if (encrypting) {
   for (unsigned j = 0; j < 8; j++) nonce[nonce_length-8+j] = ((uint64_t)i + 1) >> (8*j);
   CHECK(seal(n));
  } else CHECK(open_packet(key, n, aad));
 }
 radio_crypto_probe_results[result] = k_cyc_to_ns_floor64(k_cycle_get_64()-start) / 1000;
 return PSA_SUCCESS;
fail:
 return (psa_status_t)radio_crypto_probe_results[4];
}
static int probe_cipher(bool chacha, unsigned base)
{
 algorithm = chacha ? PSA_ALG_CHACHA20_POLY1305 : PSA_ALG_CCM;
 nonce_length = chacha ? 12 : 13;
 size_t key_length = chacha ? 32 : 16;
 mbedtls_svc_key_id_t wrong_key = 0;
 psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
 radio_crypto_probe_results[3] = chacha ? 7 : 1;
 for (unsigned i = 0; i < sizeof(key_bytes); i++) key_bytes[i] = i;
 for (unsigned i = 0; i < sizeof(nonce); i++) nonce[i] = i;
 for (unsigned i = 0; i < sizeof(plain); i++) plain[i] = i;
 CHECK(psa_crypto_init());
 radio_crypto_probe_results[3] = 2;
 psa_set_key_type(&attributes, chacha ? PSA_KEY_TYPE_CHACHA20 : PSA_KEY_TYPE_AES); psa_set_key_bits(&attributes, key_length * 8);
 psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
 psa_set_key_algorithm(&attributes, algorithm);
 CHECK(psa_import_key(&attributes, key_bytes, key_length, &key));
 key_bytes[0] ^= 1;
 CHECK(psa_import_key(&attributes, key_bytes, key_length, &wrong_key));
 psa_reset_key_attributes(&attributes);
 radio_crypto_probe_results[3] = 3;
 CHECK(seal(sizeof(plain)));
 if (memcmp(chacha ? chacha_expected : expected, cipher, sizeof(cipher))) goto fail;
 CHECK(open_packet(key, sizeof(plain), aad));
 if (memcmp(plain, decoded, sizeof(plain))) goto fail;
 radio_crypto_probe_results[base+5] = 1;
 radio_crypto_probe_results[3] = 4;
 cipher[0] ^= 1;
 if (open_packet(key, sizeof(plain), aad) != PSA_ERROR_INVALID_SIGNATURE) goto fail;
 cipher[0] ^= 1; radio_crypto_probe_results[base+4] |= 1;
 cipher[sizeof(cipher)-1] ^= 1;
 if (open_packet(key, sizeof(plain), aad) != PSA_ERROR_INVALID_SIGNATURE) goto fail;
 cipher[sizeof(cipher)-1] ^= 1; radio_crypto_probe_results[base+4] |= 2;
 uint8_t wrong_aad[sizeof(aad)]; memcpy(wrong_aad, aad, sizeof(aad)); wrong_aad[0] ^= 1;
 if (open_packet(key, sizeof(plain), wrong_aad) != PSA_ERROR_INVALID_SIGNATURE) goto fail;
 radio_crypto_probe_results[base+4] |= 4; nonce[0] ^= 1;
 if (open_packet(key, sizeof(plain), aad) != PSA_ERROR_INVALID_SIGNATURE) goto fail;
 nonce[0] ^= 1; radio_crypto_probe_results[base+4] |= 8;
 if (open_packet(wrong_key, sizeof(plain), aad) != PSA_ERROR_INVALID_SIGNATURE) goto fail;
 radio_crypto_probe_results[base+4] |= 16;
 radio_crypto_probe_results[3] = 5; radio_crypto_probe_results[5] = 1000;
 CHECK(measure(true, 176, base)); CHECK(measure(false, 176, base+1));
 CHECK(measure(true, 20, base+2)); CHECK(measure(false, 20, base+3));
 CHECK(psa_destroy_key(wrong_key)); wrong_key = 0;
 CHECK(psa_destroy_key(key)); key = 0;
 return 0;
fail:
 if (wrong_key) psa_destroy_key(wrong_key);
 if (key) psa_destroy_key(key);
 psa_reset_key_attributes(&attributes);
 radio_crypto_probe_results[2] = 3;
 return -EIO;
}


int radio_crypto_probe(void)
{
 memset((void *)radio_crypto_probe_results, 0, sizeof(radio_crypto_probe_results));
 radio_crypto_probe_results[0] = 0x50433853; radio_crypto_probe_results[1] = 2;
 radio_crypto_probe_results[2] = 1;
 if (probe_cipher(false, 6) || probe_cipher(true, 12)) return -EIO;
 radio_crypto_probe_results[3] = 8; radio_crypto_probe_results[2] = 2;
 printk("PUBLIC-vector PSA AEAD probes passed; following RF is still UNENCRYPTED\n");
 return 0;
}
