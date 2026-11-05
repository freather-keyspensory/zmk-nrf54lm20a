/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/platform_util.h>
#include "secure_dtls.h"
#include <string.h>
#if defined(CONFIG_MINEW_RADIO_SECURE) && !RADIO_TX
#ifndef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#error "Secure receiver requires cookie-checked DTLS client reconnect support"
#endif
#endif

static const int suites[] = {MBEDTLS_TLS_ECDHE_PSK_WITH_CHACHA20_POLY1305_SHA256, 0};
static const uint16_t groups[] = {MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1, 0};
/* The pinned PSA provider is not configured with its own threading layer.
 * Serialize all TLS/PSA allocation and credential-preservation hashing. */
K_MUTEX_DEFINE(radio_crypto_mutex);
static void set_timer(void *ctx, uint32_t intermediate, uint32_t final)
{
 struct rf_dtls *s = ctx;
 s->timer_start = k_uptime_get(); s->intermediate = intermediate; s->final = final;
}
static int get_timer(void *ctx)
{
 struct rf_dtls *s = ctx;
 if (!s->final) return -1;
 int64_t elapsed = k_uptime_get() - s->timer_start;
 return elapsed >= s->final ? 2 : elapsed >= s->intermediate ? 1 : 0;
}
void rf_dtls_free(struct rf_dtls *s)
{
 if (!s->initialized) return;
 k_mutex_lock(&radio_crypto_mutex, K_FOREVER);
 mbedtls_ssl_free(&s->ssl); mbedtls_ssl_config_free(&s->conf);
 mbedtls_ssl_cookie_free(&s->cookie);
 mbedtls_platform_zeroize(s, sizeof(*s));
 k_mutex_unlock(&radio_crypto_mutex);
}
static int initialize(struct rf_dtls *s, bool server, const uint8_t id[16], const uint8_t key[32],
                 void *io, mbedtls_ssl_send_t *send, mbedtls_ssl_recv_t *receive)
{
 int rc;
 if (s->initialized) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
 memset(s, 0, sizeof(*s)); s->initialized = true; s->server = server;
 mbedtls_ssl_init(&s->ssl); mbedtls_ssl_config_init(&s->conf);
 mbedtls_ssl_cookie_init(&s->cookie);
 memcpy(s->identity, "Minew-RF-v1:", 12); memcpy(s->identity + 12, id, 16);
#define TRY(call) do { rc = (call); if (rc) goto fail; } while (0)
 TRY(mbedtls_ssl_config_defaults(&s->conf, server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                                 MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_PRESET_DEFAULT));
 mbedtls_ssl_conf_ciphersuites(&s->conf, suites);
 mbedtls_ssl_conf_groups(&s->conf, groups);
 mbedtls_ssl_conf_min_tls_version(&s->conf, MBEDTLS_SSL_VERSION_TLS1_2);
 mbedtls_ssl_conf_max_tls_version(&s->conf, MBEDTLS_SSL_VERSION_TLS1_2);
 mbedtls_ssl_conf_handshake_timeout(&s->conf, 100, 1000);
 mbedtls_ssl_conf_dtls_anti_replay(&s->conf, MBEDTLS_SSL_ANTI_REPLAY_ENABLED);
 TRY(mbedtls_ssl_conf_psk(&s->conf, key, 32, s->identity, sizeof(s->identity)));
 if (server) {
  TRY(mbedtls_ssl_cookie_setup(&s->cookie));
  mbedtls_ssl_conf_dtls_cookies(&s->conf, mbedtls_ssl_cookie_write,
                               mbedtls_ssl_cookie_check, &s->cookie);
 }
 TRY(mbedtls_ssl_setup(&s->ssl, &s->conf));
 mbedtls_ssl_set_bio(&s->ssl, io, send, receive, NULL);
 mbedtls_ssl_set_timer_cb(&s->ssl, s, set_timer, get_timer);
 mbedtls_ssl_set_mtu(&s->ssl, RF_DTLS_MTU);
 if (server) TRY(mbedtls_ssl_set_client_transport_id(&s->ssl, s->identity, sizeof(s->identity)));
 return 0;
fail:
 rf_dtls_free(s); return rc;
}
static int handshake(struct rf_dtls *s)
{
 int rc = mbedtls_ssl_handshake(&s->ssl);
 if (s->server && rc == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
  rc = mbedtls_ssl_session_reset(&s->ssl); if (rc) return rc;
  mbedtls_ssl_set_mtu(&s->ssl, RF_DTLS_MTU);
  rc = mbedtls_ssl_set_client_transport_id(&s->ssl, s->identity, sizeof(s->identity));
  return rc ? rc : MBEDTLS_ERR_SSL_WANT_READ;
 }
 if (!rc && mbedtls_ssl_get_ciphersuite_id_from_ssl(&s->ssl) != suites[0]) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
 return rc;
}
int rf_dtls_init(struct rf_dtls *s, bool server, const uint8_t id[16], const uint8_t key[32],
                 void *io, mbedtls_ssl_send_t *send, mbedtls_ssl_recv_t *receive)
{
 k_mutex_lock(&radio_crypto_mutex, K_FOREVER);
 int rc = initialize(s, server, id, key, io, send, receive);
 k_mutex_unlock(&radio_crypto_mutex); return rc;
}
int rf_dtls_handshake(struct rf_dtls *s)
{
 k_mutex_lock(&radio_crypto_mutex, K_FOREVER);
 int rc = handshake(s); k_mutex_unlock(&radio_crypto_mutex); return rc;
}
int rf_dtls_read(struct rf_dtls *s, uint8_t *data, size_t length)
{
 k_mutex_lock(&radio_crypto_mutex, K_FOREVER);
 int rc = mbedtls_ssl_read(&s->ssl, data, length);
 k_mutex_unlock(&radio_crypto_mutex); return rc;
}
int rf_dtls_write(struct rf_dtls *s, const uint8_t *data, size_t length)
{
 k_mutex_lock(&radio_crypto_mutex, K_FOREVER);
 int rc = mbedtls_ssl_write(&s->ssl, data, length);
 k_mutex_unlock(&radio_crypto_mutex); return rc;
}
