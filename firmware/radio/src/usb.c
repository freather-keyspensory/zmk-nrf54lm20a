/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <sample_usbd.h>
#include <string.h>
#include "frame.h"

/* Vendor usage page: received diagnostic bytes cannot generate key presses. */
static const uint8_t descriptor[] = {
 0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01,
 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, FRAME_SIZE,
 0x09, 0x01, 0x81, 0x02, 0xc0,
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
/* Called only for fresh validated frames, from RADIO ISR. Never blocks. */
void radio_usb_enqueue(const uint8_t *frame)
{
 if (atomic_get(&radio_usb_bench_state) == 1) return;
 if (!atomic_get(&ready) || k_msgq_put(&frames, frame, K_NO_WAIT)) radio_usb_dropped++;
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
{ ARG_UNUSED(dev); return type == HID_REPORT_TYPE_INPUT && id == 0 ? FRAME_SIZE : -ENOTSUP; }
static int get_report(const struct device *dev, uint8_t type, uint8_t id, uint16_t len, uint8_t *buf)
{ ARG_UNUSED(dev); ARG_UNUSED(type); ARG_UNUSED(id); ARG_UNUSED(len); ARG_UNUSED(buf); return -ENOTSUP; }
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
     radio_usb_errors++;
     if (atomic_get(&radio_usb_bench_state) == 1) radio_usb_bench_errors++;
     atomic_clear_bit(&used, i); k_sem_give(&available);
    } else { radio_usb_submitted++; }
    submitted = true; break;
   }
  }
  if (!submitted) {
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
