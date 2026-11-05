/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <sample_usbd.h>
#include <string.h>
#include "frame.h"

/* Vendor usage page: received diagnostic bytes cannot generate key presses. */
static const uint8_t descriptor[] = {
 0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01,
 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, FRAME_SIZE,
 0x09, 0x01, 0x81, 0x02,
 /* Read-only 96-byte telemetry feature; no output/control commands. */
 0x95, 96, 0x09, 0x02, 0xb1, 0x02, 0xc0,
};
static const struct device *const hid = DEVICE_DT_GET(DT_NODELABEL(radio_hid));
K_MSGQ_DEFINE(frames, FRAME_SIZE, 16, 4);
K_SEM_DEFINE(available, 4, 4);
UDC_STATIC_BUF_DEFINE(buffers, 4 * 64);
static atomic_t ready, used;
/* Explicit SWD-triggered USB load test; synthetic frames never mean keys. */
volatile uint32_t radio_usb_bench_request, radio_usb_bench_completed, radio_usb_bench_errors;
volatile uint64_t radio_usb_bench_elapsed_ns;
static atomic_t radio_usb_bench_state; /* 0 idle, 1 active, 2 done, 3 failed */
static uint64_t bench_first_cycle;
#define BENCH_REPORTS 16000
volatile uint32_t radio_usb_submitted, radio_usb_completed, radio_usb_dropped;
volatile uint32_t radio_usb_errors;
static volatile uint32_t drop_not_ready, drop_queue_full, drop_wait_timeout;
static volatile uint32_t drop_no_buffer, drop_submit_failed, queue_high_water;
extern volatile uint32_t radio_completed, radio_fresh, radio_missing, radio_duplicate;
extern volatile uint32_t radio_bad_crc, radio_bad_frame;
/* Called only for fresh validated frames, from RADIO ISR. Never blocks. */
void radio_usb_enqueue(const uint8_t *frame)
{
 if (atomic_get(&radio_usb_bench_state) == 1) return;
 if (!atomic_get(&ready)) {
  drop_not_ready++; radio_usb_dropped++; return;
 }
 if (k_msgq_put(&frames, frame, K_NO_WAIT)) {
  drop_queue_full++; radio_usb_dropped++; return;
 }
 uint32_t queued = k_msgq_num_used_get(&frames);
 if (queued > queue_high_water) queue_high_water = queued;
}
static void iface_ready(const struct device *dev, bool value)
{ ARG_UNUSED(dev); atomic_set(&ready, value); }
static void done(const struct device *dev, const uint8_t *report)
{
 ARG_UNUSED(dev);
 ptrdiff_t offset = report - buffers;
 if (offset >= 0 && offset < sizeof(buffers) && !(offset % 64)) {
  radio_usb_completed++; atomic_clear_bit(&used, offset / 64);
  if (atomic_get(&radio_usb_bench_state) == 1) {
   uint64_t now = k_cycle_get_64();
   uint32_t count = ++radio_usb_bench_completed;
   if (count == 1) bench_first_cycle = now;
   if (count == BENCH_REPORTS) {
    radio_usb_bench_elapsed_ns = k_cyc_to_ns_floor64(now - bench_first_cycle);
    atomic_set(&radio_usb_bench_state, radio_usb_bench_errors ? 3 : 2);
   }
  }
  k_sem_give(&available);
 }
}
static int get_size(const struct device *dev, uint8_t type, uint8_t id)
{
 ARG_UNUSED(dev);
 if (id != 0) return -ENOTSUP;
 if (type == HID_REPORT_TYPE_INPUT) return FRAME_SIZE;
 return type == HID_REPORT_TYPE_FEATURE ? 96 : -ENOTSUP;
}
static int get_report(const struct device *dev, uint8_t type, uint8_t id, uint16_t len, uint8_t *buf)
{
 ARG_UNUSED(dev);
 if (type != HID_REPORT_TYPE_FEATURE || id != 0) return -ENOTSUP;
 uint32_t words[24];
 words[0] = 0x5453384d; /* M8ST, little endian */
 words[1] = 1;
 words[2] = k_uptime_get_32();
 words[3] = CONFIG_MINEW_RADIO_FREQUENCY_MHZ;
 /* Snapshot only loads while locked; serialize outside the RF critical path. */
 unsigned int key = irq_lock();
 words[4] = radio_completed; words[5] = radio_fresh;
 words[6] = radio_missing; words[7] = radio_duplicate;
 words[8] = radio_bad_crc; words[9] = radio_bad_frame;
 words[10] = radio_usb_submitted; words[11] = radio_usb_completed;
 words[12] = radio_usb_dropped; words[13] = radio_usb_errors;
 words[14] = drop_not_ready; words[15] = drop_queue_full;
 words[16] = drop_wait_timeout; words[17] = drop_no_buffer;
 words[18] = drop_submit_failed; words[19] = k_msgq_num_used_get(&frames);
 words[20] = queue_high_water; words[21] = atomic_get(&used);
 words[22] = atomic_get(&ready); words[23] = 0;
 irq_unlock(key);
 uint8_t report[96];
 for (size_t i = 0; i < ARRAY_SIZE(words); i++) sys_put_le32(words[i], report + i * 4);
 size_t size = MIN(len, sizeof(report));
 memcpy(buf, report, size);
 return size;
}
static const struct hid_device_ops ops = {
 .iface_ready = iface_ready, .input_report_done = done,
 .get_report_size = get_size, .get_report = get_report,
};
static void message(struct usbd_context *ctx, const struct usbd_msg *msg)
{
 if (msg->type == USBD_MSG_VBUS_READY && usbd_can_detect_vbus(ctx)) usbd_enable(ctx);
 if (msg->type == USBD_MSG_VBUS_REMOVED && usbd_can_detect_vbus(ctx)) usbd_disable(ctx);
 if (msg->type == USBD_MSG_RESET || msg->type == USBD_MSG_VBUS_REMOVED ||
     (msg->type == USBD_MSG_CONFIGURATION && !msg->status)) atomic_clear(&ready);
}
int radio_usb_init(void)
{
 if (!device_is_ready(hid)) return -ENODEV;
 int err = hid_device_register(hid, descriptor, sizeof(descriptor), &ops);
 if (err) return err;
 struct usbd_context *ctx = sample_usbd_init_device(message);
 if (!ctx) return -ENODEV;
 return usbd_can_detect_vbus(ctx) ? 0 : usbd_enable(ctx);
}
static void forward(void *a, void *b, void *c)
{
 ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
 uint8_t frame[FRAME_SIZE];
 for (;;) {
  k_msgq_get(&frames, frame, K_FOREVER);
  if (k_sem_take(&available, K_MSEC(2))) {
   drop_wait_timeout++;
   radio_usb_dropped++;
   if (atomic_get(&radio_usb_bench_state) == 1) radio_usb_bench_errors++;
   continue;
  }
  bool submitted = false;
  for (int i = 0; i < 4; i++) {
   if (!atomic_test_and_set_bit(&used, i)) {
    memcpy(buffers + i * 64, frame, FRAME_SIZE);
    int err = atomic_get(&ready) ? hid_device_submit_report(hid, FRAME_SIZE, buffers + i * 64) : -ENODEV;
    if (err) {
     drop_submit_failed++; radio_usb_dropped++;
     radio_usb_errors++;
     if (atomic_get(&radio_usb_bench_state) == 1) radio_usb_bench_errors++;
     atomic_clear_bit(&used, i); k_sem_give(&available);
    } else { radio_usb_submitted++; }
    submitted = true; break;
   }
  }
  if (!submitted) {
   drop_no_buffer++; radio_usb_dropped++;
   radio_usb_errors++;
   if (atomic_get(&radio_usb_bench_state) == 1) radio_usb_bench_errors++;
   k_sem_give(&available);
  }
 }
}
K_THREAD_DEFINE(forward_thread, 2048, forward, NULL, NULL, NULL, 3, 0, 0);

static void benchmark(void *a, void *b, void *c)
{
 ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
 uint8_t frame[FRAME_SIZE];
 for (;;) {
  k_sleep(K_MSEC(20));
  if (radio_usb_bench_request != 1) continue;
  radio_usb_bench_request = 0;
  radio_usb_bench_completed = 0; radio_usb_bench_errors = 0;
  radio_usb_bench_elapsed_ns = 0;
  if (!atomic_get(&ready) || atomic_get(&used) || k_msgq_num_used_get(&frames)) {
   radio_usb_bench_errors = 1; atomic_set(&radio_usb_bench_state, 3); continue;
  }
  atomic_set(&radio_usb_bench_state, 1);
  int64_t deadline = k_uptime_get() + 10000;
  for (uint32_t i = 0; i < BENCH_REPORTS; i++) {
   frame_make(frame, i);
   if (!atomic_get(&ready) || k_uptime_get() >= deadline ||
       k_msgq_put(&frames, frame, K_MSEC(100))) {
    radio_usb_bench_errors++; break;
   }
  }
  while (atomic_get(&radio_usb_bench_state) == 1 && k_uptime_get() < deadline) k_sleep(K_MSEC(5));
  if (atomic_get(&radio_usb_bench_state) == 1) {
   radio_usb_bench_errors++; atomic_set(&radio_usb_bench_state, 3);
  }
 }
}
K_THREAD_DEFINE(benchmark_thread, 2048, benchmark, NULL, NULL, NULL, 4, 0, 0);
