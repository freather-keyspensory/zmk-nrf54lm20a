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
static uint32_t channel_index;
static volatile uint32_t fault_data, fault_ack, fault_pause_ms, fault_channel;
static volatile uint32_t restart_requested;
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
static volatile uint32_t recovery_requested;
#endif

#if RADIO_TX
K_MSGQ_DEFINE(samples, FRAME_SIZE, 4096, 4);
static uint32_t next_sequence, next_tick;
static int64_t last_feedback_ms;
static uint8_t pending_frames[ARQ_BATCH * FRAME_SIZE];
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
static void tune(uint32_t index)
{
 if (index >= ARRAY_SIZE(channels) || !disabled()) return;
 if (index != channel_index) radio_link_stats[LS_HOPS]++;
 channel_index = index;
 nrf_radio_frequency_set(RADIO, channels[index]);
 radio_link_stats[LS_CHANNEL] = channels[index];
}
static bool transmit(size_t length)
{
 if (length > ARQ_MAX_PAYLOAD || !disabled()) return false;
 tx_wire[0] = length;
 nrf_radio_event_clear(RADIO, NRF_RADIO_EVENT_PHYEND);
 nrf_radio_event_clear(RADIO, NRF_RADIO_EVENT_DISABLED);
 nrf_radio_packetptr_set(RADIO, tx_wire);
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
static bool receive(uint32_t timeout_us)
{
 if (!disabled()) return false;
 rx_wire[0] = 0;
#if !RADIO_TX
 nrf_radio_event_clear(RADIO, NRF_RADIO_EVENT_ADDRESS);
#endif
 nrf_radio_event_clear(RADIO, NRF_RADIO_EVENT_PHYEND);
 nrf_radio_event_clear(RADIO, NRF_RADIO_EVENT_DISABLED);
 nrf_radio_packetptr_set(RADIO, rx_wire);
 nrf_radio_shorts_set(RADIO, NRF_RADIO_SHORT_READY_START_MASK | NRF_RADIO_SHORT_PHYEND_DISABLE_MASK);
 nrf_radio_task_trigger(RADIO, NRF_RADIO_TASK_RXEN);
 uint32_t start = k_cycle_get_32();
#if !RADIO_TX
 struct arq_receive_window window = {start, k_us_to_cyc_ceil32(timeout_us), false};
#endif
 while (!nrf_radio_event_check(RADIO, NRF_RADIO_EVENT_PHYEND)) {
#if !RADIO_TX
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
 if (!arq_valid(rx_wire + 1, rx_wire[0])) { radio_bad_frame++; return false; }
 return true;
}

/* Only bounded USB diagnostic commands; never memory, flash or RF remote commands. */
int radio_link_control(uint32_t op, uint32_t value)
{
 switch (op) {
 case 1: if (value > 64) return -EINVAL; fault_data = value; break;
 case 2: if (value > 64) return -EINVAL; fault_ack = value; break;
 case 3: if (!value || value > 100) return -EINVAL; fault_pause_ms = value; break;
 case 4: if (value != 1) return -EINVAL; restart_requested = 1; break;
 case 5: if (value >= ARRAY_SIZE(channels)) return -EINVAL; fault_channel = value + 1; break;
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
static void worker(void *a, void *b, void *c)
{
 ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
 while (!radio_started) k_sleep(K_MSEC(1));
 uint32_t pending = 0;
 for (;;) {
  sampling_stop();
  radio_link_stats[LS_DISCARDED] += pending + k_msgq_num_used_get(&samples);
  k_msgq_purge(&samples); pending = 0;
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
static void worker(void *a, void *b, void *c)
{
 ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
 while (!radio_started) k_sleep(K_MSEC(1));
 struct arq_receiver peer = {0};
 uint8_t decoded_frames[ARQ_BATCH * FRAME_SIZE];
 int64_t last_activity = k_uptime_get(), next_hop = last_activity + 20;
 int64_t last_peer_activity = last_activity;
 int64_t forced_until = 0;
 for (;;) {
  /* Keep the receiver's status honest while its peer is powered off. Retain
   * sequence state so a short outage can still resume without discarding it. */
  if (radio_link_stats[LS_STATE] && k_uptime_get() - last_peer_activity >= 300) {
   radio_link_stats[LS_STATE] = 0; radio_link_stats[LS_LINK_LOSS]++;
  }
  if (fault_pause_ms) {
   uint32_t ms = fault_pause_ms; fault_pause_ms = 0; disabled(); k_sleep(K_MSEC(ms));
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
  radio_link_stats[LS_DATA_PACKETS]++;
  if (fault_data) { fault_data--; radio_link_stats[LS_INJECT_DATA]++; continue; }
  if (frame_get32(p + 4) != peer.epoch || !peer.epoch) { acknowledge(p, ARQ_RESET); continue; }
  uint32_t base = frame_get32(p + 8);
  int state = arq_classify(&peer, base, p[12]);
  if (state == ARQ_EXPECTED || state == ARQ_DUPLICATE) {
   last_peer_activity = k_uptime_get(); radio_link_stats[LS_STATE] = 1;
  }
  if (state == ARQ_DUPLICATE) { radio_duplicate += p[12]; acknowledge(p, ARQ_COMMITTED); continue; }
  if (state != ARQ_EXPECTED) { acknowledge(p, ARQ_RESET); continue; }
  if (!arq_unpack_data(p, rx_wire[0], decoded_frames)) { radio_bad_frame++; continue; }
  if (!radio_usb_offer_batch(decoded_frames, p[12])) {
   radio_link_stats[LS_BACKPRESSURE]++; acknowledge(p, ARQ_BUSY); continue;
  }
  arq_commit(&peer, base, p[12]);
  radio_last_sequence = peer.expected - 1; radio_fresh += p[12];
  acknowledge(p, ARQ_COMMITTED);
 }
}
#endif
K_THREAD_DEFINE(link_thread, 4096, worker, NULL, NULL, NULL, 5, 0, 0);

int main(void)
{
#if !RADIO_TX
 int usb = radio_usb_init(); if (usb) return usb;
#endif
 int err = nrf_clock_control_request_sync(DEVICE_DT_GET(DT_NODELABEL(xo)), NULL, K_SECONDS(1));
 if (err) return err;
 uint32_t epoch = new_epoch(); if (!epoch) return -EIO;
 radio_link_stats[LS_LOCAL_EPOCH] = epoch;
 nrf_radio_int_disable(RADIO, UINT32_MAX);
 nrf_radio_mode_set(RADIO, NRF_RADIO_MODE_NRF_4MBIT_BT_0_6);
 nrf_radio_fast_ramp_up_enable_set(RADIO, true);
 nrf_radio_txpower_set(RADIO, NRF_RADIO_TXPOWER_0DBM);
 nrf_radio_packet_conf_t conf = {.lflen = 8, .plen = NRF_RADIO_PREAMBLE_LENGTH_16BIT,
  .maxlen = ARQ_MAX_PAYLOAD, .statlen = 0, .balen = 4, .big_endian = false, .whiteen = false};
 nrf_radio_packet_configure(RADIO, &conf);
 nrf_radio_base0_set(RADIO, 0x8e89bed6); nrf_radio_prefix0_set(RADIO, 0x74);
 nrf_radio_txaddress_set(RADIO, 0); nrf_radio_rxaddresses_set(RADIO, 1);
 nrf_radio_crc_configure(RADIO, 3, NRF_RADIO_CRC_ADDR_INCLUDE, 0x00065b);
 nrf_radio_crcinit_set(RADIO, 0x555555); tune(0);
#if RADIO_TX
 nrf_timer_mode_set(TIMER, NRF_TIMER_MODE_TIMER); nrf_timer_bit_width_set(TIMER, NRF_TIMER_BIT_WIDTH_32);
 nrf_timer_prescaler_set(TIMER, NRF_TIMER_PRESCALER_CALCULATE(NRF_TIMER_BASE_FREQUENCY_GET(TIMER), 16000000));
 nrf_timer_int_enable(TIMER, NRF_TIMER_INT_COMPARE0_MASK);
 IRQ_CONNECT(DT_IRQN(DT_NODELABEL(timer21)), 2, sample_isr, NULL, 0);
 sampling_stop();
#endif
 radio_started = 1;
 printk("R8 ARQv2 %s: 8-sample compact batches, 125 us sampling; diagnostic, unauthenticated\n", RADIO_TX ? "TX" : "RX");
 int64_t heartbeat_at = k_uptime_get() + 1000;
 for (;;) {
  k_sleep(K_MSEC(10));
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
