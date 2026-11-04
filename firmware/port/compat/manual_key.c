/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT minew_kscan_manual
#include <zephyr/device.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#if IS_ENABLED(CONFIG_ZMK_USB_NEXT)
#include <zmk/endpoints.h>
extern int minew_usb_reconnect(void);
extern void minew_usb_benchmark_start(void);
#endif
#if IS_ENABLED(CONFIG_ZMK_STUDIO)
#include <zmk/studio/core.h>
#endif
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/sys/reboot.h>
#endif
/* Debugger writes 1 for a mapped key press/release, or 2 to unlock Studio. */
volatile uint32_t minew_test_key_request;
static kscan_callback_t callback;
static const struct device *scan_dev;
static struct k_work_delayable poll_work;
static bool enabled, pressed;
static void poll(struct k_work *work) {
 if (!enabled) { return; }
#if IS_ENABLED(CONFIG_ZMK_USB_NEXT)
 if (minew_test_key_request >= 4 && minew_test_key_request <= 7) {
  uint32_t request = minew_test_key_request;
  minew_test_key_request = 0;
  if (request == 4) zmk_endpoints_select_transport(ZMK_TRANSPORT_USB);
  if (request == 5) zmk_endpoints_select_transport(ZMK_TRANSPORT_BLE);
  if (request == 6) minew_usb_reconnect();
  if (request == 7) minew_usb_benchmark_start();
 }
#endif
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
 if (minew_test_key_request == 3) {
  const struct device *retained = DEVICE_DT_GET(DT_NODELABEL(gpregret1));
  const uint8_t request = 0xb1;
  minew_test_key_request = 0;
  if (device_is_ready(retained) && retained_mem_write(retained,0,&request,1)==0)
   sys_reboot(SYS_REBOOT_COLD);
 }
#endif
#if IS_ENABLED(CONFIG_ZMK_STUDIO)
 if (minew_test_key_request == 2) {
  minew_test_key_request = 0;
  zmk_studio_core_unlock();
  printk("MINEW Studio unlocked by SWD request\n");
 }
#endif
 if (pressed) {
  callback(scan_dev, 0, 0, false); pressed = false;
  printk("MINEW test key released\n");
 } else if (minew_test_key_request == 1 && callback) {
  minew_test_key_request = 0;
  callback(scan_dev, 0, 0, true); pressed = true;
  printk("MINEW test key pressed\n");
 }
 k_work_reschedule(&poll_work, K_MSEC(100));
}
static int configure(const struct device *dev, kscan_callback_t cb) {
 callback = cb; return cb ? 0 : -EINVAL;
}
static int enable(const struct device *dev) {
 enabled = true; k_work_reschedule(&poll_work, K_MSEC(100)); return 0;
}
static int disable(const struct device *dev) {
 enabled = false; k_work_cancel_delayable(&poll_work);
 if (pressed && callback) { callback(dev,0,0,false); pressed = false; }
 return 0;
}
static int init(const struct device *dev) {
 scan_dev = dev; k_work_init_delayable(&poll_work, poll); return 0;
}
static const struct kscan_driver_api api = {
 .config=configure, .enable_callback=enable, .disable_callback=disable
};
DEVICE_DT_INST_DEFINE(0, init, NULL, NULL, NULL, POST_KERNEL, 90, &api);

#include <zephyr/init.h>
volatile uint32_t minew_boot_heartbeat;
static struct k_work_delayable heartbeat_work;
static void heartbeat(struct k_work *work) {
 minew_boot_heartbeat++;
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
 if (minew_boot_heartbeat == 5 && bt_is_ready()) {
  int err = boot_write_img_confirmed();
  printk("MINEW image confirmation: %d\n", err);
 }
#endif
 printk("MINEW ZMK heartbeat %u uptime %lld\n", minew_boot_heartbeat,k_uptime_get());
 k_work_reschedule(&heartbeat_work,K_SECONDS(2));
}
static int boot_diagnostics(void) {
 printk("MINEW ZMK board initialization complete\n");
 k_work_init_delayable(&heartbeat_work,heartbeat);
 k_work_reschedule(&heartbeat_work,K_SECONDS(2));
 return 0;
}
SYS_INIT(boot_diagnostics,APPLICATION,90);
