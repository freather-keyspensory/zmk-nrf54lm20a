/* SPDX-License-Identifier: MIT */
/* Two in-memory DTLS peers with PUBLIC TEST PSKs. No encrypted RF claim. */
#include <zephyr/kernel.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <string.h>
#include <errno.h>

#define MTU 224
#define DEPTH 8
struct packet { size_t length; uint8_t bytes[MTU]; };
struct endpoint {
 mbedtls_ssl_context ssl;
 mbedtls_ssl_config conf;
 mbedtls_ssl_cookie_ctx cookie;
 struct endpoint *other;
 struct packet queue[DEPTH], last_application;
 unsigned head, count;
 int64_t timer_start;
 uint32_t intermediate, final;
};
static struct endpoint client, server;
static const int suites[] = {MBEDTLS_TLS_ECDHE_PSK_WITH_CHACHA20_POLY1305_SHA256, 0};
static const uint16_t groups[] = {MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1, 0};
static const uint8_t identity[] = "PUBLIC-Minew-DTLS-probe";
/* magic, ABI, status, stage, error, handshake ms, pair176 ns, pair20 ns,
 * suite, max datagram, cookie requests, wrong-key/replay/tamper/old-session/
 * reflection/truncation rejected, new-session ciphertext differs,
 * post-fault valid, iterations, sent, received, reserved[2]. */
volatile uint32_t radio_dtls_probe_results[24];

static int enqueue(struct endpoint *e, const uint8_t *p, size_t n)
{
 if (n > MTU) return MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
 if (e->count == DEPTH) return MBEDTLS_ERR_SSL_WANT_WRITE;
 struct packet *q = &e->queue[(e->head + e->count++) % DEPTH];
 q->length = n; memcpy(q->bytes, p, n); return (int)n;
}
static int send_packet(void *context, const uint8_t *p, size_t n)
{
 struct endpoint *e = context;
 if (n > radio_dtls_probe_results[9]) radio_dtls_probe_results[9] = n;
 int rc = enqueue(e->other, p, n);
 if (rc >= 0) {
  radio_dtls_probe_results[20]++;
  if (n && p[0] == 23) { e->last_application.length = n; memcpy(e->last_application.bytes, p, n); }
 }
 return rc;
}
static int receive_packet(void *context, uint8_t *p, size_t n)
{
 struct endpoint *e = context;
 if (!e->count) return MBEDTLS_ERR_SSL_WANT_READ;
 struct packet *q = &e->queue[e->head];
 if (q->length > n) return MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
 memcpy(p, q->bytes, q->length); int used = q->length;
 e->head = (e->head + 1) % DEPTH; e->count--;
 radio_dtls_probe_results[21]++; return used;
}
static void set_timer(void *context, uint32_t intermediate, uint32_t final)
{
 struct endpoint *e = context;
 e->timer_start = k_uptime_get(); e->intermediate = intermediate; e->final = final;
}
static int get_timer(void *context)
{
 struct endpoint *e = context;
 if (!e->final) return -1;
 int64_t elapsed = k_uptime_get() - e->timer_start;
 return elapsed >= e->final ? 2 : elapsed >= e->intermediate ? 1 : 0;
}
static void free_peers(void)
{
 struct endpoint *ends[] = {&client, &server};
 for (unsigned i = 0; i < 2; i++) {
  mbedtls_ssl_free(&ends[i]->ssl); mbedtls_ssl_config_free(&ends[i]->conf);
  mbedtls_ssl_cookie_free(&ends[i]->cookie);
 }
}
static int setup_peer(struct endpoint *e, bool is_server, bool wrong_key)
{
 uint8_t key[32]; for (unsigned i = 0; i < sizeof(key); i++) key[i] = i;
 if (wrong_key) key[0] ^= 1;
 int rc = mbedtls_ssl_config_defaults(&e->conf,
    is_server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
    MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_PRESET_DEFAULT);
 if (rc) return rc;
 mbedtls_ssl_conf_ciphersuites(&e->conf, suites);
 mbedtls_ssl_conf_groups(&e->conf, groups);
 mbedtls_ssl_conf_min_tls_version(&e->conf, MBEDTLS_SSL_VERSION_TLS1_2);
 mbedtls_ssl_conf_max_tls_version(&e->conf, MBEDTLS_SSL_VERSION_TLS1_2);
 mbedtls_ssl_conf_handshake_timeout(&e->conf, 100, 1000);
 mbedtls_ssl_conf_dtls_anti_replay(&e->conf, MBEDTLS_SSL_ANTI_REPLAY_ENABLED);
 rc = mbedtls_ssl_conf_psk(&e->conf, key, sizeof(key), identity, sizeof(identity)-1);
 memset(key, 0, sizeof(key)); if (rc) return rc;
 if (is_server) {
  rc = mbedtls_ssl_cookie_setup(&e->cookie); if (rc) return rc;
  mbedtls_ssl_conf_dtls_cookies(&e->conf, mbedtls_ssl_cookie_write,
                               mbedtls_ssl_cookie_check, &e->cookie);
 }
 rc = mbedtls_ssl_setup(&e->ssl, &e->conf); if (rc) return rc;
 mbedtls_ssl_set_bio(&e->ssl, e, send_packet, receive_packet, NULL);
 mbedtls_ssl_set_timer_cb(&e->ssl, e, set_timer, get_timer);
 mbedtls_ssl_set_mtu(&e->ssl, MTU);
 if (is_server) return mbedtls_ssl_set_client_transport_id(&e->ssl, identity, sizeof(identity)-1);
 return 0;
}
static int start_peers(bool wrong_key)
{
 memset(&client, 0, sizeof(client)); memset(&server, 0, sizeof(server));
 struct endpoint *ends[] = {&client, &server};
 for (unsigned i = 0; i < 2; i++) {
  mbedtls_ssl_init(&ends[i]->ssl); mbedtls_ssl_config_init(&ends[i]->conf);
  mbedtls_ssl_cookie_init(&ends[i]->cookie);
 }
 client.other = &server; server.other = &client;
 int rc = setup_peer(&client, false, false); if (rc) return rc;
 return setup_peer(&server, true, wrong_key);
}
static int handshake(void)
{
 int64_t end = k_uptime_get() + 5000;
 while (k_uptime_get() < end) {
  struct endpoint *ends[] = {&client, &server};
  for (unsigned i = 0; i < 2; i++) {
   struct endpoint *e = ends[i];
   if (mbedtls_ssl_is_handshake_over(&e->ssl)) continue;
   int rc = mbedtls_ssl_handshake(&e->ssl);
   if (rc == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED && i == 1) {
    radio_dtls_probe_results[10]++;
    rc = mbedtls_ssl_session_reset(&e->ssl); if (rc) return rc;
    mbedtls_ssl_set_mtu(&e->ssl, MTU);
    rc = mbedtls_ssl_set_client_transport_id(&e->ssl, identity, sizeof(identity)-1);
   }
   if (rc && rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) return rc;
  }
  if (mbedtls_ssl_is_handshake_over(&client.ssl) && mbedtls_ssl_is_handshake_over(&server.ssl)) return 0;
  k_sleep(K_MSEC(1));
 }
 return MBEDTLS_ERR_SSL_TIMEOUT;
}
static int roundtrip(size_t length)
{
 uint8_t plain[176], out[176]; for (unsigned i = 0; i < sizeof(plain); i++) plain[i] = i;
 int rc = mbedtls_ssl_write(&client.ssl, plain, length);
 if (rc != (int)length) return rc < 0 ? rc : -EIO;
 rc = mbedtls_ssl_read(&server.ssl, out, sizeof(out));
 return rc == (int)length && !memcmp(plain, out, length) ? 0 : rc < 0 ? rc : -EIO;
}
static int reject(struct endpoint *e, const struct packet *p)
{
 uint8_t out[176];
 int rc = enqueue(e, p->bytes, p->length); if (rc < 0) return rc;
 rc = mbedtls_ssl_read(&e->ssl, out, sizeof(out));
 return rc == MBEDTLS_ERR_SSL_WANT_READ ? 0 : rc < 0 ? rc : -EIO;
}
int radio_dtls_probe(void)
{
 int rc = 0;
#define TRY(call) do { rc = (call); if (rc) goto done; } while (0)
 memset((void *)radio_dtls_probe_results, 0, sizeof(radio_dtls_probe_results));
 radio_dtls_probe_results[0] = 0x50433844; radio_dtls_probe_results[1] = 1;
 radio_dtls_probe_results[2] = 1; radio_dtls_probe_results[3] = 1;
 TRY(start_peers(false));
 int64_t start = k_uptime_get(); TRY(handshake());
 radio_dtls_probe_results[5] = k_uptime_get() - start;
 radio_dtls_probe_results[8] = MBEDTLS_TLS_ECDHE_PSK_WITH_CHACHA20_POLY1305_SHA256;
 radio_dtls_probe_results[3] = 2;
 TRY(roundtrip(176)); struct packet old = client.last_application;
 TRY(reject(&server, &old)); radio_dtls_probe_results[12] = 1;
 struct packet tampered = old; tampered.bytes[tampered.length-1] ^= 1;
 /* A new, not-yet-seen sequence reaches AEAD verification rather than only
  * the replay filter. Corrupt the next record before the receiver reads it. */
 uint8_t input[176] = {0};
 rc = mbedtls_ssl_write(&client.ssl, input, sizeof(input));
 if (rc != 176) { if (rc >= 0) rc = -EIO; goto done; }
 tampered = client.last_application; tampered.bytes[tampered.length-1] ^= 1;
 server.count = 0;
 TRY(reject(&server, &tampered)); radio_dtls_probe_results[13] = 1;
 TRY(reject(&client, &client.last_application)); radio_dtls_probe_results[15] = 1;
 tampered.length = 7; TRY(reject(&server, &tampered)); radio_dtls_probe_results[16] = 1;
 TRY(roundtrip(176)); radio_dtls_probe_results[18] = 1;
 radio_dtls_probe_results[3] = 3; radio_dtls_probe_results[19] = 1000;
 for (unsigned j = 0; j < 2; j++) {
  uint64_t cycles = k_cycle_get_64();
  for (unsigned i = 0; i < 1000; i++) TRY(roundtrip(j ? 20 : 176));
  radio_dtls_probe_results[6+j] = k_cyc_to_ns_floor64(k_cycle_get_64()-cycles)/1000;
 }
 free_peers(); TRY(start_peers(false)); TRY(handshake());
 radio_dtls_probe_results[3] = 4;
 TRY(reject(&server, &old)); radio_dtls_probe_results[14] = 1;
 TRY(roundtrip(176));
 if (old.length != client.last_application.length ||
     memcmp(old.bytes, client.last_application.bytes, old.length)) radio_dtls_probe_results[17] = 1;
 else { rc = -EIO; goto done; }
 free_peers(); TRY(start_peers(true));
 radio_dtls_probe_results[3] = 5;
 rc = handshake();
 radio_dtls_probe_results[22] = (uint32_t)rc;
 if (rc == 0 || mbedtls_ssl_is_handshake_over(&client.ssl) || mbedtls_ssl_is_handshake_over(&server.ssl)) {
  rc = -EIO; goto done;
 }
 radio_dtls_probe_results[11] = 1; rc = 0;
 radio_dtls_probe_results[3] = 6;
done:
 radio_dtls_probe_results[4] = (uint32_t)rc;
 radio_dtls_probe_results[2] = rc ? 3 : 2;
 free_peers();
 return rc ? -EIO : 0;
}
