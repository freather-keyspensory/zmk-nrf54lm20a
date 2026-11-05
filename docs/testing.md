# Test status — 2026-11-05

Actual hardware: Minew MX54BE13/ME54BS13 nRF54LM20A. Host: Apple Silicon macOS. Probe: an older J-Link OB through a raw CoreSight transport. These results do not establish XIAO or Windows compatibility.

| Check | Result |
|---|---|
| Pinned ZMK/Zephyr port builds | Passed |
| SWD identity, programming, complete image readback | Passed; old probe occasionally has transport errors |
| ZMK boot, scheduler heartbeat | Passed |
| BLE advertising, pairing, encrypted connection | Passed |
| BLE HID character received in TextEdit | Passed |
| Bond persistence and reconnect after software reset | Passed |
| Native macOS Studio discovery, unlock, layout | Passed |
| Studio A→B remap, save, reload after reset, BLE B input | Passed; restored A |
| Native USB High Speed negotiation | Passed, 480 Mbps |
| USB MSC drive mount and INFO file read | Passed |
| USB validation-only UF2 transfer | Passed, both test blocks received |
| Host UF2 parser/converter and sanitizer tests | Passed |
| Virtual FAT image checked by macOS fsck | Passed |
| Signed MCUboot build and signed ZMK package | Passed |
| Signed application booted through MCUboot | Passed |
| Image confirmation after application health check | Passed |
| Retained request enters recovery drive | Passed |
| Complete 900-block UF2 upgrade, scratch swap, ZMK boot | Passed; version 0.1.0+2 read back identical to signed image |
| Hardware rejection of tampered signed update | Passed; existing 0.1.1 image booted |
| Unconfirmed trial-image rollback after software reset | Passed; MCUboot reverted to confirmed 0.1.1 |
| Double-reset detection using software resets | Passed; physical button not yet tested |
| Incomplete UF2 transfer followed by reset | Passed; 100/900 blocks received, reset booted existing firmware |
| Physical power loss during swap / prolonged update stress | Not yet tested |
| USB HID keyboard and USB/BLE switching | Passed on macOS; actual A input verified on each transport |
| USB 125 us / 8 kHz HID endpoint | Host descriptor verified |
| Neutral-report throughput, 16,000 reports/run | 7,985.3–7,999.8 completions/s; zero reported submission errors |
| Software USB detach/reconnect and MCU reboot | Passed, including USB typing afterward |
| Physical cable replug / host reboot | Deferred / untested |
| Physical keyboard matrix, EC/HE scanning | Not tested by this firmware baseline |
| Two-board proprietary-radio diagnostics | TX/RX standalone and MCUboot builds, host codec tests, signed-package verification and UF2 reconstruction passed; RF hardware testing pending |
| Receiver diagnostic single-board USB smoke test | Signed RX 0.3.0+1 booted, heartbeat advanced, image confirmed, installed image matched package, USB High Speed 480 Mbps and 125 us HID interval verified; original ZMK restored |
| J-Link-triggered receiver USB load test | RX 0.3.0+2: 96,000 device completions at 7,998.6–8,000.1/s, zero recorded device errors/drops; host received all 16,000 reports in five of six bursts, one capture had 44 gaps. Final three captures were complete after deferring host parsing. RF remains untested; ZMK restored. See radio README. |
| Second board factory backup via auxiliary AHB port | Passed: 2,084,864 RRAM bytes matched on two complete reads; 4,096 UICR bytes matched before/after. Unique target ID differs from the original ZMK USB board. No target-memory writes or resets. |
| Second board debug recovery | Read-only plan passed against backup and unique target ID. After explicit owner authorization, CTRL-AP recovery erased RRAM/UICR and restored main-core debug; recovered target ID matched, Cortex-M33 identified, CPU halted. No OTP writes. Receiver installation pending. |

Host checks include full UF2 reconstruction of the signed image, bounds/alignment checks, and imgtool rejection of a tampered payload. Hardware rejection of the tampered signed file was also observed; the previously confirmed application remained bootable.

A software reset is not a physical power-cycle test. The initial confirmed boot and successful update do not establish power-loss safety at every swap stage. Bootloader development uses a local signing key without OTP/UICR provisioning. The USB VID/PID and UF2 family ID remain experimental.

Raw device backups and desktop captures are deliberately not published because they can contain personal information and Bluetooth bond material. Report new results with reproducible steps and sanitized excerpts.

## Bootloader hardware sequence

1. Installed MCUboot plus signed ZMK 0.1.0+1 by guarded SWD with complete readback.
2. Booted the signed primary and observed successful application confirmation and encrypted BLE reconnection.
3. Entered recovery with a retained request; copied the complete 900-block 0.1.0+2 UF2. MCUboot reported a test swap, and the installed signed bytes subsequently matched the packaged image exactly.
4. Copied 0.1.1+1 over UF2; ZMK booted and confirmed again.
5. Copied a tampered signed image; MCUboot reported `Image in the secondary slot is not valid!` and booted the existing 0.1.1 application.
6. Triggered two software resets 250 ms apart. The first-reset retained marker was 0x57, and the second boot entered UF2 recovery.
7. Installed a valid trial then reset it before confirmation. MCUboot reported `Swap type: revert` and returned to confirmed 0.1.1.
8. Copied only 100 of 900 UF2 blocks. The primary stayed at 0.1.1; a reset booted it without activating the incomplete secondary.

These are controlled development tests, not a production certification. Anti-rollback provisioning is not enabled; signed older versions can be installed, which was useful for the rollback test. User-visible transfer/error status and broader power-loss/stress coverage remain future work.

## Pre-USB-HID audited state (historical)

The board is left running signed ZMK **0.1.1+1** through MCUboot. The entire installed signed image matched its package on SWD readback. The bootloader region also matched the originally installed bootloader after positive, tampered, rollback, and partial-copy tests. The CPU was running, the test-key request was idle, BLE reconnected, and the final application typed A in an empty TextEdit document. The standalone, non-MCUboot BLE/Studio target was rebuilt successfully as a regression check.

- Signed application SHA-256: `3cd0f1812174adb688f99bb49ebe41d1ba53a907b0a0038497fa0f57750a25be`
- Bootloader SHA-256: `1c488f756478e6e5b6581db49a25607f2dc7a4561edae3ef0e47c63adc10a960`

These hashes identify local development builds; new signing keys or rebuilt dependencies will produce different artifacts. Private keys and full memory images are not published.

## Current USB HID state

The board is running signed **0.2.0+1**, with USB preferred and the High Speed HID endpoint configured for 125 us polling. See [USB HID report](usb-hid.md) for descriptor, throughput, switching and reboot evidence. Physical replug was deferred by the owner. No Windows or host-reboot result is claimed.
