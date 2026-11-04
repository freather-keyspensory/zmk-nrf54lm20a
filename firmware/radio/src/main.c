/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <hal/nrf_radio.h>
#include <hal/nrf_timer.h>
#include <helpers/nrfx_gppi.h>
#include <string.h>
#include "frame.h"
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/sys/reboot.h>
volatile uint32_t radio_recovery_request;
#endif

#define RADIO ((NRF_RADIO_Type *)DT_REG_ADDR(DT_NODELABEL(radio)))
#define TIMER ((NRF_TIMER_Type *)DT_REG_ADDR(DT_NODELABEL(timer21)))
/* S0/LENGTH absent: exactly 32 bytes on air, plus preamble/address/CRC. */
static uint8_t packet[FRAME_SIZE] __aligned(4);
volatile uint32_t radio_completed, radio_fresh, radio_missing, radio_duplicate;
volatile uint32_t radio_bad_crc, radio_bad_frame, radio_late, radio_heartbeat;
volatile uint32_t radio_last_sequence, radio_started;
volatile uint64_t radio_first_cycle, radio_last_cycle;

#if !RADIO_TX
static bool have_sequence;
#endif
#if RADIO_TX
static nrfx_gppi_handle_t pacing;
#endif
#if !RADIO_TX
int radio_usb_init(void);
void radio_usb_enqueue(const uint8_t *frame);
extern volatile uint32_t radio_usb_submitted, radio_usb_completed, radio_usb_dropped, radio_usb_errors;
#endif

static void radio_isr(const void *unused)
{
 ARG_UNUSED(unused);
 if (!nrf_radio_event_check(RADIO, NRF_RADIO_EVENT_END)) return;
 nrf_radio_event_clear(RADIO, NRF_RADIO_EVENT_END);
 radio_completed++;
 uint64_t now = k_cycle_get_64();
 if (radio_completed == 1) radio_first_cycle = now;
 radio_last_cycle = now;
#if RADIO_TX
 /* END leaves TXIDLE. Hardware START is periodic; skip mutation near the
  * next boundary rather than race EasyDMA. Repeat is counted by receiver. */
 unsigned int key = irq_lock();
 nrf_timer_task_trigger(TIMER, NRF_TIMER_TASK_CAPTURE1);
 uint32_t phase = nrf_timer_cc_get(TIMER, 1);
 if (phase < 2000 - 320 && nrf_radio_state_get(RADIO) == NRF_RADIO_STATE_TXIDLE) {
  frame_make(packet, ++radio_last_sequence);
 } else { radio_late++; }
 __DMB();
 irq_unlock(key);
#else
 if (!nrf_radio_crc_status_check(RADIO)) { radio_bad_crc++; return; }
 /* Snapshot promptly: continuous RX reuses EasyDMA storage for the next
  * frame. Validate and queue the snapshot, not a changing DMA buffer. */
 uint8_t received[FRAME_SIZE];
 memcpy(received, packet, sizeof(received));
 if (!frame_valid(received, sizeof(received))) { radio_bad_frame++; return; }
 uint32_t seq = frame_get32(received + 4);
 uint32_t delta = frame_forward_delta(radio_last_sequence, seq);
 if (!have_sequence || delta) {
  if (have_sequence) radio_missing += delta - 1;
  have_sequence = true; radio_last_sequence = seq; radio_fresh++;
  radio_usb_enqueue(received);
 } else { radio_duplicate++; }
#endif
}

int main(void)
{
#if !RADIO_TX
 int usb_err = radio_usb_init();
 if (usb_err) { printk("USB init error %d\n", usb_err); return usb_err; }
#endif
 int err = nrf_clock_control_request_sync(DEVICE_DT_GET(DT_NODELABEL(xo)), NULL, K_SECONDS(1));
 if (err) { printk("HFXO error %d\n", err); return err; }
 nrf_radio_mode_set(RADIO, NRF_RADIO_MODE_NRF_4MBIT_BT_0_6);
 nrf_radio_frequency_set(RADIO, 2480);
 nrf_radio_txpower_set(RADIO, NRF_RADIO_TXPOWER_0DBM);
 nrf_radio_packet_conf_t cfg = {
  .lflen = 0, .s0len = 0, .s1len = 0,
  .plen = NRF_RADIO_PREAMBLE_LENGTH_16BIT,
  .maxlen = FRAME_SIZE, .statlen = FRAME_SIZE, .balen = 4,
  .big_endian = false, .whiteen = false,
 };
 nrf_radio_packet_configure(RADIO, &cfg);
 nrf_radio_base0_set(RADIO, 0x8e89bed6);
 nrf_radio_prefix0_set(RADIO, 0x73);
 nrf_radio_txaddress_set(RADIO, 0);
 nrf_radio_rxaddresses_set(RADIO, 1);
 nrf_radio_crc_configure(RADIO, 3, NRF_RADIO_CRC_ADDR_INCLUDE, 0x00065b);
 nrf_radio_crcinit_set(RADIO, 0x555555);
 nrf_radio_packetptr_set(RADIO, packet);
 IRQ_CONNECT(DT_IRQN(DT_NODELABEL(radio)), 2, radio_isr, NULL, 0);
 nrf_radio_event_clear(RADIO, NRF_RADIO_EVENT_END);
 nrf_radio_int_enable(RADIO, NRF_RADIO_INT_END_MASK);
 irq_enable(DT_IRQN(DT_NODELABEL(radio)));
#if RADIO_TX
 frame_make(packet, 0);
 nrf_timer_mode_set(TIMER, NRF_TIMER_MODE_TIMER);
 nrf_timer_bit_width_set(TIMER, NRF_TIMER_BIT_WIDTH_32);
 nrf_timer_prescaler_set(TIMER, NRF_TIMER_PRESCALER_CALCULATE(NRF_TIMER_BASE_FREQUENCY_GET(TIMER), 16000000));
 nrf_timer_cc_set(TIMER, 0, 2000); /* 125 us at 16 MHz */
 nrf_timer_shorts_enable(TIMER, NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK);
 err = nrfx_gppi_conn_alloc(nrf_timer_event_address_get(TIMER, NRF_TIMER_EVENT_COMPARE0),
                           nrf_radio_task_address_get(RADIO, NRF_RADIO_TASK_START), &pacing);
 if (err) { printk("GPPI error %d\n", err); return err; }
 nrf_radio_task_trigger(RADIO, NRF_RADIO_TASK_TXEN);
 int64_t deadline = k_uptime_get() + 100;
 while (!nrf_radio_event_check(RADIO, NRF_RADIO_EVENT_READY)) {
  if (k_uptime_get() > deadline) { printk("RADIO ready timeout\n"); return -ETIMEDOUT; }
  k_busy_wait(1);
 }
 nrfx_gppi_conn_enable(pacing);
 nrf_timer_task_trigger(TIMER, NRF_TIMER_TASK_CLEAR);
 nrf_timer_task_trigger(TIMER, NRF_TIMER_TASK_START);
#else
 nrf_radio_shorts_set(RADIO, NRF_RADIO_SHORT_READY_START_MASK | NRF_RADIO_SHORT_END_START_MASK);
 nrf_radio_task_trigger(RADIO, NRF_RADIO_TASK_RXEN);
#endif
 radio_started = 1;
 printk("MINEW 4 Mbps %s diagnostic; 125 us target, no BLE/keys/authentication\n", RADIO_TX ? "TX" : "RX");
 for (;;) {
  k_sleep(K_SECONDS(1)); radio_heartbeat++;
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
  /* Bring-up health gate, deliberately independent of a second board. */
  if (radio_heartbeat == 5 && (!RADIO_TX || radio_completed)) {
   int confirm = boot_write_img_confirmed();
   printk("Diagnostic boot confirmation: %d\n", confirm);
  }
  if (radio_recovery_request == 3) {
   const struct device *retained = DEVICE_DT_GET(DT_NODELABEL(gpregret1));
   uint8_t request = 0xb1;
   if (device_is_ready(retained) && !retained_mem_write(retained, 0, &request, 1)) sys_reboot(SYS_REBOOT_COLD);
   radio_recovery_request = 0;
  }
#endif
  printk("end=%u fresh=%u missing=%u dup=%u crc=%u bad=%u late=%u\n",
   radio_completed, radio_fresh, radio_missing, radio_duplicate,
   radio_bad_crc, radio_bad_frame, radio_late);
#if !RADIO_TX
  printk("USB submitted=%u callbacks=%u dropped=%u errors=%u\n",
   radio_usb_submitted, radio_usb_completed, radio_usb_dropped, radio_usb_errors);
#endif
 }
}
