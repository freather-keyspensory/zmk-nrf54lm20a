/* SPDX-License-Identifier: MIT */
/* Diagnostic stop-and-wait ARQ. No pairing, encryption, or keyboard injection. */
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <hal/nrf_radio.h>
#include <hal/nrf_timer.h>
#include "reliable.h"
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
#include "secure_dtls.h"
#include "secure_pair.h"
#include "secure_record.h"
#include <mbedtls/platform_util.h>
#include <psa/crypto.h>
volatile uint32_t radio_security_stats[10];
volatile int32_t radio_secure_last_error;
#endif
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/sys/reboot.h>
volatile uint32_t radio_recovery_request;
#endif
#define RADIO ((NRF_RADIO_Type *)DT_REG_ADDR(DT_NODELABEL(radio)))
#define TIMER ((NRF_TIMER_Type *)DT_REG_ADDR(DT_NODELABEL(timer21)))
volatile uint32_t radio_completed, radio_fresh, radio_missing, radio_duplicate;
volatile uint32_t radio_bad_crc, radio_bad_frame, radio_late, radio_heartbeat;
volatile uint32_t radio_last_sequence, radio_started;
volatile uint64_t radio_first_cycle, radio_last_cycle;
volatile uint32_t radio_link_stats[LS_COUNT];
static const uint16_t channels[] = {CONFIG_MINEW_RADIO_FREQUENCY_MHZ, 2444, 2474};
static uint8_t tx_wire[1 + ARQ_MAX_PAYLOAD] __aligned(4);
static uint8_t rx_wire[1 + ARQ_MAX_PAYLOAD] __aligned(4);
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
#define AIR_MAX_PAYLOAD RF_DTLS_MTU
static uint8_t air_tx[1 + AIR_MAX_PAYLOAD] __aligned(4);
static uint8_t air_rx[1 + AIR_MAX_PAYLOAD] __aligned(4);
BUILD_ASSERT(ARQ_MAX_PAYLOAD + 29 <= AIR_MAX_PAYLOAD);
#else
#define AIR_MAX_PAYLOAD ARQ_MAX_PAYLOAD
#define air_tx tx_wire
#define air_rx rx_wire
#endif
static uint32_t channel_index;
static volatile uint32_t fault_data, fault_ack, fault_pause_ms, fault_channel;
static volatile uint32_t restart_requested;
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE) && !RADIO_TX
K_SEM_DEFINE(secure_rx_done, 0, 1);
static uint8_t secure_rx_dma[1 + AIR_MAX_PAYLOAD] __aligned(4);
static volatile bool secure_rx_prepared, secure_rx_crc;
#endif
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
static volatile uint32_t recovery_requested;
#endif

#if RADIO_TX
K_MSGQ_DEFINE(samples, FRAME_SIZE, 4096, 4);
static uint32_t next_sequence, next_tick;
static int64_t last_feedback_ms;
static uint8_t pending_frames[ARQ_WINDOW * ARQ_BATCH * FRAME_SIZE];
static void sample_isr(const void *unused)
{
 ARG_UNUSED(unused);
 if (!nrf_timer_event_check(TIMER, NRF_TIMER_EVENT_COMPARE0)) return;
 nrf_timer_event_clear(TIMER, NRF_TIMER_EVENT_COMPARE0);
 nrf_timer_task_trigger(TIMER, NRF_TIMER_TASK_CAPTURE1);
 uint32_t now = nrf_timer_cc_get(TIMER, 1);
 uint32_t due = (uint32_t)(now - next_tick) / 2000 + 1;
 next_tick += due * 2000;
 nrf_timer_cc_set(TIMER, 0, next_tick);
 radio_link_stats[LS_SAMPLES] += due;
 if (due > 32) {
  radio_link_stats[LS_SAMPLE_LATE] += due - 32;
  radio_link_stats[LS_QUEUE_OVERFLOW] += due - 32;
  next_sequence += due - 32; due = 32;
 }
 for (uint32_t i = 0; i < due; i++) {
  uint8_t f[FRAME_SIZE];
  frame_make_session(f, next_sequence++, radio_link_stats[LS_LOCAL_EPOCH]);
  if (k_msgq_put(&samples, f, K_NO_WAIT)) radio_link_stats[LS_QUEUE_OVERFLOW]++;
  else radio_fresh++;
 }
 radio_last_sequence = next_sequence - 1;
 uint32_t queued = k_msgq_num_used_get(&samples);
 if (queued > radio_link_stats[LS_MAX_QUEUE]) radio_link_stats[LS_MAX_QUEUE] = queued;
}
static void sampling_stop(void)
{
 irq_disable(DT_IRQN(DT_NODELABEL(timer21)));
 nrf_timer_task_trigger(TIMER, NRF_TIMER_TASK_STOP);
 nrf_timer_event_clear(TIMER, NRF_TIMER_EVENT_COMPARE0);
}
static void sampling_start(void)
{
 next_sequence = 0; next_tick = 2000;
 nrf_timer_task_trigger(TIMER, NRF_TIMER_TASK_CLEAR);
 nrf_timer_cc_set(TIMER, 0, next_tick);
 nrf_timer_event_clear(TIMER, NRF_TIMER_EVENT_COMPARE0);
 irq_enable(DT_IRQN(DT_NODELABEL(timer21)));
 nrf_timer_task_trigger(TIMER, NRF_TIMER_TASK_START);
}
#else
int radio_usb_init(void);
bool radio_usb_is_ready(void);
bool radio_usb_offer_batch(const uint8_t *data, size_t count);
uint32_t radio_usb_begin_session(void);
#endif

static uint32_t new_epoch(void)
{
 uint32_t value = 0;
 const struct device *rng = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
 if (!device_is_ready(rng)) return 0;
 for (unsigned i = 0; i < 4 && (!value || value == radio_link_stats[LS_LOCAL_EPOCH]); i++) {
  if (entropy_get_entropy(rng, (uint8_t *)&value, sizeof(value))) return 0;
 }
 return value && value != radio_link_stats[LS_LOCAL_EPOCH] ? value : 0;
}
static bool expired(uint32_t start, uint32_t us)
{ return (uint32_t)(k_cycle_get_32() - start) >= k_us_to_cyc_ceil32(us); }
static bool disabled(void)
{
 if (nrf_radio_state_get(RADIO) == NRF_RADIO_STATE_DISABLED) return true;
 nrf_radio_task_trigger(RADIO, NRF_RADIO_TASK_DISABLE);
 uint32_t start = k_cycle_get_32();
 while (nrf_radio_state_get(RADIO) != NRF_RADIO_STATE_DISABLED) {
  if (expired(start, 300)) return false;
  k_busy_wait(1);
 }
 return true;
}
static void packet_event(void)
{
 radio_completed++;
 radio_last_cycle = k_cycle_get_64();
 if (radio_completed == 1) radio_first_cycle = radio_last_cycle;
}
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE) && !RADIO_TX
static void secure_radio_isr(const void *unused)
{
 ARG_UNUSED(unused);
 if (!secure_rx_prepared || !nrf_radio_event_check(RADIO, NRF_RADIO_EVENT_PHYEND)) return;
 nrf_radio_event_clear(RADIO, NRF_RADIO_EVENT_PHYEND);
 secure_rx_crc = nrf_radio_crc_status_check(RADIO);
 packet_event(); k_sem_give(&secure_rx_done);
}
static void secure_rx_stop(void)
{
 unsigned key = irq_lock();
 nrf_radio_int_disable(RADIO, NRF_RADIO_INT_PHYEND_MASK);
 secure_rx_prepared = false; k_sem_reset(&secure_rx_done);
 irq_unlock(key);
}
static bool secure_rx_arm(void)
{
 secure_rx_stop();
 if (!disabled()) return false;
 secure_rx_dma[0] = 0;
 nrf_radio_event_clear(RADIO, NRF_RADIO_EVENT_PHYEND);
 nrf_radio_event_clear(RADIO, NRF_RADIO_EVENT_DISABLED);
 nrf_radio_packetptr_set(RADIO, secure_rx_dma);
 nrf_radio_shorts_set(RADIO, NRF_RADIO_SHORT_READY_START_MASK | NRF_RADIO_SHORT_PHYEND_DISABLE_MASK);
 secure_rx_prepared = true;
 nrf_radio_int_enable(RADIO, NRF_RADIO_INT_PHYEND_MASK);
 nrf_radio_task_trigger(RADIO, NRF_RADIO_TASK_RXEN);
 return true;
}
static bool secure_receive_async(uint32_t timeout_us)
{
 if (!secure_rx_prepared && !secure_rx_arm()) return false;
 /* A receive timeout leaves the hardware listening. It must not cancel a
  * matched frame; PHYEND wakes us when the entire ciphertext is available. */
 if (k_sem_take(&secure_rx_done, K_USEC(timeout_us))) return false;
 bool crc = secure_rx_crc;
 if (!disabled()) return false;
 size_t n = secure_rx_dma[0];
 memcpy(air_rx, secure_rx_dma, n+1);
 /* Snapshot before rearming; DMA now uses a different buffer from the TLS
  * decoder. Rearm before decryption/USB work, with IRQ-driven completion. */
 if (!secure_rx_arm()) return false;
 if (!crc) { radio_bad_crc++; return false; }
 return true;
}
#endif
static void tune(uint32_t index)
{
 if (index >= ARRAY_SIZE(channels)) return;
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE) && !RADIO_TX
 secure_rx_stop();
#endif
 if (!disabled()) return;
 if (index != channel_index) radio_link_stats[LS_HOPS]++;
 channel_index = index;
 nrf_radio_frequency_set(RADIO, channels[index]);
 radio_link_stats[LS_CHANNEL] = channels[index];
}
static bool physical_transmit(size_t length)
{
 if (length > AIR_MAX_PAYLOAD) return false;
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE) && !RADIO_TX
 secure_rx_stop();
#endif
 if (!disabled()) return false;
 air_tx[0] = length;
 nrf_radio_event_clear(RADIO, NRF_RADIO_EVENT_PHYEND);
 nrf_radio_event_clear(RADIO, NRF_RADIO_EVENT_DISABLED);
 nrf_radio_packetptr_set(RADIO, air_tx);
 nrf_radio_shorts_set(RADIO, NRF_RADIO_SHORT_READY_START_MASK | NRF_RADIO_SHORT_PHYEND_DISABLE_MASK);
 nrf_radio_task_trigger(RADIO, NRF_RADIO_TASK_TXEN);
 uint32_t start = k_cycle_get_32();
 while (!nrf_radio_event_check(RADIO, NRF_RADIO_EVENT_PHYEND)) {
  if (expired(start, 1500)) { disabled(); radio_link_stats[LS_TIMEOUTS]++; return false; }
  k_busy_wait(1);
 }
 packet_event();
 return disabled();
}
static bool physical_receive(uint32_t timeout_us)
{
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE) && !RADIO_TX
 return secure_receive_async(timeout_us);
#else
 if (!disabled()) return false;
 air_rx[0] = 0;
#if !RADIO_TX || IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
 nrf_radio_event_clear(RADIO, NRF_RADIO_EVENT_ADDRESS);
#endif
 nrf_radio_event_clear(RADIO, NRF_RADIO_EVENT_PHYEND);
 nrf_radio_event_clear(RADIO, NRF_RADIO_EVENT_DISABLED);
 nrf_radio_packetptr_set(RADIO, air_rx);
 nrf_radio_shorts_set(RADIO, NRF_RADIO_SHORT_READY_START_MASK | NRF_RADIO_SHORT_PHYEND_DISABLE_MASK);
 nrf_radio_task_trigger(RADIO, NRF_RADIO_TASK_RXEN);
 uint32_t start = k_cycle_get_32();
#if !RADIO_TX || IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
 struct arq_receive_window window = {start, k_us_to_cyc_ceil32(timeout_us), false};
#endif
 while (!nrf_radio_event_check(RADIO, NRF_RADIO_EVENT_PHYEND)) {
#if !RADIO_TX || IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
  bool timeout = arq_receive_expired(&window, k_cycle_get_32(),
       nrf_radio_event_check(RADIO, NRF_RADIO_EVENT_ADDRESS), k_us_to_cyc_ceil32(1000));
#else
  bool timeout = expired(start, timeout_us);
#endif
  if (timeout) { disabled(); return false; }
  k_busy_wait(1);
 }
 packet_event();
 bool crc = nrf_radio_crc_status_check(RADIO);
 if (!disabled()) return false;
 if (!crc) { radio_bad_crc++; return false; }
#if !IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
 if (!arq_valid(rx_wire + 1, rx_wire[0])) { radio_bad_frame++; return false; }
#endif
 return true;
#endif
}

#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
static struct rf_dtls secure_session;
struct secure_saved_packet { size_t length; uint8_t bytes[AIR_MAX_PAYLOAD]; };
static struct secure_saved_packet secure_current, secure_previous, secure_last_input, secure_plain;
static volatile uint32_t fault_wire_tamper, fault_wire_replay, fault_old_replay, fault_plaintext;
static bool secure_input_injected;
static uint32_t secure_pair_version, secure_read_start, secure_read_budget;
static int64_t secure_last_wire, secure_last_authenticated, secure_started;
static void secure_reset(void)
{
 if (secure_current.length) secure_previous = secure_current;
 secure_current.length = secure_plain.length = 0; secure_input_injected = false;
 rf_dtls_free(&secure_session);
 radio_security_stats[0] = rf_pair_is_ready() ? 1 : 0;
 radio_link_stats[LS_STATE] = 0;
}
static int secure_bio_send(void *unused, const uint8_t *data, size_t length)
{
 ARG_UNUSED(unused);
 if (secure_pair_version != rf_pair_generation()) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
 if (length > AIR_MAX_PAYLOAD) return MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
 memcpy(air_tx+1, data, length);
 if (!physical_transmit(length)) return MBEDTLS_ERR_SSL_WANT_WRITE;
 radio_security_stats[3]++; return length;
}
static int secure_bio_receive(void *unused, uint8_t *data, size_t length)
{
 ARG_UNUSED(unused);
 if (secure_pair_version != rf_pair_generation()) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
 if (expired(secure_read_start, secure_read_budget)) return MBEDTLS_ERR_SSL_WANT_READ;
 /* Explicit local negative tests enter the same pre-authentication decoder
  * as a CRC-valid RF datagram. They never disable library authentication or
  * replay checks. The DATA-injection counter counts consumed test datagrams. */
 struct secure_saved_packet *injected = NULL;
 volatile uint32_t *remaining = NULL;
 if (radio_security_stats[0] == 2) {
  if (fault_wire_replay && secure_current.length) { injected = &secure_current; remaining = &fault_wire_replay; }
  else if (fault_old_replay && secure_previous.length) { injected = &secure_previous; remaining = &fault_old_replay; }
  else if (fault_plaintext && secure_plain.length) { injected = &secure_plain; remaining = &fault_plaintext; }
 }
 if (injected && injected->length <= length) {
  memcpy(data, injected->bytes, injected->length); (*remaining)--;
  radio_link_stats[LS_INJECT_DATA]++; secure_input_injected = true;
  return injected->length;
 }
 if (!physical_receive(1500)) return MBEDTLS_ERR_SSL_WANT_READ;
 size_t n = air_rx[0];
 if (n > length) return MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
 bool reject_clear = radio_security_stats[0] == 2 && k_uptime_get() - secure_last_authenticated < 300;
 if (!rf_dtls_framing(air_rx+1, n, reject_clear)) return MBEDTLS_ERR_SSL_WANT_READ;
 memcpy(data, air_rx+1, n); secure_last_wire = k_uptime_get();
 secure_input_injected = false;
 if (fault_wire_tamper && radio_security_stats[0] == 2 && n > 13 && data[0] == 23) {
  data[n-1] ^= 1; fault_wire_tamper--; radio_link_stats[LS_INJECT_DATA]++;
  secure_input_injected = true;
 }
 secure_last_input.length = n; memcpy(secure_last_input.bytes, data, n);
 radio_security_stats[4]++; return n;
}
static bool secure_connected(void)
{
 if (secure_session.initialized && secure_pair_version != rf_pair_generation()) secure_reset();
 if (!secure_session.initialized) {
  uint8_t id[16], secret[32];
  if (!rf_pair_snapshot(id, secret, &secure_pair_version)) { radio_security_stats[0] = 0; return false; }
  int rc = rf_dtls_init(&secure_session, !RADIO_TX, id, secret, NULL, secure_bio_send, secure_bio_receive);
  mbedtls_platform_zeroize(secret, sizeof(secret)); mbedtls_platform_zeroize(id, sizeof(id));
  if (rc) { radio_secure_last_error = rc; radio_security_stats[2]++; return false; }
  secure_started = secure_last_wire = k_uptime_get(); radio_security_stats[0] = 1;
 }
 if (radio_security_stats[0] == 2) return true;
 int64_t now = k_uptime_get();
 if (now - secure_started > 12000) { radio_security_stats[2]++; secure_reset(); return false; }
 /* Hold a channel long enough for the ephemeral-key handshake computation.
  * Steady-state ARQ retains the independently tested 90 ms dwell. */
 if (now - secure_last_wire > (RADIO_TX ? 600 : 500)) {
  tune((channel_index+1) % ARRAY_SIZE(channels));
  secure_last_wire = now - (RADIO_TX ? 0 : 490);
 }
 secure_read_start = k_cycle_get_32(); secure_read_budget = 2000;
 int rc = rf_dtls_handshake(&secure_session);
 if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) return false;
 if (rc) { radio_secure_last_error = rc; radio_security_stats[2]++; secure_reset(); return false; }
 if (secure_pair_version != rf_pair_generation()) { secure_reset(); return false; }
 radio_security_stats[0] = 2; radio_security_stats[1]++;
 secure_last_authenticated = k_uptime_get(); return true;
}
static bool transmit(size_t length)
{
 if (radio_security_stats[0] != 2 || secure_pair_version != rf_pair_generation()) return false;
 int rc = rf_dtls_write(&secure_session, tx_wire+1, length);
 if (rc == (int)length) { radio_security_stats[5]++; return true; }
 if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
  radio_secure_last_error = rc;
  radio_security_stats[2]++; secure_reset();
 }
 return false;
}
static bool receive(uint32_t timeout_us)
{
 if (radio_security_stats[0] != 2 || secure_pair_version != rf_pair_generation()) return false;
 secure_read_start = k_cycle_get_32(); secure_read_budget = timeout_us;
 int rc = rf_dtls_read(&secure_session, rx_wire+1, ARQ_MAX_PAYLOAD);
 if (rc == MBEDTLS_ERR_SSL_CLIENT_RECONNECT) {
  /* Mbed TLS already reset this server context after checking the cookie.
   * Keep its configuration/cookie and require the entire new PSK handshake.
   * It must never remain marked authenticated during that handshake. */
  if (secure_current.length) secure_previous = secure_current;
  secure_current.length = secure_plain.length = 0; secure_input_injected = false;
  radio_security_stats[0] = 1; radio_link_stats[LS_STATE] = 0;
  secure_started = secure_last_wire = k_uptime_get();
  return false;
 }
 if (rc > 0 && secure_input_injected) {
  /* A security test fails loudly if the library ever releases plaintext
   * from a known bad/replayed datagram, even if ARQ would deduplicate it. */
  radio_security_stats[2]++; secure_reset();
  mbedtls_platform_zeroize(rx_wire, sizeof(rx_wire)); return false;
 }
 if (rc > 0 && secure_pair_version == rf_pair_generation()) {
  secure_last_authenticated = k_uptime_get(); radio_security_stats[6]++;
  rx_wire[0] = rc;
  if (arq_valid(rx_wire+1, rc)) {
   secure_current = secure_last_input;
   secure_plain.length = rc; memcpy(secure_plain.bytes, rx_wire+1, rc);
   return true;
  }
  radio_bad_frame++; return false;
 }
 mbedtls_platform_zeroize(rx_wire, sizeof(rx_wire));
 if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
  radio_secure_last_error = rc;
  radio_security_stats[2]++; secure_reset();
 }
 return false;
}
#else
#define transmit physical_transmit
#define receive physical_receive
#endif

/* Only bounded USB diagnostic commands; never memory, flash or RF remote commands. */
int radio_link_control(uint32_t op, uint32_t value)
{
 switch (op) {
 case 1: if (value > 64) return -EINVAL; fault_data = value; break;
 case 2: if (value > 64) return -EINVAL; fault_ack = value; break;
 case 3: if (!value || value > 100) return -EINVAL; fault_pause_ms = value; break;
 case 4: if (value != 1) return -EINVAL; restart_requested = 1; break;
 case 5: if (value >= ARRAY_SIZE(channels)) return -EINVAL; fault_channel = value + 1; break;
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
 case 8: if (value > 64) return -EINVAL; fault_wire_tamper = value; break;
 case 9: if (value > 64) return -EINVAL; fault_wire_replay = value; break;
 case 10: if (value > 64) return -EINVAL; fault_old_replay = value; break;
 case 11: if (value > 64) return -EINVAL; fault_plaintext = value; break;
#endif
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
 case 6: if (value != 1) return -EINVAL; recovery_requested = 1; break;
#endif
 default: return -ENOTSUP;
 }
 return 0;
}

#if RADIO_TX
static bool exchange(size_t len, uint8_t wanted_status)
{
 uint32_t started = k_cycle_get_32();
 if (!transmit(len)) return false;
 if (tx_wire[4] == ARQ_DATA) radio_link_stats[LS_DATA_SENT]++;
 if (!receive(1500)) { radio_link_stats[LS_TIMEOUTS]++; return false; }
 const uint8_t *a = rx_wire + 1, *d = tx_wire + 1;
 if (a[3] != ARQ_ACK || frame_get32(a + 4) != frame_get32(d + 4) ||
     frame_get32(a + 8) != frame_get32(d + 8) || a[12] != d[12]) return false;
 radio_link_stats[LS_ACK_RECEIVED]++;
 last_feedback_ms = k_uptime_get();
 uint32_t remote = frame_get32(a + 16);
 if (wanted_status == ARQ_READY) {
  if (a[13] == ARQ_READY) radio_link_stats[LS_REMOTE_EPOCH] = remote;
 } else if (remote != radio_link_stats[LS_REMOTE_EPOCH] || a[13] == ARQ_RESET) {
  radio_link_stats[LS_RESTARTS]++; radio_link_stats[LS_STATE] = 0; return false;
 }
 if (a[13] == ARQ_BUSY) radio_link_stats[LS_BUSY]++;
 uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - started);
 if (us > radio_link_stats[LS_MAX_ACK_US]) radio_link_stats[LS_MAX_ACK_US] = us;
 return a[13] == wanted_status;
}
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
static bool window_send(size_t length, int64_t deadline, uint32_t overflow)
{
 while (k_uptime_get() < deadline && radio_link_stats[LS_STATE] &&
        radio_security_stats[0] == 2 && secure_pair_version == rf_pair_generation() &&
        radio_link_stats[LS_QUEUE_OVERFLOW] == overflow) {
  /* Preserve identical arguments across a nonblocking TLS write retry. */
  if (transmit(length)) {
   if (tx_wire[4] == ARQ_DATA) radio_link_stats[LS_DATA_SENT]++;
   return true;
  }
  k_sleep(K_USEC(100));
 }
 return false;
}
static int window_ack(uint32_t first, unsigned *acknowledged)
{
 if (!receive(2000)) { radio_link_stats[LS_TIMEOUTS]++; return 0; }
 const uint8_t *a = rx_wire+1;
 if (a[3] != ARQ_ACK || frame_get32(a+4) != radio_link_stats[LS_LOCAL_EPOCH] || a[12]) return 0;
 if (a[13] == ARQ_RESET || frame_get32(a+16) != radio_link_stats[LS_REMOTE_EPOCH]) {
  radio_link_stats[LS_RESTARTS]++; radio_link_stats[LS_STATE] = 0; return -1;
 }
 if (a[13] != ARQ_COMMITTED && a[13] != ARQ_BUSY) return 0;
 int progress = arq_window_progress(first, frame_get32(a+8));
 if (progress < 0 || (unsigned)progress < *acknowledged) return 0;
 *acknowledged = progress; radio_link_stats[LS_ACK_RECEIVED]++;
 last_feedback_ms = k_uptime_get();
 if (a[13] == ARQ_BUSY) radio_link_stats[LS_BUSY]++;
 return 1;
}
static bool send_window(uint32_t *pending, uint32_t overflow)
{
 int64_t deadline = k_uptime_get()+300, next_hop = k_uptime_get()+ARQ_TX_DWELL_MS;
 *pending = 0;
 for (unsigned packet = 0; packet < ARQ_WINDOW; packet++) {
  for (unsigned sample = 0; sample < ARQ_BATCH; sample++) {
   if (k_msgq_get(&samples, pending_frames + *pending * FRAME_SIZE, K_FOREVER)) return false;
   (*pending)++;
  }
  size_t n = arq_pack_data(tx_wire+1, pending_frames+packet*ARQ_BATCH*FRAME_SIZE, ARQ_BATCH);
  if (!n) return false;
  tx_wire[1+13] = packet+1 == ARQ_WINDOW ? ARQ_ACK_REQUEST : 0;
  if (!window_send(n, deadline, overflow)) return false;
  /* Receiver decrypt/USB/rearm budget. Newly produced batches are sent as
   * they become available; collecting a window never delays its first batch. */
  if (packet+1 < ARQ_WINDOW) k_busy_wait(350);
 }
 uint32_t first = frame_get32(pending_frames+4);
 unsigned acknowledged = 0;
 for (;;) {
  uint32_t started = k_cycle_get_32();
  int result = window_ack(first, &acknowledged);
  uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32()-started);
  if (us > radio_link_stats[LS_MAX_ACK_US]) radio_link_stats[LS_MAX_ACK_US] = us;
  *pending = (ARQ_WINDOW-acknowledged)*ARQ_BATCH;
  if (acknowledged == ARQ_WINDOW) return radio_link_stats[LS_QUEUE_OVERFLOW] == overflow;
  if (result < 0 || !radio_link_stats[LS_STATE] || k_uptime_get() >= deadline ||
      radio_link_stats[LS_QUEUE_OVERFLOW] != overflow || secure_pair_version != rf_pair_generation()) return false;
  radio_link_stats[LS_RETRIES]++;
  if (k_uptime_get() >= next_hop && k_uptime_get()-last_feedback_ms >= ARQ_TX_DWELL_MS) {
   tune((channel_index+1)%ARRAY_SIZE(channels)); next_hop = k_uptime_get()+ARQ_TX_DWELL_MS;
  }
  if (result == 1) {
   /* Cumulative progress identifies the missing suffix. Retries are new
    * DTLS records, retaining their original application sample sequences. */
   for (unsigned packet = acknowledged; packet < ARQ_WINDOW; packet++) {
    size_t n = arq_pack_data(tx_wire+1, pending_frames+packet*ARQ_BATCH*FRAME_SIZE, ARQ_BATCH);
    if (!n) return false;
    tx_wire[1+13] = packet+1 == ARQ_WINDOW ? ARQ_ACK_REQUEST : 0;
    if (!window_send(n, deadline, overflow)) return false;
    if (packet+1 < ARQ_WINDOW) k_busy_wait(350);
   }
  } else {
   /* A lost ACK does not require re-encrypting/retransmitting four DATA
    * packets. An authenticated poll retrieves the receiver's commit point. */
   arq_header(tx_wire+1, ARQ_POLL, radio_link_stats[LS_LOCAL_EPOCH], first, 0, 0);
   if (!window_send(ARQ_HEADER, deadline, overflow)) return false;
  }
 }
}
#endif
static void worker(void *a, void *b, void *c)
{
 ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
 while (!radio_started) k_sleep(K_MSEC(1));
 uint32_t pending = 0;
 for (;;) {
  sampling_stop();
  radio_link_stats[LS_DISCARDED] += pending + k_msgq_num_used_get(&samples);
  k_msgq_purge(&samples); pending = 0;
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
  secure_reset();
  while (!secure_connected()) k_sleep(K_USEC(100));
#endif
  uint32_t epoch = new_epoch();
  if (!epoch) { k_sleep(K_MSEC(10)); continue; }
  radio_link_stats[LS_LOCAL_EPOCH] = epoch;
  radio_link_stats[LS_STATE] = 0;
  arq_header(tx_wire + 1, ARQ_HELLO, epoch, 0, 0, 0);
  int64_t next_hop = k_uptime_get() + ARQ_TX_DWELL_MS;
  while (!exchange(ARQ_HEADER, ARQ_READY)) {
   if (k_uptime_get() >= next_hop && k_uptime_get() - last_feedback_ms >= ARQ_TX_DWELL_MS) {
    tune((channel_index + 1) % ARRAY_SIZE(channels)); next_hop = k_uptime_get() + ARQ_TX_DWELL_MS;
   }
   k_sleep(K_USEC(200));
  }
  radio_link_stats[LS_STATE] = 1;
  sampling_start();
  uint32_t overflow_at_start = radio_link_stats[LS_QUEUE_OVERFLOW];
  bool connected = true;
  while (connected) {
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
   connected = send_window(&pending, overflow_at_start);
   if (!connected) radio_link_stats[LS_LINK_LOSS]++;
#else
   for (pending = 0; pending < ARQ_BATCH; pending++) {
    k_msgq_get(&samples, pending_frames + pending * FRAME_SIZE, K_FOREVER);
   }
   size_t data_len = arq_pack_data(tx_wire + 1, pending_frames, ARQ_BATCH);
   if (!data_len) { connected = false; radio_link_stats[LS_LINK_LOSS]++; break; }
   int64_t deadline = k_uptime_get() + 300;
   next_hop = k_uptime_get() + ARQ_TX_DWELL_MS;
   while (!exchange(data_len, ARQ_COMMITTED)) {
    radio_link_stats[LS_RETRIES]++;
    if (!radio_link_stats[LS_STATE] || k_uptime_get() >= deadline || radio_link_stats[LS_QUEUE_OVERFLOW] != overflow_at_start) {
     connected = false; radio_link_stats[LS_LINK_LOSS]++; break;
    }
    if (k_uptime_get() >= next_hop && k_uptime_get() - last_feedback_ms >= ARQ_TX_DWELL_MS) {
     tune((channel_index + 1) % ARRAY_SIZE(channels)); next_hop = k_uptime_get() + ARQ_TX_DWELL_MS;
    }
    k_sleep(K_USEC(100));
   }
   if (connected) { pending = 0; k_busy_wait(20); }
#endif
  }
 }
}
#else
static void acknowledge(const uint8_t *p, uint8_t status)
{
 if (fault_ack) { fault_ack--; radio_link_stats[LS_INJECT_ACK]++; return; }
 arq_header(tx_wire + 1, ARQ_ACK, frame_get32(p + 4), frame_get32(p + 8), p[12], status);
 frame_put32(tx_wire + 1 + 16, radio_link_stats[LS_LOCAL_EPOCH]);
 if (transmit(20)) radio_link_stats[LS_ACK_SENT]++;
}
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
static void acknowledge_progress(const uint8_t *p, uint32_t next, uint8_t status, bool force)
{
 if (!force && !(p[13] & ARQ_ACK_REQUEST)) return;
 if (fault_ack) { fault_ack--; radio_link_stats[LS_INJECT_ACK]++; return; }
 arq_header(tx_wire+1, ARQ_ACK, frame_get32(p+4), next, 0, status);
 frame_put32(tx_wire+1+16, radio_link_stats[LS_LOCAL_EPOCH]);
 if (transmit(20)) radio_link_stats[LS_ACK_SENT]++;
}
#endif
static void worker(void *a, void *b, void *c)
{
 ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
 while (!radio_started) k_sleep(K_MSEC(1));
 struct arq_receiver peer = {0};
 uint8_t decoded_frames[ARQ_BATCH * FRAME_SIZE];
 int64_t last_activity = k_uptime_get(), next_hop = last_activity + 20;
 int64_t last_peer_activity = last_activity;
 int64_t forced_until = 0;
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
 uint32_t peer_secure_session = 0;
#endif
 for (;;) {
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
  if (!secure_connected()) { k_sleep(K_USEC(100)); continue; }
  if (peer_secure_session != radio_security_stats[1]) {
   /* A resumed source port is a new authenticated connection, never a
    * continuation of the previous application's sequence state. */
   memset(&peer, 0, sizeof(peer)); peer_secure_session = radio_security_stats[1];
   radio_link_stats[LS_REMOTE_EPOCH] = radio_link_stats[LS_STATE] = 0;
  }
  /* CRC-valid, well-framed records keep the physical channel stable during
   * bounded corruption/retry bursts. Only authenticated application traffic
   * updates last_peer_activity or permits sample delivery. */
  if (secure_last_wire > last_activity) {
   last_activity = secure_last_wire; next_hop = last_activity+20;
  }
#endif
  /* Keep the receiver's status honest while its peer is powered off. Retain
   * sequence state so a short outage can still resume without discarding it. */
  if (radio_link_stats[LS_STATE] && k_uptime_get() - last_peer_activity >= 300) {
   radio_link_stats[LS_STATE] = 0; radio_link_stats[LS_LINK_LOSS]++;
  }
  if (fault_pause_ms) {
   uint32_t ms = fault_pause_ms; fault_pause_ms = 0;
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
   secure_rx_stop();
#endif
   disabled(); k_sleep(K_MSEC(ms));
  }
  if (fault_channel) {
   uint32_t index = fault_channel - 1; fault_channel = 0; tune(index);
   forced_until = k_uptime_get() + 100; next_hop = forced_until;
  }
  if (k_uptime_get() >= forced_until && k_uptime_get() - last_activity > 20 && k_uptime_get() >= next_hop) {
   tune((channel_index + 1) % ARRAY_SIZE(channels)); next_hop = k_uptime_get() + 10;
  }
  if (!receive(2000)) continue;
  const uint8_t *p = rx_wire + 1;
  if (p[3] == ARQ_ACK) continue;
  last_activity = k_uptime_get(); next_hop = last_activity + 20;
  if (p[3] == ARQ_HELLO) {
   if (!radio_usb_is_ready()) { acknowledge(p, ARQ_BUSY); continue; }
   bool changed = peer.epoch != frame_get32(p + 4);
   if (!arq_begin(&peer, frame_get32(p + 4))) continue;
   if (changed) {
    radio_link_stats[LS_DISCARDED] += radio_usb_begin_session();
    radio_link_stats[LS_RESTARTS]++;
   }
   radio_link_stats[LS_REMOTE_EPOCH] = peer.epoch;
   last_peer_activity = k_uptime_get();
   radio_link_stats[LS_STATE] = 1;
   acknowledge(p, ARQ_READY); continue;
  }
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
  if (p[3] == ARQ_POLL) {
   bool known = peer.epoch && peer.epoch == frame_get32(p+4);
   if (known) { last_peer_activity = k_uptime_get(); radio_link_stats[LS_STATE] = 1; }
   acknowledge_progress(p, peer.expected, known ? ARQ_COMMITTED : ARQ_RESET, true);
   continue;
  }
#endif
  radio_link_stats[LS_DATA_PACKETS]++;
  if (fault_data) { fault_data--; radio_link_stats[LS_INJECT_DATA]++; continue; }
  if (frame_get32(p + 4) != peer.epoch || !peer.epoch) {
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
   acknowledge_progress(p, peer.expected, ARQ_RESET, true);
#else
   acknowledge(p, ARQ_RESET);
#endif
   continue;
  }
  uint32_t base = frame_get32(p + 8);
  int state = arq_classify(&peer, base, p[12]);
  if (state == ARQ_EXPECTED || state == ARQ_DUPLICATE) {
   last_peer_activity = k_uptime_get(); radio_link_stats[LS_STATE] = 1;
  }
  if (state == ARQ_DUPLICATE) {
   radio_duplicate += p[12];
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
   acknowledge_progress(p, peer.expected, ARQ_COMMITTED, false);
#else
   acknowledge(p, ARQ_COMMITTED);
#endif
   continue;
  }
  if (state != ARQ_EXPECTED) {
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
   acknowledge_progress(p, peer.expected, ARQ_COMMITTED, false);
#else
   acknowledge(p, ARQ_RESET);
#endif
   continue;
  }
  if (!arq_unpack_data(p, rx_wire[0], decoded_frames)) { radio_bad_frame++; continue; }
  if (!radio_usb_offer_batch(decoded_frames, p[12])) {
   radio_link_stats[LS_BACKPRESSURE]++;
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
   acknowledge_progress(p, peer.expected, ARQ_BUSY, false);
#else
   acknowledge(p, ARQ_BUSY);
#endif
   continue;
  }
  arq_commit(&peer, base, p[12]);
  radio_last_sequence = peer.expected - 1; radio_fresh += p[12];
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
  acknowledge_progress(p, peer.expected, ARQ_COMMITTED, false);
#else
  acknowledge(p, ARQ_COMMITTED);
#endif
 }
}
#endif
K_THREAD_DEFINE(link_thread, IS_ENABLED(CONFIG_MINEW_RADIO_SECURE) ? 8192 : 4096, worker, NULL, NULL, NULL,
                IS_ENABLED(CONFIG_MINEW_RADIO_SECURE) && !RADIO_TX ? 2 : 5, 0, 0);

int main(void)
{
#if !RADIO_TX
 int usb = radio_usb_init(); if (usb) return usb;
#endif
 int err = nrf_clock_control_request_sync(DEVICE_DT_GET(DT_NODELABEL(xo)), NULL, K_SECONDS(1));
 if (err) return err;
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
 err = psa_crypto_init(); if (err) return -EIO;
 err = rf_pair_init();
 if (err) printk("Pairing storage unavailable (%d); RF remains unpaired\n", err);
#endif
#if IS_ENABLED(CONFIG_MINEW_RADIO_CRYPTO_PROBE)
 extern int radio_crypto_probe(void);
 err = radio_crypto_probe(); if (err) return err;
#endif
#if IS_ENABLED(CONFIG_MINEW_RADIO_DTLS_PROBE)
 extern int radio_dtls_probe(void);
 err = radio_dtls_probe(); if (err) return err;
#endif
 uint32_t epoch = new_epoch(); if (!epoch) return -EIO;
 radio_link_stats[LS_LOCAL_EPOCH] = epoch;
 nrf_radio_int_disable(RADIO, UINT32_MAX);
 nrf_radio_mode_set(RADIO, NRF_RADIO_MODE_NRF_4MBIT_BT_0_6);
 nrf_radio_fast_ramp_up_enable_set(RADIO, true);
 nrf_radio_txpower_set(RADIO, NRF_RADIO_TXPOWER_0DBM);
 nrf_radio_packet_conf_t conf = {.lflen = 8, .plen = NRF_RADIO_PREAMBLE_LENGTH_16BIT,
  .maxlen = AIR_MAX_PAYLOAD, .statlen = 0, .balen = 4, .big_endian = false, .whiteen = false};
 nrf_radio_packet_configure(RADIO, &conf);
 nrf_radio_base0_set(RADIO, 0x8e89bed6); nrf_radio_prefix0_set(RADIO, IS_ENABLED(CONFIG_MINEW_RADIO_SECURE) ? 0x75 : 0x74);
 nrf_radio_txaddress_set(RADIO, 0); nrf_radio_rxaddresses_set(RADIO, 1);
 nrf_radio_crc_configure(RADIO, 3, NRF_RADIO_CRC_ADDR_INCLUDE, 0x00065b);
 nrf_radio_crcinit_set(RADIO, 0x555555); tune(0);
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE) && !RADIO_TX
 IRQ_CONNECT(DT_IRQN(DT_NODELABEL(radio)), 2, secure_radio_isr, NULL, 0);
 irq_enable(DT_IRQN(DT_NODELABEL(radio)));
#endif
#if RADIO_TX
 nrf_timer_mode_set(TIMER, NRF_TIMER_MODE_TIMER); nrf_timer_bit_width_set(TIMER, NRF_TIMER_BIT_WIDTH_32);
 nrf_timer_prescaler_set(TIMER, NRF_TIMER_PRESCALER_CALCULATE(NRF_TIMER_BASE_FREQUENCY_GET(TIMER), 16000000));
 nrf_timer_int_enable(TIMER, NRF_TIMER_INT_COMPARE0_MASK);
 IRQ_CONNECT(DT_IRQN(DT_NODELABEL(timer21)), 2, sample_isr, NULL, 0);
 sampling_stop();
#endif
 radio_started = 1;
 printk("R8 ARQv%u %s: %u-sample batches, 125 us sampling; diagnostic, security=%u\n",
        ARQ_WIRE_VERSION, RADIO_TX ? "TX" : "RX", ARQ_BATCH, IS_ENABLED(CONFIG_MINEW_RADIO_SECURE));
 int64_t heartbeat_at = k_uptime_get() + 1000;
 for (;;) {
  k_sleep(K_MSEC(10));
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
  rf_pair_service(); radio_security_stats[7] = rf_pair_id_crc();
  radio_security_stats[8] = radio_pair_mailbox.response; radio_security_stats[9] = rf_pair_generation();
#endif
  if (restart_requested) {
   k_sleep(K_MSEC(100));
#if IS_ENABLED(CONFIG_REBOOT)
   sys_reboot(SYS_REBOOT_COLD);
#endif
  }
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
  if (recovery_requested) {
   recovery_requested = 0; k_sleep(K_MSEC(100)); radio_recovery_request = 3;
  }
  if (radio_recovery_request == 3) {
   uint8_t request = 0xb1; const struct device *retained = DEVICE_DT_GET(DT_NODELABEL(gpregret1));
   if (device_is_ready(retained) && !retained_mem_write(retained, 0, &request, 1)) sys_reboot(SYS_REBOOT_COLD);
   radio_recovery_request = 0;
  }
#endif
  if (k_uptime_get() < heartbeat_at) continue;
  heartbeat_at += 1000; radio_heartbeat++;
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
  if (radio_heartbeat == 5) printk("ARQ image confirmation: %d\n", boot_write_img_confirmed());
#endif
  printk("ARQ state=%u frames=%u retry=%u busy=%u ch=%u epoch=%08x peer=%08x\n",
   radio_link_stats[LS_STATE], radio_fresh, radio_link_stats[LS_RETRIES], radio_link_stats[LS_BACKPRESSURE],
   radio_link_stats[LS_CHANNEL], radio_link_stats[LS_LOCAL_EPOCH], radio_link_stats[LS_REMOTE_EPOCH]);
 }
}
