/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zmk/hid.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include "keyboard_frame.h"
BUILD_ASSERT(sizeof(struct zmk_hid_keyboard_report) == RF_KEYBOARD_REPORT_SIZE);
BUILD_ASSERT(sizeof(struct zmk_hid_consumer_report) == RF_CONSUMER_REPORT_SIZE);
K_MSGQ_DEFINE(key_events, RF_REPORT_PAYLOAD, 256, 4);
static uint8_t latest_keyboard[RF_REPORT_PAYLOAD] = {RF_KEYBOARD_REPORT_SIZE, 0, 1};
static uint8_t latest_consumer[RF_REPORT_PAYLOAD] = {RF_CONSUMER_REPORT_SIZE, 0, 2};
static uint32_t refresh;
volatile uint32_t minew_radio_zmk_reports, minew_radio_zmk_queue_errors;
int minew_zmk_radio_send_report(uint16_t page)
{
 uint8_t p[RF_REPORT_PAYLOAD]; const void *report; size_t size;
 if (page == HID_USAGE_KEY) { report = zmk_hid_get_keyboard_report(); size = sizeof(struct zmk_hid_keyboard_report); }
 else if (page == HID_USAGE_CONSUMER) { report = zmk_hid_get_consumer_report(); size = sizeof(struct zmk_hid_consumer_report); }
 else return -ENOTSUP;
 if (!rf_keyboard_payload_make(p, report, size)) return -EINVAL;
 unsigned key = irq_lock();
 memcpy(page == HID_USAGE_KEY ? latest_keyboard : latest_consumer, p, sizeof(p));
 irq_unlock(key);
 int err = k_msgq_put(&key_events, p, K_MSEC(20));
 if (err) minew_radio_zmk_queue_errors++; else minew_radio_zmk_reports++;
 return err;
}
/* TIMER ISR snapshots real ZMK reports. Every queued transition is preserved;
 * idle ticks refresh both complete states, including after an RF reconnect. */
void minew_zmk_radio_sample(uint8_t payload[RF_REPORT_PAYLOAD])
{
 if (!k_msgq_get(&key_events, payload, K_NO_WAIT)) return;
 unsigned key = irq_lock();
 memcpy(payload, (++refresh & 1) ? latest_keyboard : latest_consumer, RF_REPORT_PAYLOAD);
 irq_unlock(key);
}

volatile uint32_t minew_radio_zmk_retired_events;
void minew_zmk_radio_begin_session(void)
{
 unsigned key = irq_lock();
 minew_radio_zmk_retired_events += k_msgq_num_used_get(&key_events);
 k_msgq_purge(&key_events); irq_unlock(key);
}
