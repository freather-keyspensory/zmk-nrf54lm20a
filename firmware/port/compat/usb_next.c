/* SPDX-License-Identifier: MIT */
/* ZMK report/endpoint adapter for Zephyr's next-generation USB device stack. */
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <sample_usbd.h>
#include <zmk/hid.h>
#include <zmk/usb.h>
#include <zmk/usb_hid.h>
#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <string.h>
#include <errno.h>

#define TX_COUNT 4
#define TX_SIZE 64
static const struct device *const hid = DEVICE_DT_GET(DT_NODELABEL(minew_usb_hid));
static struct usbd_context *usb;
static atomic_t configured, powered, suspended, tx_used;
static enum usb_dc_status_code status = USB_DC_UNKNOWN;
static uint32_t idle_ms[4];
static int64_t last_report[4];
UDC_STATIC_BUF_DEFINE(tx_data, TX_COUNT * TX_SIZE);
K_SEM_DEFINE(tx_available, TX_COUNT, TX_COUNT);
volatile uint32_t minew_usb_tx_submitted, minew_usb_tx_completed, minew_usb_tx_errors;

static void notify_work(struct k_work *work)
{
 ARG_UNUSED(work);
 raise_zmk_usb_conn_state_changed((struct zmk_usb_conn_state_changed){
  .conn_state = zmk_usb_get_conn_state()});
}
K_WORK_DEFINE(notifier, notify_work);
static void notify(void) { k_work_submit(&notifier); }

enum usb_dc_status_code zmk_usb_get_status(void) { return status; }
bool zmk_usb_is_hid_ready(void) { return atomic_get(&configured) && !atomic_get(&suspended); }
enum zmk_usb_conn_state zmk_usb_get_conn_state(void)
{
 if (zmk_usb_is_hid_ready()) return ZMK_USB_CONN_HID;
 return atomic_get(&powered) ? ZMK_USB_CONN_POWERED : ZMK_USB_CONN_NONE;
}

static void iface_ready(const struct device *dev, bool ready)
{
 ARG_UNUSED(dev);
 atomic_set(&configured, ready);
 if (ready) { atomic_set(&powered, 1); status = USB_DC_CONFIGURED; }
 printk("MINEW USB HID interface %s\n", ready ? "ready" : "not ready");
 notify();
}
#if IS_ENABLED(CONFIG_MINEW_MANUAL_KEY)
static atomic_t minew_usb_bench_state;
volatile uint32_t minew_usb_bench_completed, minew_usb_bench_errors;
volatile uint64_t minew_usb_bench_elapsed_ns;
static uint64_t bench_first_cycle, bench_last_cycle;
#define BENCH_REPORTS 16000
#endif
static void report_done(const struct device *dev, const uint8_t *report)
{
 ARG_UNUSED(dev);
 ptrdiff_t offset = report - tx_data;
 if (offset >= 0 && offset < sizeof(tx_data) && offset % TX_SIZE == 0) {
  atomic_clear_bit(&tx_used, offset / TX_SIZE);
  minew_usb_tx_completed++;
#if IS_ENABLED(CONFIG_MINEW_MANUAL_KEY)
  if (atomic_get(&minew_usb_bench_state) == 1) {
   uint32_t count = ++minew_usb_bench_completed;
   if (count == 1) bench_first_cycle = k_cycle_get_64();
   if (count == BENCH_REPORTS) bench_last_cycle = k_cycle_get_64();
  }
#endif
  k_sem_give(&tx_available);
 }
}
static int send_report(const void *data, size_t len)
{
 if (!zmk_usb_is_hid_ready()) return -ENODEV;
 if (len > TX_SIZE || !len) return -EINVAL;
 if (k_sem_take(&tx_available, K_MSEC(30))) return -EBUSY;
 for (int i = 0; i < TX_COUNT; i++) {
  if (!atomic_test_and_set_bit(&tx_used, i)) {
   memcpy(tx_data + i * TX_SIZE, data, len);
   int err = hid_device_submit_report(hid, len, tx_data + i * TX_SIZE);
   if (err) {
    atomic_clear_bit(&tx_used, i); k_sem_give(&tx_available); minew_usb_tx_errors++;
   } else {
    minew_usb_tx_submitted++;
    uint8_t id = *(const uint8_t *)data;
    if (id < ARRAY_SIZE(last_report)) last_report[id] = k_uptime_get();
   }
   return err;
  }
 }
 k_sem_give(&tx_available);
 return -EIO;
}
int zmk_usb_hid_send_keyboard_report(void)
{
 struct zmk_hid_keyboard_report *r = zmk_hid_get_keyboard_report();
 return send_report(r, sizeof(*r));
}
int zmk_usb_hid_send_consumer_report(void)
{
 struct zmk_hid_consumer_report *r = zmk_hid_get_consumer_report();
 return send_report(r, sizeof(*r));
}
#if IS_ENABLED(CONFIG_ZMK_POINTING)
int zmk_usb_hid_send_mouse_report(void)
{
 struct zmk_hid_mouse_report *r = zmk_hid_get_mouse_report();
 return send_report(r, sizeof(*r));
}
#endif
/* This adapter advertises report protocol only, not BIOS boot protocol. */
void zmk_usb_hid_set_protocol(uint8_t protocol) { ARG_UNUSED(protocol); }

static int report_size(const struct device *dev, uint8_t type, uint8_t id)
{
 ARG_UNUSED(dev);
 if (type != HID_REPORT_TYPE_INPUT) return -ENOTSUP;
 if (id == ZMK_HID_REPORT_ID_KEYBOARD) return sizeof(struct zmk_hid_keyboard_report);
 if (id == ZMK_HID_REPORT_ID_CONSUMER) return sizeof(struct zmk_hid_consumer_report);
 return -ENOTSUP;
}
static int get_report(const struct device *dev, uint8_t type, uint8_t id,
                      uint16_t len, uint8_t *buf)
{
 int size = report_size(dev, type, id);
 if (size < 0) return size;
 const void *report = id == ZMK_HID_REPORT_ID_KEYBOARD ?
  (void *)zmk_hid_get_keyboard_report() : (void *)zmk_hid_get_consumer_report();
 size = MIN(size, len); memcpy(buf, report, size); return size;
}
static void idle_work(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(idle_timer, idle_work);
static void set_idle(const struct device *dev, uint8_t id, uint32_t duration)
{
 ARG_UNUSED(dev);
 for (int i = 1; i <= 2; i++) if (!id || id == i) idle_ms[i] = duration;
 k_work_reschedule(&idle_timer, K_MSEC(4));
}
static uint32_t get_idle(const struct device *dev, uint8_t id)
{
 ARG_UNUSED(dev); return id < ARRAY_SIZE(idle_ms) ? idle_ms[id] : 0;
}
static void idle_work(struct k_work *work)
{
 ARG_UNUSED(work);
 int64_t now = k_uptime_get();
 if (zmk_usb_is_hid_ready()) {
  if (idle_ms[1] && now - last_report[1] >= idle_ms[1]) zmk_usb_hid_send_keyboard_report();
  if (idle_ms[2] && now - last_report[2] >= idle_ms[2]) zmk_usb_hid_send_consumer_report();
 }
 if (idle_ms[1] || idle_ms[2]) k_work_reschedule(&idle_timer, K_MSEC(4));
}
static const struct hid_device_ops ops = {
 .iface_ready = iface_ready, .input_report_done = report_done,
 .get_report_size = report_size, .get_report = get_report,
 .set_idle = set_idle, .get_idle = get_idle,
};
static void usb_message(struct usbd_context *ctx, const struct usbd_msg *msg)
{
 switch (msg->type) {
 case USBD_MSG_VBUS_READY:
  atomic_set(&powered, 1); status = USB_DC_CONNECTED;
  if (usbd_can_detect_vbus(ctx)) usbd_enable(ctx);
  break;
 case USBD_MSG_VBUS_REMOVED:
  atomic_clear(&powered); atomic_clear(&configured); atomic_clear(&suspended);
  status = USB_DC_DISCONNECTED;
  if (usbd_can_detect_vbus(ctx)) usbd_disable(ctx);
  break;
 case USBD_MSG_RESET:
  atomic_clear(&configured); atomic_clear(&suspended); status = USB_DC_RESET; break;
 case USBD_MSG_CONFIGURATION:
  status = msg->status ? USB_DC_CONFIGURED : USB_DC_RESET;
  if (!msg->status) atomic_clear(&configured);
  break;
 case USBD_MSG_SUSPEND: atomic_set(&suspended, 1); status = USB_DC_SUSPEND; break;
 case USBD_MSG_RESUME: atomic_clear(&suspended); status = USB_DC_RESUME; break;
 case USBD_MSG_UDC_ERROR:
 case USBD_MSG_STACK_ERROR: atomic_clear(&configured); status = USB_DC_ERROR; break;
 default: return;
 }
 printk("MINEW USB %s\n", usbd_msg_type_string(msg->type));
 notify();
}
static void reconnect(struct k_work *work)
{
 ARG_UNUSED(work); if (usb) usbd_enable(usb);
}
K_WORK_DELAYABLE_DEFINE(reconnect_work, reconnect);
int minew_usb_reconnect(void)
{
 if (!usb) return -ENODEV;
 int err = usbd_disable(usb);
 if (!err) { atomic_clear(&configured); notify(); k_work_reschedule(&reconnect_work, K_SECONDS(1)); }
 return err;
}
static int init_usb(void)
{
 if (!device_is_ready(hid)) return -ENODEV;
 int err = hid_device_register(hid, zmk_hid_report_desc, sizeof(zmk_hid_report_desc), &ops);
 if (err) return err;
 usb = sample_usbd_init_device(usb_message);
 if (!usb) return -ENODEV;
 if (!usbd_can_detect_vbus(usb)) { atomic_set(&powered, 1); return usbd_enable(usb); }
 return 0;
}
SYS_INIT(init_usb, APPLICATION, CONFIG_ZMK_USB_INIT_PRIORITY);

#if IS_ENABLED(CONFIG_MINEW_MANUAL_KEY)
K_SEM_DEFINE(bench_trigger, 0, 1);
static void benchmark(void *a, void *b, void *c)
{
 ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
 for (;;) {
  k_sem_take(&bench_trigger, K_FOREVER);
  struct zmk_hid_keyboard_report report = { .report_id = ZMK_HID_REPORT_ID_KEYBOARD };
  /* Development-only load test. Every report means all keys released. */
  minew_usb_bench_completed = 0; minew_usb_bench_errors = 0;
  minew_usb_bench_elapsed_ns = 0;
  int acquired = 0;
  for (; acquired < TX_COUNT; acquired++)
   if (k_sem_take(&tx_available, K_MSEC(500))) break;
  bool ready = acquired == TX_COUNT && zmk_usb_is_hid_ready();
  for (int i = 0; i < acquired; i++) k_sem_give(&tx_available);
  if (!ready) { atomic_set(&minew_usb_bench_state, 3); continue; }
  atomic_set(&minew_usb_bench_state, 1);
  for (int i = 0; i < BENCH_REPORTS; i++) {
   if (send_report(&report, sizeof(report))) { minew_usb_bench_errors++; break; }
  }
  acquired = 0;
  for (; acquired < TX_COUNT; acquired++)
   if (k_sem_take(&tx_available, K_MSEC(500))) break;
  if (minew_usb_bench_completed == BENCH_REPORTS && !minew_usb_bench_errors) {
   minew_usb_bench_elapsed_ns = k_cyc_to_ns_floor64(bench_last_cycle - bench_first_cycle);
   atomic_set(&minew_usb_bench_state, 2);
  } else { atomic_set(&minew_usb_bench_state, 3); }
  for (int i = 0; i < acquired; i++) k_sem_give(&tx_available);
  printk("MINEW USB benchmark: %u completions, %llu ns, %u errors\n",
         minew_usb_bench_completed, minew_usb_bench_elapsed_ns, minew_usb_bench_errors);
 }
}
K_THREAD_DEFINE(benchmark_thread, 2048, benchmark, NULL, NULL, NULL, 10, 0, 0);
void minew_usb_benchmark_start(void) { k_sem_give(&bench_trigger); }
#endif
