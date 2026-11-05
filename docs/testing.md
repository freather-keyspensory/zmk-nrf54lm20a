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
| Two-board proprietary-radio diagnostics | TX/RX builds, host codec tests, signed-package verification and UF2 reconstruction passed. At 2480 MHz: approximately 7,934–7,995 fresh host reports/s with gaps; TX END events approximately 7,999.85/s. Receiver counters also recorded CRC errors; no loss-free claim. |
| Receiver diagnostic single-board USB smoke test | Signed RX 0.3.0+1 booted, heartbeat advanced, image confirmed, installed image matched package, USB High Speed 480 Mbps and 125 us HID interval verified; original ZMK restored |
| J-Link-triggered receiver USB load test | RX 0.3.0+2: 96,000 device completions at 7,998.6–8,000.1/s, zero recorded device errors/drops; host received all 16,000 reports in five of six bursts, one capture had 44 gaps. Final three captures were complete after deferring host parsing. This synthetic test bypassed RF; ZMK was restored afterward. See radio README. |
| Second board factory backup via auxiliary AHB port | Passed: 2,084,864 RRAM bytes matched on two complete reads; 4,096 UICR bytes matched before/after. Unique target ID differs from the original ZMK USB board. No target-memory writes or resets. |
| Second board debug recovery | Read-only plan passed against backup and unique target ID. After explicit owner authorization, CTRL-AP recovery erased RRAM/UICR and restored main-core debug; recovered target ID matched, Cortex-M33 identified, CPU halted. No OTP writes. |
| Second board receiver installation | Passed: complete 211,936-byte MCUboot + signed RX 0.3.0+2 factory image read back identical; receiver booted, heartbeat advanced, USB serial matched backed-up chip ID; High Speed 480 Mbps and 125 us HID interval verified. |
| Second board receiver USB load | Passed: 16,000 synthetic reports, 7,999.8 device completions/s and 7,999.6 host reports/s; no sequence gaps, duplicates, malformed reports, or recorded device errors/drops. This test bypassed RF. |
| Two-board 2408 MHz configuration | RX 0.3.0+3 installed through UF2, signed-image readback matched and trial confirmed. TX 0.3.0+2 installed through guarded SWD; all 177,568 programmed bytes matched, unchanged bootloader retained. Four captures: 7,914.1–7,999.4 fresh reports/s, with 1–860 gaps. The 30-second window after builds finished delivered 239,733 fresh reports at 7,991.1/s with 270 gaps. No malformed or duplicate/backward reports. Further loss localization pending. |
| 2408 MHz RF versus USB loss localization | Over an approximately 20-second receiver-counter window: zero new RF missing sequences, CRC errors, malformed frames or repeats; 19 new forwarding drops and zero USB submission errors. The overlapping host capture delivered 159,986 fresh reports at 7,999.3/s with 20 gaps. Windows were not exactly aligned and SWD counter reads are not atomic. |
| Receiver forwarding queue occupancy | In 100 live samples over approximately 11 seconds, 11–14 of 16 slots were occupied and nine additional reports were dropped. Clock drift and scheduling pressure remain to be distinguished; larger buffers alone cannot guarantee sustained loss-free operation. |

Host checks include full UF2 reconstruction of the signed image, bounds/alignment checks, and imgtool rejection of a tampered payload. Hardware rejection of the tampered signed file was also observed; the previously confirmed application remained bootable.

A software reset is not a physical power-cycle test. The initial confirmed boot and successful update do not establish power-loss safety at every swap stage. Bootloader development uses a local signing key without OTP/UICR provisioning. The USB VID/PID and UF2 family ID remain experimental.

## RRAM scratch cleanup regression

A later reset of Board B selected stale scratch status and returned from RX
0.3.0+3 at 2408 MHz to RX 0.3.0+2 at 2480 MHz, despite a previously observed
confirmation flag. Complete readback identified the older signed image.
The pinned scratch-swap code used `boot_erase_region()` for scratch setup and
temporary trailer cleanup. That helper intentionally does nothing on erase-free
RRAM, leaving valid scratch magic. The boot log explicitly selected scratch as
its status source. Earlier successful update/confirmation tests did not cover
this later-reset failure and do not establish that old installed bootloaders
are free from this bug.

`firmware/patches/mcuboot-rram-scratch-cleanup.patch` changes those two operations
to the existing scramble helper. The patched bootloader builds, UF2 host tests
pass, and the patch applies to the pinned MCUboot revision. Board B received it
through backed-up SWD programming with complete 128 KiB readback verification;
its application and settings were retained. Scratch magic was then observed
cleared. A new signed RX 0.3.0+4 was installed through UF2 and confirmed, with
complete signed-image readback matching. Two subsequent resets retained RX4;
both boot logs reported `Boot source: none` and `Swap type: none`, with confirmed
primary and blank scratch metadata. An unconfirmed RX 0.3.0+5 trial was then
reset at heartbeat 1, before its five-heartbeat confirmation gate; MCUboot
reported `Swap type: revert`. Complete readback of the returned image matched
signed RX4, with confirmed primary and blank scratch metadata. One further
reset again retained RX4 at 2408 MHz with blank scratch.

Board A subsequently received the same patched bootloader through guarded SWD.
Its original bootloader matched the saved package before replacement, and all
128 KiB of the replacement matched on readback. Its signed TX 0.3.0+2 image
matched complete readback after programming, and all 36 KiB of settings were
byte-identical to the snapshot taken before the update. Both the initial restart
and a further software reset booted TX2 at 2408 MHz. The second boot reported
`Boot source: none` and `Swap type: none`; scratch remained blank and the primary
confirmation flag stayed set. Final audit found an advancing heartbeat, idle
recovery request and RRAM write enable off. Physical power-loss testing remains
outstanding.

The transmitter's volatile sequence counter was deliberately resumed through
SWD after each maintenance reset, using the symbol from its exact ELF, so the
still-powered receiver could accept new diagnostic frames. This is a test
workaround, not implemented session/reconnect negotiation. Maintenance gaps
were excluded by starting a new host capture afterward. That final 20-second
capture delivered 159,985 fresh reports at 7,999.2/s, with eight sequence gaps
and no malformed or duplicate/backward reports. Loss-free forwarding remains
unverified.

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

## Verified ZMK USB HID baseline (before radio tests)

Before the radio diagnostics, Board A ran signed ZMK **0.2.0+1**, with USB preferred and the High Speed HID endpoint configured for 125 us polling. That signed package is preserved locally for restoration. See [USB HID report](usb-hid.md) for descriptor, throughput, switching and reboot evidence. Physical replug was deferred by the owner. No Windows or host-reboot result is claimed.

## Current two-board test state

Both boards have the patched MCUboot bootloader. Board A runs TX **0.3.0+2**;
Board B runs RX **0.3.0+4**, both at 2408 MHz. These are vendor-HID timing
diagnostics, not the ZMK keyboard application. The receiver forwards numbered
test frames and does not inject keyboard input. Forwarding-loss handling,
authenticated pairing/encryption, automatic session recovery and ZMK report
integration remain unfinished.
