/* Copyright (c) 2020 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 * Descriptor layout adapted from the pinned ZMK HID report definitions. */
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <string.h>
#include "frame.h"
#include "reliable.h"
#include "keyboard_frame.h"
/* Match the pinned ZMK HKRO=6, consumer FULL=6 reports exactly. */
static const uint8_t descriptor[] = {
 0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,
 0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,
 0x75,0x08,0x95,0x01,0x81,0x03,
 0x15,0x00,0x26,0xff,0x00,0x19,0x00,0x29,0xff,0x75,0x08,0x95,0x06,0x81,0x00,0xc0,
 0x05,0x0c,0x09,0x01,0xa1,0x01,0x85,0x02,
 0x15,0x00,0x26,0xff,0x0f,0x19,0x00,0x2a,0xff,0x0f,0x75,0x10,0x95,0x06,0x81,0x00,0xc0
};
static const struct device *const keyboard = DEVICE_DT_GET(DT_NODELABEL(keyboard_hid));
extern volatile uint32_t radio_security_stats[10], radio_link_stats[LS_COUNT];
static atomic_t ready, used;
K_SEM_DEFINE(keyboard_available, 4, 4);
UDC_STATIC_BUF_DEFINE(buffers, 4*64);
struct key_event { uint32_t epoch; uint8_t payload[RF_REPORT_PAYLOAD]; };
K_MSGQ_DEFINE(events, sizeof(struct key_event), 64, 4);
static uint8_t cached[2][RF_REPORT_PAYLOAD], current[2][RF_REPORT_MAX];
static uint32_t producer_epoch;
static int64_t last_fresh;
static atomic_t overflow;
volatile uint32_t radio_keyboard_submitted, radio_keyboard_completed, radio_keyboard_errors;
volatile uint32_t radio_keyboard_queue_errors, radio_keyboard_failsafe_releases;
static bool authenticated(uint32_t epoch)
{ return radio_security_stats[0] == 2 && radio_link_stats[LS_STATE] &&
         epoch && epoch == radio_link_stats[LS_REMOTE_EPOCH]; }
void radio_keyboard_offer_frame(const uint8_t *frame)
{
 if (!frame_valid(frame, FRAME_SIZE) || frame[2] != 3 || !authenticated(frame_get32(frame+28))) return;
 unsigned key = irq_lock();
 uint32_t epoch = frame_get32(frame+28);
 if (producer_epoch != epoch) { producer_epoch = epoch; memset(cached, 0, sizeof(cached)); k_msgq_purge(&events); }
 last_fresh = k_uptime_get();
 unsigned index = frame[10]-1;
 if (memcmp(cached[index], frame+8, RF_REPORT_PAYLOAD)) {
  struct key_event e = {.epoch = epoch}; memcpy(e.payload, frame+8, RF_REPORT_PAYLOAD);
  if (k_msgq_put(&events, &e, K_NO_WAIT)) { radio_keyboard_queue_errors++; atomic_set(&overflow, 1); }
  else memcpy(cached[index], e.payload, RF_REPORT_PAYLOAD);
 }
 irq_unlock(key);
}
static void iface_ready(const struct device *dev, bool value)
{ ARG_UNUSED(dev); atomic_set(&ready, value); atomic_set(&overflow, 1); }
static void done(const struct device *dev, const uint8_t *report)
{
 ARG_UNUSED(dev); ptrdiff_t offset = report-buffers;
 if (offset >= 0 && offset < sizeof(buffers) && !(offset%64)) {
  radio_keyboard_completed++; atomic_clear_bit(&used, offset/64); k_sem_give(&keyboard_available);
 }
}
static int size(const struct device *dev, uint8_t type, uint8_t id)
{
 ARG_UNUSED(dev); if (type != HID_REPORT_TYPE_INPUT) return -ENOTSUP;
 return id == 1 ? RF_KEYBOARD_REPORT_SIZE : id == 2 ? RF_CONSUMER_REPORT_SIZE : -ENOTSUP;
}
static int get(const struct device *dev, uint8_t type, uint8_t id, uint16_t len, uint8_t *buf)
{
 int n = size(dev,type,id); if (n < 0) return n;
 unsigned key = irq_lock(); n = MIN(n,len); memcpy(buf,current[id-1],n); irq_unlock(key); return n;
}
static const struct hid_device_ops ops = {
 .iface_ready = iface_ready, .input_report_done = done, .get_report_size = size, .get_report = get,
};
int radio_keyboard_init(void)
{
 current[0][0] = 1; current[1][0] = 2;
 return device_is_ready(keyboard) ? hid_device_register(keyboard,descriptor,sizeof(descriptor),&ops) : -ENODEV;
}
static bool send(const uint8_t *p, size_t n)
{
 if (!atomic_get(&ready) || k_sem_take(&keyboard_available,K_MSEC(20))) return false;
 for (unsigned i=0;i<4;i++) if (!atomic_test_and_set_bit(&used,i)) {
  memcpy(buffers+i*64,p,n);
  int err = hid_device_submit_report(keyboard,n,buffers+i*64);
  if (err) { atomic_clear_bit(&used,i); k_sem_give(&keyboard_available); radio_keyboard_errors++; return false; }
  unsigned key=irq_lock(); memcpy(current[p[0]-1],p,n); irq_unlock(key);
  radio_keyboard_submitted++; return true;
 }
 k_sem_give(&keyboard_available); radio_keyboard_errors++; return false;
}
static bool neutral(void)
{
 uint8_t key[RF_KEYBOARD_REPORT_SIZE]={1}, consumer[RF_CONSUMER_REPORT_SIZE]={2};
 bool a=send(key,sizeof(key)), b=send(consumer,sizeof(consumer));
 return a && b;
}
static void worker(void *a, void *b, void *c)
{
 ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
 uint32_t active_epoch=0; bool released=false;
 for (;;) {
  struct key_event e;
  int rc=k_msgq_get(&events,&e,K_MSEC(10));
  unsigned key=irq_lock(); int64_t last=last_fresh; irq_unlock(key);
  bool live=authenticated(radio_link_stats[LS_REMOTE_EPOCH]) && k_uptime_get()-last<300;
  if (atomic_get(&overflow) || !live) {
   if (!released || atomic_get(&overflow)) {
    if (!neutral()) { k_sleep(K_MSEC(1)); continue; }
    radio_keyboard_failsafe_releases++; released=true; active_epoch=0;
    key=irq_lock(); k_msgq_purge(&events); memset(cached,0,sizeof(cached)); atomic_clear(&overflow); irq_unlock(key);
   }
   continue;
  }
  if (rc || !authenticated(e.epoch)) continue;
  if (active_epoch != e.epoch) {
   if (!neutral()) { atomic_set(&overflow,1); continue; }
   active_epoch=e.epoch;
  }
  released=false;
  if (!send(e.payload+2,e.payload[0])) atomic_set(&overflow,1);
 }
}
K_THREAD_DEFINE(keyboard_forward_thread,2048,worker,NULL,NULL,NULL,4,0,0);
void radio_keyboard_snapshot(uint32_t out[7])
{
 out[0]=radio_keyboard_submitted; out[1]=radio_keyboard_completed; out[2]=radio_keyboard_errors;
 out[3]=radio_keyboard_queue_errors; out[4]=radio_keyboard_failsafe_releases; out[5]=atomic_get(&ready);
 out[6]=0;
 for (unsigned i=1;i<RF_KEYBOARD_REPORT_SIZE;i++) if (current[0][i]) out[6]|=1;
 for (unsigned i=1;i<RF_CONSUMER_REPORT_SIZE;i++) if (current[1][i]) out[6]|=2;
}

bool radio_keyboard_is_ready(void) { return atomic_get(&ready); }
