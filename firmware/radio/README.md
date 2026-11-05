# Experimental 8 kHz proprietary radio diagnostics

The opt-in **secure** variant adds wired private pairing and DTLS authentication,
encryption and replay protection to the diagnostic transport. See
[security design, provisioning and current evidence](../../docs/radio-security.md).
It has a separate RF address and build directory. Neither `legacy` nor `reliable`
authenticates its peer. All variants remain diagnostic sample transports.

The opt-in **reliable** transport adds ACK/retry, boot sessions, bounded
USB backpressure, channel reacquisition and automatic restart recovery. See
[reliability design and hardware evidence](../../docs/radio-reliability.md) for
its separate build commands and limitations. The default **legacy** transport
and its historical timing results are documented below. These use different
radio addresses and cannot communicate with one another.

Target: two Minew MX54BE13 boards with nRF54LM20A. One transmits, the other
receives and forwards fresh diagnostic frames to a USB High Speed vendor HID
endpoint. This is the timing bring-up stage, **not a ZMK wireless keyboard
transport yet**. Both targets build in standalone and MCUboot layouts on the
pinned macOS workspace. Local 0.3.0+1 packages passed signature verification,
UF2 reconstruction/bounds checks and tampered-payload rejection by imgtool.
Receiver 0.3.0+1 has been installed temporarily on the original board: it boots,
confirms its trial image and enumerates at USB High Speed (480 Mbps), with a
125 us HID polling interval. The original ZMK 0.2.0+1 was restored afterward.
Both boards now run signed diagnostics. Initial two-board tests at 2480 MHz
delivered approximately 7,934–7,995 fresh host reports/s, with sequence gaps.
The subsequent USB catch-up format passed three separate 60-second steady-state
captures: 1,439,995 fresh frames total, with zero host/RF gaps, CRC errors or USB
drops/errors. These bounded results do not establish recovery under interference.

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
overwrite the working bootloader just to try them. Build and package commands do not flash a board. The separate guarded
`install_uf2.py` tool explicitly copies an update to a uniquely matched recovery
drive; pairing tools explicitly write credentials over trusted wires.

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

## Legacy diagnostic protocol

- Exclusive ownership of RADIO; Bluetooth is disabled. No simultaneous BLE mode.
- Fixed configurable frequency (`CONFIG_MINEW_RADIO_FREQUENCY_MHZ`, currently
  2408 MHz), proprietary 4 Mbps BT=0.6 mode, 0 dBm, five-byte address,
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
- USB uses vendor usage page `0xff00`, 64-byte input reports and a 125 us polling
  interval. Each report contains one frame and either a second queued frame or
  32 zero padding bytes. The receiver never waits to fill a pair. The previous
  32-byte reports had no catch-up capacity when the independent RF and USB
  clocks or scheduling diverged. It cannot inject keyboard input. Experimental VID/PID `2fe3:0009`
  needs a legitimate allocation for production distribution.

The fixed packet airtime is approximately 84 us at 4 Mbps (2-byte preamble,
5-byte address, 32-byte payload, 3-byte CRC). This is a budget estimate, not a
measured airtime result. Hardware tests demonstrated periodic transmission,
reception and USB forwarding. Reception losses and scheduling margin still need
characterization before using this as a keyboard transport.

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
The tool buffers reports and validates after capture, with cyclic garbage
collection paused during reception. Host batching and queue overflow can affect
the result; this is not scan-to-host latency. Compare receiver counters to
distinguish RF errors from later losses.
A receiver USB accessory permission prompt may require Allow on macOS.

The telemetry receiver build adds a read-only, unnumbered HID feature
report (`M8ST`, version 1 is 96 bytes; version 2 is 112 bytes). It snapshots RF counters, USB counters, individual
drop reasons, queue occupancy/high-water mark, buffer ownership and readiness.
It accepts no output commands. Use `capture.py --stats --seconds 30` with this
build; older receiver firmware does not support the feature and will fail the
request. The two feature requests bracket the host capture, but device uptime
and host timing use separate clocks; the reported windows are not identical. Compare bounded deltas,
not cumulative startup drops. Synthetic USB benchmarks deliberately suppress
RF forwarding and must not be interpreted as loss-free RF tests.

Drop reasons separate USB-not-ready, queue-full, buffer-wait timeout, no free
buffer, and submission failure. Submission/no-buffer failures now also increment
the total drop counter; older builds counted those only as errors. The queue
capacity, two-millisecond wait and 125 us endpoint are unchanged in this
measurement build. Do not interpret instrumentation as a forwarding repair.

RX 0.3.0+6 implements this telemetry. Both receiver layouts build, malformed
telemetry/wrap tests pass, and its signed package and UF2 reconstruction pass.
It was installed, confirmed, and matched complete signed-image readback. Its
first 20-second capture identified 17 queue-full drops, with zero new RF missing
sequences, CRC errors or USB submission errors. The 32-byte USB path delivered
159,982 frames while RF received 160,000 fresh frames during its counter window.

RX 0.3.0+7 adds the two-frame USB catch-up format and version-2 telemetry. Input
reports remain polled every 125 us, now with a 64-byte maximum packet verified
on the 480 Mbps High Speed bus. USB frame counters count payload frames, while
`usb_reports_submitted/completed` count actual USB reports. `usb_dual_reports`
records reports containing two frames. The host capture reports both USB report
rate and fresh frame rate; `fresh_reports_per_second` is retained as a legacy
alias for `fresh_frames_per_second`. Do not equate the two rates after batching.

The first 20-second catch-up capture received 160,003 fresh frames with zero
host gaps, RF sequence gaps, CRC errors, USB drops or submission errors. Its
counter window included 39 dual reports. Three subsequent captures produced:

| Separate window | Fresh frames | Fresh frames/s | Host gaps | RF gaps / CRC errors | USB drops / errors | Dual reports in counter window |
|---|---:|---:|---:|---:|---:|---:|
| 60 s, run 1 | 479,994 | 7,999.9 | 0 | 0 / 0 | 0 / 0 | 108 |
| 60 s, run 2 | 480,001 | 8,000.0 | 0 | 0 / 0 | 0 / 0 | 115 |
| 60 s, run 3 | 480,000 | 8,000.0 | 0 | 0 / 0 | 0 / 0 | 119 |

These are separate steady-state
windows after USB was ready. Cumulative startup drops are retained in telemetry
and are not zero. There is no implemented RF acknowledgment/retry mechanism,
so clean windows do not establish recovery under interference or outages.

A separate RF-paused synthetic USB test delivered all 16,000 frames in 15,953
reports without gaps or corruption. Device USB report completions measured
7,999.3/s. The test used the same batching path, then RX was re-enabled. This
maintenance pause is excluded from the subsequent RF measurement windows.
This vendor format is not a standard keyboard report format or a completed ZMK
wireless transport. It preserves diagnostic samples using occasional two-frame
reports; it does not prove one fresh standard keyboard report every microframe.

RX 0.3.0+8 retains that format and corrects the RTT labels to identify frame
completion counts. The feature report separately exposes actual USB report
completion counts. Both receiver layouts and the host protocol tests pass.
RX8 was installed and confirmed, its complete signed image matched readback,
and a software reset retained it with blank scratch and no swap. Its subsequent
20-second capture delivered 160,007 fresh frames with zero host/RF gaps, CRC
errors or USB drops/errors. This final window used 42 two-frame reports in the
receiver counter interval.

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

## Receiver USB load measurement — 2026-11-05

Signed RX **0.3.0+2** adds an explicit SWD-triggered synthetic USB test. It
generates 16,000 numbered 32-byte frames through the same queue, forwarding
thread, buffer pool and USB endpoint used by received RF frames. USB polling
provides backpressure; this bypasses RF and scanning. RF forwarding is paused
while the test is active. Start with an idle receiver and no transmitter.

Writing 1 to `radio_usb_bench_request` starts the test. Resolve its address
from the exact running ELF. `radio_usb_bench_state` is 0 idle, 1 active, 2
complete, or 3 failed. Completion counters and first-to-last hardware-cycle
timing are readable through SWD. The benchmark has a ten-second timeout;
disconnect/cancellation recovery during a benchmark has not been tested.

The host tool opens the vendor HID device before triggering the request, checks
all sequence numbers/payloads and compares host receipt with device completions:

```sh
firmware/port/.venv/bin/python -m pip install -r firmware/radio/requirements-host.txt
firmware/port/.venv/bin/python firmware/radio/tools/measure_usb.py \
  --elf firmware/releases/minew-radio/rx/0.3.0+2/app.elf \
  --nm firmware/toolchains/zephyr-sdk-0.16.8/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm \
  --logs firmware/bootloader/docs/receiver-rate-probe \
  --output firmware/bootloader/docs/receiver-rate.json
```

Use a new log directory for each run. Build/package RX 0.3.0+2 with the same
commands above, changing the version argument, before installation through a
compatible bootloader. Never pass another build's ELF. `passed` in the JSON
means the expected 16,000 reports passed integrity/count checks; inspect the
reported rates separately.

Six approximately two-second bursts were tested on the original board and Mac:

| Run | Device completions/s | Host reports | Host active-span reports/s | Sequence gaps |
|---|---:|---:|---:|---:|
| 1 | 7,999.0 | 16,000 | 7,999.5 | 0 |
| 2 | 7,999.5 | 16,000 | 8,000.2 | 0 |
| 3 | 7,999.6 | 15,956 | 7,978.0 | 44 |
| 4 | 8,000.1 | 16,000 | 8,000.4 | 0 |
| 5 | 7,999.0 | 16,000 | 7,999.5 | 0 |
| 6 | 7,998.6 | 16,000 | 7,999.3 | 0 |

Device totals: 96,000 submissions and completion callbacks, zero recorded USB
submission errors or forwarding-queue drops. Host totals: 95,956 reports; no
malformed frames or duplicate/backward sequences. Run 3's missing reports are
retained as a failure, not treated as a loss-free run.

After run 3, the host reader was changed to buffer received reports and validate
after the burst, reducing work in the read loop. Runs 4–6 used this version and
each received all 16,000 reports. The upstream [HIDAPI 0.15 macOS implementation](https://github.com/libusb/hidapi/blob/hidapi-0.15.0/mac/hid.c#L829-L848)
discards older entries when its small input queue fills. Host stalls are a
plausible explanation for the earlier gaps, but the loss was not conclusively
localized. These tests do not establish zero-loss operation under arbitrary
host load or long-duration stress.

The measured USB path reaches approximately 8 kHz, with fresh synthetic sequence
numbers, on a 480 Mbps High Speed bus. This is not a 480 Mbps payload-throughput
test or an RF/scanning/scan-to-host latency result. The original signed ZMK
0.2.0+1 was restored after the measurements.
The restored signed image matched its package on complete readback, the
bootloader was unchanged, and all pre-existing settings bytes were preserved.
The CPU was running with an idle test-key request at the final audit.

## Work after timing bring-up

Board B was installed as the receiver on 2026-11-05. Its factory main-core
debug was protected, but the auxiliary AHB port was open. Before any erase,
all 2,084,864 RRAM bytes were saved and matched on a second complete read;
4,096 UICR bytes also matched before/after. With explicit owner authorization,
CTRL-AP recovery erased RRAM/UICR and restored main-core debug. A new halted
baseline was verified entirely erased before factory programming.

The complete 211,936-byte bootloader + signed RX 0.3.0+2 image matched on
readback. Board B then booted the receiver, and its USB serial matched the
backed-up FICR identity. USB negotiated High Speed with a 125 us HID endpoint.
A synthetic burst delivered all 16,000 reports: 7,999.8 completions/s on device,
7,999.6 reports/s at the Mac, with no gaps, duplicates, malformed frames, or
recorded device errors/drops. Final audit showed an advancing heartbeat,
`radio_started=1`, CPU running and both debugger request fields idle.

This first factory image has unset trailer magic. MCUboot treats an initial
preprogrammed image as confirmed; an erased image-ok flag is expected here and
is not an unconfirmed UF2 trial. Trial-image rollback on Board B has not been
tested. Board A was subsequently installed with TX 0.3.0+1 through guarded SWD,
preserving its bootloader and settings; the complete programmed prefix matched
on readback. Its USB recovery drive was not enumerating, so UF2 was not used
for that installation.

At 2480 MHz, three host windows delivered about 7,934, 7,935 and 7,995 fresh
reports/s, with 663, 647 and 60 sequence gaps respectively. A separate roughly
five-second receiver counter window recorded 40,138 fresh frames, 16 missing
sequences, six bad CRCs, and zero recorded USB errors/drops. Thus some losses
occurred on the RF path; host capture may add others. Transmitter END events
advanced at approximately 7,999.85/s.

`tools/probe_receiver.py` reads receiver counters using an exact ELF and private
target-identity guard. Its optional RSSI sampling and temporary normal-priority
experiment use narrowly bounded volatile writes, without reset or flash writes.
The priority is restored in `finally`. Unsynchronized RSSI samples were
−45 to −48 dBm. A temporary RADIO priority change did not establish an
improvement and was restored; the default priority remains unchanged.

RX 0.3.0+3 was then installed via UF2 with a 2408 MHz configuration. Its signed
image matched complete readback, the frequency register matched 2408 MHz, and
its trial was confirmed. Matching TX 0.3.0+2 uses the same channel. These version
numbers identify the local test packages; contributors must sign their own.

TX 0.3.0+2 was installed through guarded SWD and the complete 177,568-byte
factory prefix matched readback. Reopening the debug session after CTRL-AP
reset verified the running transmitter at 2408 MHz, with an advancing heartbeat
and approximately 7,999 END events/s in a host-timed ten-second counter window.

| 2408 MHz host window | Fresh reports | Fresh reports/s | Sequence gaps |
|---|---:|---:|---:|
| First, 10 s | 79,995 | 7,999.4 | 1 |
| Second, 10 s; host builds active | 79,142 | 7,914.1 | 860 |
| Third, 10 s; host builds active | 79,474 | 7,946.8 | 528 |
| Fourth, 30 s; host builds finished | 239,733 | 7,991.1 | 270 |

All four captures had zero malformed or duplicate/backward reports. Build load
is a possible contributor to host capture loss, not a demonstrated cause of
each gap. Changing channel has not established loss-free operation. Read the
receiver's RF/USB counters before attributing the remaining loss. Capture
duration is limited to 60 seconds because reports are buffered in memory.

A subsequent approximately 20-second receiver-counter window had zero new RF
missing sequences, CRC errors, malformed frames or repeats, but 19 forwarding
drops. The overlapping host capture delivered 159,986 fresh reports at
7,999.3/s with 20 gaps. These windows are not exactly aligned, and live SWD
counters are not atomic. A separate 100-sample observation found the 16-slot
queue holding 11–14 frames and accumulating nine more drops over approximately
11 seconds. Sustained forwarding needs clock-drift/scheduling management;
increasing buffer capacity alone would not guarantee loss-free operation.

The attempted receiver-reset experiment exposed stale MCUboot scratch metadata,
which selected older firmware despite an earlier observed confirmation flag.
See the RRAM scratch regression in `docs/testing.md`; older installed bootloaders
need the documented patch and a backed-up SWD update. Board B's patched
bootloader subsequently installed and confirmed RX 0.3.0+4 and retained it
through two resets; an unconfirmed RX5 trial also reverted to byte-identical
confirmed RX4, which survived a further reset.

Board A then received the same patched bootloader. Its 128 KiB bootloader
readback matched, its complete signed TX 0.3.0+2 image was unchanged, and all
36 KiB of settings matched the pre-update snapshot. TX2 booted at 2408 MHz
after the initial restart and another software reset, with confirmed primary
and blank scratch metadata. To keep the still-powered receiver accepting test
frames, the transmitter sequence counter was resumed through SWD after each
reset. This manual diagnostic action is not session negotiation. A new host
window started after maintenance delivered 159,985 fresh reports over 20 seconds
(7,999.2/s), with eight gaps and zero malformed or duplicate/backward reports.
Both boards now have the scratch fix. RF keyboard integration and
forwarding-loss repair remain unfinished.

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
