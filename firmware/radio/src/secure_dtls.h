/* SPDX-License-Identifier: MIT */
#ifndef MINEW_SECURE_DTLS_H
#define MINEW_SECURE_DTLS_H
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cookie.h>
#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
extern struct k_mutex radio_crypto_mutex;

#define RF_DTLS_MTU 255
#define RF_PAIR_ID_BYTES 16
#define RF_PAIR_KEY_BYTES 32
struct rf_dtls {
 mbedtls_ssl_context ssl;
 mbedtls_ssl_config conf;
 mbedtls_ssl_cookie_ctx cookie;
 int64_t timer_start;
 uint32_t intermediate, final;
 uint8_t identity[RF_PAIR_ID_BYTES + 12];
 bool server, initialized;
};
int rf_dtls_init(struct rf_dtls *s, bool server, const uint8_t id[16], const uint8_t key[32],
                 void *io, mbedtls_ssl_send_t *send, mbedtls_ssl_recv_t *receive);
int rf_dtls_handshake(struct rf_dtls *s);
int rf_dtls_read(struct rf_dtls *s, uint8_t *data, size_t length);
int rf_dtls_write(struct rf_dtls *s, const uint8_t *data, size_t length);
void rf_dtls_free(struct rf_dtls *s);
#endif
