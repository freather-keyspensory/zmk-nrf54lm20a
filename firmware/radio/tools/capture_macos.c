/* SPDX-License-Identifier: MIT */
/* Direct IOHID capture into preallocated memory. No HIDAPI/Python input queue. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct record { uint64_t tick; uint32_t length, reserved; uint8_t data[64]; };
struct header {
 char magic[8]; uint32_t version, record_size, numer, denom;
 uint64_t started, stopped, count, errors, overflow;
};
_Static_assert(sizeof(struct record) == 80, "Capture record ABI");
_Static_assert(sizeof(struct header) == 64, "Capture header ABI");
static struct record *records;
static uint64_t used, capacity, errors, overflow;
static void input(void *ctx, IOReturn result, void *sender,
                  IOHIDReportType type, uint32_t id, uint8_t *data, CFIndex length)
{
 (void)ctx; (void)sender;
 if (result || type != kIOHIDReportTypeInput || id || length < 0 || length > 64) {
  errors++; return;
 }
 if (used == capacity) { overflow++; return; }
 struct record *r = &records[used++];
 r->tick = mach_absolute_time(); r->length = (uint32_t)length;
 memcpy(r->data, data, (size_t)length);
}
static void number(CFMutableDictionaryRef d, CFStringRef key, int value)
{
 CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &value);
 CFDictionarySetValue(d, key, n); CFRelease(n);
}
int main(int argc, char **argv)
{
 if (argc != 5 && argc != 6) { fprintf(stderr, "Usage: capture_macos SECONDS OUTPUT SERIAL READY_FILE\n"); return 2; }
 char *end; double seconds = strtod(argv[1], &end);
 if (*end || seconds <= 0 || seconds > 60) return 2;
 capacity = (uint64_t)(seconds * 10000) + 4096;
 records = calloc((size_t)capacity, sizeof(*records));
 if (!records) return 2;
 IOHIDManagerRef manager = IOHIDManagerCreate(NULL, kIOHIDOptionsTypeNone);
 CFMutableDictionaryRef match = CFDictionaryCreateMutable(NULL, 0,
          &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
 number(match, CFSTR(kIOHIDVendorIDKey), 0x2fe3);
 int product=9;
 if (argc == 6) { char *tail; long p=strtol(argv[5],&tail,10); if (*tail || (p!=9 && p!=10)) return 2; product=(int)p; }
 number(match, CFSTR(kIOHIDProductIDKey), product);
 number(match, CFSTR(kIOHIDPrimaryUsagePageKey), 0xff00);
 CFStringRef serial = CFStringCreateWithCString(NULL, argv[3], kCFStringEncodingUTF8);
 CFDictionarySetValue(match, CFSTR(kIOHIDSerialNumberKey), serial);
 IOHIDManagerSetDeviceMatching(manager, match);
 if (IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone)) return 3;
 CFSetRef devices = IOHIDManagerCopyDevices(manager);
 if (!devices || CFSetGetCount(devices) != 1) { fprintf(stderr, "Expected one receiver\n"); return 3; }
 const void *value; CFSetGetValues(devices, &value);
 IOHIDDeviceRef device = (IOHIDDeviceRef)value;
 if (IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone)) return 3;
 uint8_t buffer[64];
 IOHIDDeviceRegisterInputReportCallback(device, buffer, sizeof(buffer), input, NULL);
 IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
 mach_timebase_info_data_t base; mach_timebase_info(&base);
 struct header h = {{'M','8','C','A','P',0,0,0}, 1, 80, base.numer, base.denom, 0,0,0,0,0};
 h.started = mach_absolute_time();
 if (*argv[4]) {
  FILE *ready = fopen(argv[4], "wx");
  if (!ready) return 4;
  fputs("ready\n", ready); fclose(ready);
 }
 CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
 h.stopped = mach_absolute_time(); h.count = used; h.errors = errors; h.overflow = overflow;
 IOHIDDeviceUnscheduleFromRunLoop(device, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
 IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
 IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
 FILE *out = fopen(argv[2], "wb");
 if (!out || fwrite(&h, sizeof(h), 1, out) != 1 ||
     fwrite(records, sizeof(*records), (size_t)used, out) != used || fclose(out)) return 4;
 CFRelease(devices); CFRelease(manager); CFRelease(match); CFRelease(serial); free(records);
 return errors || overflow ? 5 : 0;
}
