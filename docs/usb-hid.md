# USB HID and 8 kHz verification

Tested on the Minew MX54BE13/ME54BS13 nRF54LM20A board and Apple Silicon macOS, with signed application **0.2.0+1** installed through UF2.

## USB implementation

ZMK v0.3.0 uses Zephyr's older USB device API. The port adds an opt-in `CONFIG_ZMK_USB_NEXT` adapter in `firmware/port/compat/usb_next.c` for the nRF54LM20A USB HS controller. It preserves ZMK's HID report descriptor, keymap, and endpoint selection. The legacy transport remains available when the new adapter is disabled.

The keyboard advertises report protocol, with interrupt IN endpoint 0x81 and a 64-byte maximum packet. It uses four aligned asynchronous report buffers so the USB completion path does not require the key-processing work queue to wait for every packet. The device supports the current keyboard/consumer report configuration; BIOS boot protocol is not enabled.

Host-read configuration:

| Field | Observed |
|---|---|
| Link speed | High Speed, 480 Mbps |
| Interface class | HID (3) |
| Endpoint | Interrupt IN, 0x81 |
| Maximum packet | 64 bytes |
| High Speed bInterval | 1 |
| Polling interval | 125 microseconds |
| Nominal polling rate | 8,000 Hz |

The device uses an experimental development USB identity (VID 0x2fe3, PID 0x0007). These are not a production allocation. Full Speed interrupt polling is clamped by Zephyr to at least 1 ms; 8 kHz requires a High Speed link.

## Measured report throughput

A development-only command sends 16,000 all-keys-released keyboard reports. It does not generate 16,000 letter keypresses. Firmware measures time between the first and last USB completion callbacks using Zephyr's hardware cycle counter.

| Run | Reports | Completion rate | Reported submission errors |
|---|---:|---:|---:|
| Initial | 16,000 | 7,999.5/s | 0 |
| After software USB reconnect | 16,000 | 7,999.8/s | 0 |
| After MCU reboot | 16,000 | 7,985.3/s | 0 |

This verifies the configured 8 kHz endpoint and approximately 7.985–8.000 kHz report-completion throughput in these runs. It is not a host application event-latency measurement, an electrical bus-analyzer capture, or proof of 8 kHz physical matrix/EC/HE scanning. The current manual test scanner polls its debugger command every 100 ms. Normal ZMK sends reports when input changes; the endpoint interval is not a promise to generate empty reports continuously.

## Functional checks

- Selected USB output, generated one mapped A press/release, and received A in an empty TextEdit document. USB completions increased by two with zero reported submission errors.
- Selected BLE output and received A, while USB report completions remained unchanged for that key.
- Selected USB again, disabled/re-enabled the USB device in software, and observed automatic BLE fallback during disconnection and return to preferred USB after reconnection. A sent during fallback arrived over BLE.
- Reset the MCU through the debugger. MCUboot launched 0.2.0, USB re-enumerated with the 125 us endpoint, USB remained preferred, and another A press/release reached TextEdit.
- Standalone and MCUboot-linked firmware variants both build successfully.

Physical cable replug was explicitly deferred by the owner. Host macOS reboot, Windows, electrical power-loss, suspend/remote-wakeup behavior, and physical keyboard scanning remain separate tests. Software detach and MCU reset must not be reported as physical cable or host reboot tests.

Studio's BLE transport remains present. Select BLE output when connecting Studio over BLE; USB Studio/CDC transport is not implemented by this adapter.

## Reproduce the read-only descriptor check

Install libusb and PyUSB on the host, then run:

```
firmware/port/.venv/bin/python firmware/port/scripts/inspect_usb_hid.py
```

On Apple Silicon, the helper finds Homebrew's libusb library if it is not on the standard search path. Set `LIBUSB_LIBRARY` for another location. It does not claim the HID interface.

## Development commands

Resolve `minew_test_key_request` from the exact flashed ELF. Commands 1–3 retain their existing meaning; with the USB adapter enabled:

- 4: prefer USB output.
- 5: prefer BLE output.
- 6: software USB disconnect, followed by reconnect after one second.
- 7: run the neutral-report benchmark. Use only with the manual test scanner and no other key activity or cable changes.

The benchmark helper requires an explicit ELF, nm executable, new log directory, and output path. It writes only the deliberate RAM command after verifying the target identity. Keep the ELF matched to the flashed image.
