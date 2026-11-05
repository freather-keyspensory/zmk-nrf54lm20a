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
#if IS_ENABLED(CONFIG_MINEW_RADIO_RELIABLE)
#include "reliable.h"
extern volatile uint32_t radio_link_stats[LS_COUNT];
int radio_link_control(uint32_t op, uint32_t value);
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
#include "secure_pair.h"
extern volatile uint32_t radio_security_stats[10];
#define FEATURE_SIZE 248
#else
#define FEATURE_SIZE 208
#endif
#else
#define FEATURE_SIZE 112
#endif
#define USB_REPORT_SIZE DT_PROP(DT_NODELABEL(radio_hid), in_report_size)
BUILD_ASSERT(USB_REPORT_SIZE == 32 || USB_REPORT_SIZE == 64);

/* Vendor usage page: received diagnostic bytes cannot generate key presses. */
static const uint8_t descriptor[] = {
 0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01,
 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, USB_REPORT_SIZE,
 0x09, 0x01, 0x81, 0x02,
 /* Telemetry; reliable mode additionally accepts bounded diagnostic controls. */
 0x95, FEATURE_SIZE, 0x09, 0x02, 0xb1, 0x02, 0xc0,
};
static const struct device *const hid = DEVICE_DT_GET(DT_NODELABEL(radio_hid));
K_MSGQ_DEFINE(frames, FRAME_SIZE, 16, 4);
K_SEM_DEFINE(available, 4, 4);
UDC_STATIC_BUF_DEFINE(buffers, 4 * 64);
static atomic_t ready, used;
static uint8_t buffer_frames[4];
/* Explicit SWD-triggered USB load test; synthetic frames never mean keys. */
volatile uint32_t radio_usb_bench_request, radio_usb_bench_completed, radio_usb_bench_errors;
volatile uint64_t radio_usb_bench_elapsed_ns;
volatile uint32_t radio_usb_bench_reports, radio_usb_bench_first_frames;
static atomic_t radio_usb_bench_state; /* 0 idle, 1 active, 2 done, 3 failed */
static uint64_t bench_first_cycle;
#define BENCH_REPORTS 16000
/* Frame counts; a USB completion may deliver one or two frames. */
volatile uint32_t radio_usb_submitted, radio_usb_completed, radio_usb_dropped;
volatile uint32_t radio_usb_errors;
static volatile uint32_t usb_reports_submitted, usb_reports_completed, usb_dual_reports;
static volatile uint32_t drop_not_ready, drop_queue_full, drop_wait_timeout;
static volatile uint32_t drop_no_buffer, drop_submit_failed, queue_high_water;
extern volatile uint32_t radio_completed, radio_fresh, radio_missing, radio_duplicate;
extern volatile uint32_t radio_bad_crc, radio_bad_frame;
#if IS_ENABLED(CONFIG_MINEW_RADIO_RELIABLE)
#if IS_ENABLED(CONFIG_MINEW_RADIO_KEYBOARD)
static int64_t ready_hold_until;
int radio_usb_hold_ready(uint32_t ms)
{
 if (ms > 10000) return -EINVAL;
 unsigned key = irq_lock(); ready_hold_until = ms ? k_uptime_get()+ms : 0; irq_unlock(key);
 return 0;
}
#endif
bool radio_usb_is_ready(void)
{
#if IS_ENABLED(CONFIG_MINEW_RADIO_KEYBOARD)
 extern bool radio_keyboard_is_ready(void);
 unsigned key = irq_lock(); bool hold = k_uptime_get() < ready_hold_until; irq_unlock(key);
 if (hold || !radio_keyboard_is_ready()) return false;
#endif
 return atomic_get(&ready) && atomic_get(&radio_usb_bench_state) != 1;
}
uint32_t radio_usb_begin_session(void)
{
 uint32_t discarded = k_msgq_num_used_get(&frames);
 k_msgq_purge(&frames);
 return discarded;
}
bool radio_usb_offer_batch(const uint8_t *data, size_t count)
{
 if (!count || count > ARQ_BATCH || !radio_usb_is_ready()) return false;
 /* Single RF producer. Prevent another thread producer entering mid-batch;
  * interrupts remain enabled and no packet is acknowledged before acceptance. */
 k_sched_lock();
 if (k_msgq_num_free_get(&frames) < count) { k_sched_unlock(); return false; }
 for (size_t i = 0; i < count; i++) {
  int err = k_msgq_put(&frames, data + i * FRAME_SIZE, K_NO_WAIT);
  __ASSERT_NO_MSG(!err);
 }
 uint32_t queued = k_msgq_num_used_get(&frames);
 if (queued > queue_high_water) queue_high_water = queued;
 k_sched_unlock();
 return true;
}
#endif
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
  uint8_t count_frames = buffer_frames[offset / 64];
  radio_usb_completed += count_frames; usb_reports_completed++;
  atomic_clear_bit(&used, offset / 64);
  if (atomic_get(&radio_usb_bench_state) == 1) {
   uint64_t now = k_cycle_get_64();
   uint32_t count = radio_usb_bench_completed += count_frames;
   radio_usb_bench_reports++;
   if (count == count_frames) {
    bench_first_cycle = now; radio_usb_bench_first_frames = count_frames;
   }
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
 if (type == HID_REPORT_TYPE_INPUT) return USB_REPORT_SIZE;
 return type == HID_REPORT_TYPE_FEATURE ? FEATURE_SIZE : -ENOTSUP;
}
static int get_report(const struct device *dev, uint8_t type, uint8_t id, uint16_t len, uint8_t *buf)
{
 ARG_UNUSED(dev);
 if (type != HID_REPORT_TYPE_FEATURE || id != 0) return -ENOTSUP;
 uint32_t words[FEATURE_SIZE / 4];
 words[0] = 0x5453384d; /* M8ST, little endian */
 words[1] = IS_ENABLED(CONFIG_MINEW_RADIO_KEYBOARD) ? 17 : IS_ENABLED(CONFIG_MINEW_RADIO_SECURE) ? 12 : IS_ENABLED(CONFIG_MINEW_RADIO_RELIABLE) ? 7 : 2;
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
 words[24] = usb_reports_submitted; words[25] = usb_reports_completed;
 words[26] = usb_dual_reports; words[27] = USB_REPORT_SIZE;
#if IS_ENABLED(CONFIG_MINEW_RADIO_RELIABLE)
 for (size_t i = 0; i < LS_COUNT; i++) words[28 + i] = radio_link_stats[i];
#endif
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
 for (size_t i = 0; i < 10; i++) words[52 + i] = radio_security_stats[i];
 /* Read completion status and token together, never a cached earlier success. */
 words[51] = radio_pair_completed_token;
 words[60] = radio_pair_mailbox.response; words[61] = radio_pair_mailbox.generation;
#endif
#if IS_ENABLED(CONFIG_MINEW_RADIO_KEYBOARD)
 extern void radio_keyboard_snapshot(uint32_t out[7]);
 uint32_t keyboard_stats[7]; radio_keyboard_snapshot(keyboard_stats);
 /* Only reuse TX-only counters in this receiver profile; retain the real
  * receiver link-loss counter at word 49. Flags use TX sample-late's slot. */
 memcpy(words+44, keyboard_stats, 5*sizeof(uint32_t));
 words[50] = keyboard_stats[5] | (keyboard_stats[6]<<1);
 if (k_uptime_get() < ready_hold_until) words[50] |= 8;
#endif
 irq_unlock(key);
 uint8_t report[FEATURE_SIZE];
 for (size_t i = 0; i < ARRAY_SIZE(words); i++) sys_put_le32(words[i], report + i * 4);
 size_t size = MIN(len, sizeof(report));
 memcpy(buf, report, size);
 return size;
}
#if IS_ENABLED(CONFIG_MINEW_RADIO_RELIABLE)
static int verify_control(const struct device *dev, uint8_t type, uint8_t id, uint16_t len)
{ ARG_UNUSED(dev); return type == HID_REPORT_TYPE_FEATURE && id == 0 && len == FEATURE_SIZE ? 0 : -EINVAL; }
static int set_control(const struct device *dev, uint8_t type, uint8_t id, uint16_t len, const uint8_t *buf)
{
#if IS_ENABLED(CONFIG_MINEW_RADIO_SECURE)
 if (!verify_control(dev, type, id, len) && !memcmp(buf, "R8CT", 4) &&
     sys_get_le32(buf+4) == 3 && sys_get_le32(buf+8) == 7 && sys_get_le32(buf+12) <= 1) {
  for (size_t i = 84; i < FEATURE_SIZE; i++) if (buf[i]) return -EINVAL;
  return rf_pair_queue(buf+16, sys_get_le32(buf+12) == 1, sys_get_le32(buf+80));
 }
#endif
 if (verify_control(dev, type, id, len) || memcmp(buf, "R8CT", 4) || sys_get_le32(buf + 4) != 1) return -EINVAL;
 for (size_t i = 16; i < FEATURE_SIZE; i++) if (buf[i]) return -EINVAL;
 return radio_link_control(sys_get_le32(buf + 8), sys_get_le32(buf + 12));
}
#endif
static const struct hid_device_ops ops = {
 .iface_ready = iface_ready, .input_report_done = done,
 .get_report_size = get_size, .get_report = get_report,
#if IS_ENABLED(CONFIG_MINEW_RADIO_RELIABLE)
 .verify_set_report = verify_control, .set_report = set_control,
#endif
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
#if IS_ENABLED(CONFIG_MINEW_RADIO_KEYBOARD)
 extern int radio_keyboard_init(void);
 int keyboard_err = radio_keyboard_init(); if (keyboard_err) return keyboard_err;
#endif
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
  /* A new RF session purges the queue and wakes waiters with -ENOMSG.
   * Never submit the previous stack frame after that cancelled read. */
  if (k_msgq_get(&frames, frame, K_FOREVER)) continue;
  if (k_sem_take(&available, IS_ENABLED(CONFIG_MINEW_RADIO_RELIABLE) ? K_FOREVER : K_MSEC(2))) {
   drop_wait_timeout++;
   radio_usb_dropped++;
   if (atomic_get(&radio_usb_bench_state) == 1) radio_usb_bench_errors++;
   continue;
  }
  bool submitted = false;
  for (int i = 0; i < 4; i++) {
   if (!atomic_test_and_set_bit(&used, i)) {
    uint8_t count_frames = 1;
    memset(buffers + i * 64, 0, USB_REPORT_SIZE);
    memcpy(buffers + i * 64, frame, FRAME_SIZE);
    /* Never wait to fill a batch: use the spare slot only for queued data.
     * A zero second slot means absent, not a repeated/synthetic RF sample. */
    if (USB_REPORT_SIZE == 64 && !k_msgq_get(&frames, buffers + i * 64 + FRAME_SIZE, K_NO_WAIT)) count_frames++;
#if IS_ENABLED(CONFIG_MINEW_RADIO_KEYBOARD)
    extern void radio_keyboard_offer_frame(const uint8_t *frame);
    for (unsigned f = 0; f < count_frames; f++) {
     uint8_t *received = buffers+i*64+f*FRAME_SIZE;
     radio_keyboard_offer_frame(received);
     /* Keep sequence/epoch telemetry one-for-one with received snapshots,
      * but never export key bytes on the unprivileged vendor interface. */
     received[2] = 4; memset(received+8, 0, 20);
    }
#endif
    buffer_frames[i] = count_frames;
    int err;
    do {
     if (IS_ENABLED(CONFIG_MINEW_RADIO_RELIABLE)) while (!atomic_get(&ready)) k_sleep(K_MSEC(1));
     err = atomic_get(&ready) ? hid_device_submit_report(hid, USB_REPORT_SIZE, buffers + i * 64) : -ENODEV;
     if (err && IS_ENABLED(CONFIG_MINEW_RADIO_RELIABLE)) { radio_usb_errors++; k_sleep(K_MSEC(1)); }
    } while (err && IS_ENABLED(CONFIG_MINEW_RADIO_RELIABLE));
    if (err) {
     drop_submit_failed += count_frames; radio_usb_dropped += count_frames;
     radio_usb_errors++;
     if (atomic_get(&radio_usb_bench_state) == 1) radio_usb_bench_errors++;
     atomic_clear_bit(&used, i); k_sem_give(&available);
    } else {
     radio_usb_submitted += count_frames; usb_reports_submitted++;
     if (count_frames == 2) usb_dual_reports++;
    }
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
  radio_usb_bench_reports = 0; radio_usb_bench_first_frames = 0;
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
