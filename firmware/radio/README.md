# Experimental 8 kHz proprietary radio diagnostics

Target: two Minew MX54BE13 boards with nRF54LM20A. One transmits, the other
receives and forwards fresh diagnostic frames to a USB High Speed vendor HID
endpoint. This is the timing bring-up stage, **not a ZMK wireless keyboard
transport yet**. Both targets build in standalone and MCUboot layouts on the
pinned macOS workspace. Local 0.3.0+1 packages passed signature verification,
UF2 reconstruction/bounds checks and tampered-payload rejection by imgtool.
Receiver 0.3.0+1 has been installed temporarily on the original board: it boots,
confirms its trial image and enumerates at USB High Speed (480 Mbps), with a
125 us HID polling interval. The original ZMK 0.2.0+1 was restored afterward.
TX has not been flashed, and two-board RF delivery remains untested.

Nordic documents [proprietary radio rates up to 4 Mbps](https://www.nordicsemi.com/Products/nRF54LM20A).
That PHY capability is not proof of an 8 kHz keyboard. The target here is a new
numbered frame every 125 us, with separate counters for radio reception, fresh
sequence numbers, losses, repeats, USB submissions and USB completions.

## Build and host checks

```sh
bash firmware/radio/tests/run.sh
bash firmware/radio/build.sh tx
bash firmware/radio/build.sh rx
```

ELFs and binaries are under `firmware/radio/build/{tx,rx}/zephyr/`. These first
diagnostics are **standalone address-zero images**. They cannot be copied to
the existing UF2 drive or installed into the MCUboot application slot. Do not
overwrite the working bootloader just to try them. No script in this directory
flashes a board.

For an already installed compatible bootloader, build the separate slot-linked
variants and sign using your own local key matching that bootloader:

```sh
bash firmware/radio/build.sh tx mcuboot
bash firmware/radio/build.sh rx mcuboot
bash firmware/radio/package.sh tx 0.3.0+1
bash firmware/radio/package.sh rx 0.3.0+1
```

Slot-linked builds are under `build/{tx,rx}-mcuboot/`. Packages are under ignored
`firmware/releases/minew-radio/{tx,rx}/VERSION/`. Package creation verifies the
signature, header, vector address and slot bounds before generating UF2. Preserve
the working signed ZMK package for restoration. The diagnostic confirms a trial
image after five seconds of basic application progress (and TX completions for
TX); this does not establish wireless link health. Writing 3 to the
`radio_recovery_request` RAM symbol resolved from the exact running ELF requests
the existing UF2 recovery mode. Double-reset recovery also belongs to that
bootloader, not this application's USB interface.

## Diagnostic protocol

- Exclusive ownership of RADIO; Bluetooth is disabled. No simultaneous BLE mode.
- Fixed 2480 MHz, proprietary 4 Mbps BT=0.6 mode, 0 dBm, five-byte address,
  16-bit preamble and 24-bit hardware CRC. No ACK/retransmission/channel hopping.
- Fixed 32-byte payload: `M8`, version 1, reserved zero, little-endian 32-bit
  sequence, then 24 deterministic pattern bytes. This is not authentication.
- TIMER21 at 16 MHz triggers RADIO START through GPPI every 2000 ticks.
  TX ramps once and remains enabled between packets. END interrupts prepare the
  next sequence in TXIDLE, skipping mutation if too close to the next START.
- RX uses READY→START and END→START shortcuts. The ISR promptly snapshots the
  received buffer and queues validated, forward sequence numbers without
  blocking. The available copy/ISR margin needs hardware verification.
- A 16-frame queue and four USB buffers absorb small scheduling differences.
  Queue overflow, unavailable USB, and submission failure are observable counters;
  those buffers cannot compensate indefinitely for clock drift or interference.
- USB uses vendor usage page `0xff00`, 32-byte input reports and a 125 us polling
  interval. It cannot inject keyboard input. Experimental VID/PID `2fe3:0009`
  needs a legitimate allocation for production distribution.

The fixed packet airtime is approximately 84 us at 4 Mbps (2-byte preamble,
5-byte address, 32-byte payload, 3-byte CRC). This is a budget estimate, not a
measured timing result. RX initialization and its scheduler heartbeat passed a
single-board smoke test. RF ramp/START behavior, timer-domain routing, timing
margin, reception and sustained USB forwarding remain to be checked on hardware.

Sequence comparisons handle 32-bit wrap, reject repeats/backward values, and
count forward gaps. Restart RX after restarting TX: this diagnostic has no
authenticated session/epoch negotiation. No physical keys or matrix are sampled.

## Measurement

Resolve all counter addresses from each exact flashed ELF. `radio_started` and
`radio_heartbeat` show basic application progress. Read `radio_completed`,
`radio_fresh`, `radio_missing`, `radio_duplicate`, `radio_bad_crc`,
`radio_bad_frame`, and `radio_late` separately. Cycle timestamps bracket all
radio END events, including invalid RX frames; they do not by themselves measure
fresh reception. Concurrent 64-bit reads need a consistent snapshot.

RX additionally exposes `radio_usb_submitted`, `radio_usb_completed`,
`radio_usb_dropped`, and `radio_usb_errors`. Counters wrap at 32 bits. Compare
deltas over bounded intervals, and record both device and host evidence.

With Python `hidapi` installed in a local environment, run:

```sh
python firmware/radio/tools/capture.py --seconds 10
```

The tool opens only a matching vendor HID device and fails on ambiguity.
It reports fresh host delivery rate, gaps, repeats and malformed frames.
Host batching affects timing; this is not scan-to-host latency. Receiver USB
enumeration passed, but no wireless connection or throughput result is claimed.
A receiver USB accessory permission prompt may require Allow on macOS.

## Single-board USB smoke test — 2026-11-05

The original board was updated through its existing UF2 bootloader to signed RX
0.3.0+1. Its installed signed bytes matched the package on SWD readback.
`radio_started` was 1 and the heartbeat advanced from 22 to 37 during that audit.
RTT reported successful image confirmation. macOS approved the new accessory;
both IOKit and libusb reported High Speed. Descriptor inspection showed
`2fe3:0009`, endpoint `0x81`, 32-byte maximum packet and `bInterval=1` (125 us).

Reproduce the read-only descriptor check using:

```sh
firmware/port/.venv/bin/python firmware/port/scripts/inspect_usb_hid.py --pid 0x0009
```

There was no transmitter connected, and radio/USB frame counters remained zero.
This proves enumeration and application progress, not RF reception or sustained
USB data delivery. After the test, the existing UF2 recovery request restored
signed ZMK 0.2.0+1; its USB identity `2fe3:0007` returned at High Speed and RTT
reported successful image confirmation.

The complete restored signed ZMK image matched its saved package byte for byte,
and the bootloader was unchanged. Every pre-existing settings byte was preserved;
the only settings change was a new 16-byte ZMS entry containing the same four-byte
value as the previous entry for that ID. The existing BLE bond reconnected to
macOS. Final audit found the CPU running and the test-key request idle.

## Work after timing bring-up

1. Preserve the original board's working signed ZMK package; identify and back
   up the second board before writes. Install the slot-linked signed diagnostics
   without changing the tested bootloader/settings layout.
2. Measure transmitter period, fresh RX rate, USB completions and host receipt
   with both boards. Check idle, distance, interference and sustained drift;
   optimize actual failures rather than interpreting endpoint descriptors as proof.
3. Add authenticated pairing, encryption, replay protection, persistent unique
   nonce epochs, reconnect handling and lost-link all-keys-release behavior.
   Never forward unauthenticated radio packets as keyboard input.
4. Integrate ZMK report state and the receiver's keyboard HID interface. Design
   feedback/channel management and packet budgets with authentication overhead.
5. Measure actual matrix/EC/HE scanning and end-to-end latency. Decide mode
   switching or explicit coexistence scheduling before enabling BLE alongside RF.

An 8 kHz endpoint, an 8 kHz RF transmitter, 8 kHz fresh host delivery and an
8 kHz physical keyboard scan are separate claims. Each requires its own test.
