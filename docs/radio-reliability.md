# Wireless reliability and restart recovery

This phase concerns numbered diagnostic samples, not a ZMK keyboard transport.
The prior single-direction baseline passed bounded zero-loss tests but could
not recover lost packets or a restarted transmitter. The new `reliable` build
is opt-in; the tested `legacy` build remains available for restoration.

## Protocol and limits

- TIMER21 produces a sample every 125 us independently of radio completion.
  Delayed timer service is accounted for; sample queue overflow is explicit.
- A DATA packet carries up to eight samples behind a 16-byte header.
  Repeated frame magic, contiguous sequence and session metadata are encoded
  once; all 20 data bytes from each original 32-byte sample are transmitted.
  The receiver copies those bytes back into the full USB sample format. The
  current sender uses eight samples per exchange. A batch waits up to
  seven sample intervals (875 us) before transmission, in addition to radio,
  retransmission and USB delay. This is not a measured scan-to-host latency.
- Stop-and-wait ACKs echo the sender epoch, base sequence and sample count.
  Receiver commit occurs only after accepting the entire batch into the USB
  queue. A lost ACK causes an identical retry; committed duplicates are ACKed
  again without being forwarded twice.
- USB keeps the 64-byte catch-up format and 125 us polling interval. Reliable
  mode applies queue backpressure and retains reports on submission failure
  rather than discarding them after a two-millisecond buffer wait.
- Hardware entropy supplies nonzero diagnostic boot/session identifiers. A
  HELLO/READY exchange precedes sampling. A restarted receiver requests a new
  session; the four most recently retired transmitter sessions cannot switch the receiver back.
- Channel reacquisition uses the configured frequency plus 2444 and 2474 MHz.
  An unacknowledged sender changes channel after 90 ms without valid feedback;
  an idle receiver scans. BUSY ACKs retain the known working channel.
- An idle-listen deadline cannot truncate a packet whose address has already
  matched: reception gets a separate, bounded one-millisecond completion window.
  Receiver link status goes down after 300 ms without a recognized peer, retaining
  sequence state for a short interruption. Valid traffic or HELLO restores it.
- The transmitter holds 4,096 samples (nominally 512 ms of sampling). A batch
  without successful acknowledgment for 300 ms ends the session. Queued or
  ambiguous samples discarded at restart/long-outage boundaries are counted.
  Recovery of fresh sampling is automatic; lossless delivery across arbitrary
  power interruptions or unbounded interference is not claimed.

The implementation uses PHYEND-to-DISABLE and explicit direction changes;
it does not assume hardware TIFS qualification for proprietary 4 Mbps. See
[Nordic transmit sequencing](https://docs.nordicsemi.com/r/bundle/ps_nrf54lm20a/page/radio.html-concept_db5_cnl_4r)
and [TIFS restrictions](https://docs.nordicsemi.com/r/bundle/ps_nrf54lm20a/page/radio.html-ifs).
The exchange timing budget needs hardware measurement, not just packet arithmetic.

This protocol is unauthenticated. It has no pairing, encryption, or protection
against hostile forged packets. Its USB interface is vendor HID and cannot
inject keyboard input. Those constraints must remain until the security and
ZMK integration phases are implemented.

## Build

```sh
bash firmware/radio/tests/run.sh
bash firmware/radio/build.sh tx mcuboot reliable
bash firmware/radio/build.sh rx mcuboot reliable
bash firmware/radio/package.sh tx 0.4.0+3 reliable
bash firmware/radio/package.sh rx 0.4.0+5 reliable
```

Build directories include `-reliable`; package creation checks the selected
configuration. Sign with a local key matching the installed bootloader. The
protocol uses a different radio address from the legacy build; both peers must
be updated. Existing package directories cannot be overwritten.

## Measurement and controlled faults

Feature telemetry versions 3–7 extend the old counters with receiver/sender epochs,
ACKs, duplicate retries, backpressure, channels, injected losses, session changes
and overflow counters. Version 4 identifies compact ARQ wire version 2, version 5
adds the receive-window guard, version 6 adds receiver idle detection, and version 7
identifies the session-purge cancellation fix.
The capture tool understands version-2 sample frames;
an epoch change is reported separately from missing samples within an epoch.

The reliable receiver alone accepts bounded USB feature commands. They never
write arbitrary memory, flash, UICR or OTP. Examples:

```sh
python firmware/radio/tools/capture.py --stats --seconds 30
python firmware/radio/tools/control.py drop-data 8
python firmware/radio/tools/control.py drop-ack 8
python firmware/radio/tools/control.py pause-radio 100
python firmware/radio/tools/control.py channel 2
python firmware/radio/tools/control.py restart
python firmware/radio/tools/control.py bootloader
```

The commands respectively discard the next bounded number of DATA/ACK packets,
pause reception for at most 100 ms, hold a different receive channel for 100 ms
before scanning, restart the receiver, or enter its existing signed UF2 recovery
mode. `bootloader` requires the MCUboot build. Packet-drop counts are limited to
64. Unknown commands, nonzero padding and out-of-range arguments are rejected.
There is no RF command that resets a peer. A channel move tests reacquisition;
it is not a calibrated real-world interference test.

On macOS use the direct IOHID collector for acceptance captures:

```sh
python firmware/radio/tools/reliability_suite.py --output-dir PRIVATE_NEW_DIRECTORY \
  --seconds 10 --faults --restart-rx --shared --native-macos
python firmware/radio/tools/capture.py --stats --seconds 60 --shared --native-macos
```

The native collector is compiled with the installed Apple command-line tools into
the ignored build directory. A direct IOHID callback copies input reports into
preallocated memory; Python validates them afterward. Callback errors and buffer
overflow fail the capture. Raw temporary files are removed after decoding.
HIDAPI still handles feature/control reports, using its public Darwin shared-open
API. The suite opens its controller before capture and keeps it open; it never
creates another controller in the middle of an acceptance window.

The optional Python/HIDAPI input collector remains useful for comparison. Its
32-report native queue can overflow when Python is delayed; it produced sporadic
host gaps despite clean device counters, including six gaps in a repeat run with
a persistent controller. Those captures are failures, not zero-loss evidence.
See [HIDAPI 0.15 macOS implementation](https://github.com/libusb/hidapi/blob/hidapi-0.15.0/mac/hid.c).
Direct IOHID collection removes that particular intermediate queue; finite
captures still cannot guarantee loss-free behavior under arbitrary host load.

The pinned Zephyr HID completion callback does not expose transfer status.
Recorded USB errors cover forwarding/submission errors; completion counts alone
are not proof of delivery or of an error-free physical bus. Acceptance therefore
checks the complete numbered payload stream at the host as well as counters.

Transmitter counters can be read using `tools/probe_transmitter.py` with the exact
flashed `--elf`, matching `--signed-image`, ARM `--nm`, private `--identity` JSON,
and new private `--logs` / `--output` paths. It checks the configured probe,
unique SoC ID and installed image header. `--seconds 30` takes a second snapshot;
explicit `--restart` performs a normal software reset and verifies a new sender
session. It does not halt, flash, erase or change protection. This restart proof
must be paired with a continuous host capture to inspect session boundaries.

## Completion evidence

| Check | Current evidence |
|---|---|
| Codec bounds, wrap, commit/retry, ACK-loss deduplication | Host sanitizer tests passed |
| Malformed telemetry, sessions, bounded host commands, native capture bounds/errors | Ten Python host tests passed, plus sanitizer codec tests |
| Reliable TX/RX, standalone and MCUboot | Builds passed |
| Signed TX/RX packages and bounded UF2 reconstruction | Passed through TX 0.4.0+3 / RX 0.4.0+5 |
| Receiver boots and telemetry/control responds | +5 installed through signed UF2; schema 7 and live two-board link verified |
| Two-board fresh delivery near 8 kHz | Compact wire version 2 passed bounded captures |
| Injected DATA and ACK loss, with no host gaps/duplicates | Final TX +3 / RX +5: baseline plus eleven fault cases passed using native IOHID; DATA/ACK drop counts 1, 8, 32 and 64 |
| Bounded reception interruption and channel reacquisition | 20/100 ms pauses and channel move passed in that final matrix |
| Transmitter and receiver restart recovery without RAM sequence repair | Both passed; receiver recovery observed in 1.535 s, transmitter in 1.287 s. Continuous transmitter-restart capture had one session change, zero within-session gaps and zero duplicates |
| Repeated steady-state runs and final image/boot audit | Three 60 s windows: 1,440,010 fresh samples, zero gaps/duplicates/malformed samples or recorded RF gaps/USB drops/errors. A complete signed-image readback matched; bootloader/settings preserved, image confirmed, scratch clear, core running, write enable off. B schema 7 retained after signed update and reset; USB/RF ready |

The diagnostic reliability and software-restart phase is complete. The
[sanitized numeric results](radio-reliability-results.json) record the exact
packages, windows, failure history, audit scope and limits. The final matrix plus
its post-receiver-restart capture delivered 1,040,050 fresh samples. A separate
source-counter observation recorded no overflow, discarded samples, lost links
or late samples during its stable window. Both live audits identified their
intended boards. The final receiver application was verified through signed UF2
and retained schema after reset; it was not reread through SWD in this audit.

Restart discontinuities and downtime are reported separately; they are not
continuous zero-loss windows. Physical power-loss/replug, sustained real-world
interference/range, Windows and host reboot remain unverified. Pairing/security,
real ZMK keyboard integration, physical scanning and end-to-end latency are
subsequent work. Host callback gaps reached about 9.6 ms without sequence loss;
125 us polling does not establish 125 us host delivery latency.

## Development failure retained

The first 0.4.0+1 pair transmitted seven full 32-byte frames per packet. Its
first ten-second capture delivered only 63,262 fresh frames (about 6.3 kHz),
with four sender-session changes and four host duplicates. Transmitter counters
showed its 4,096-sample queue filling and overflowing. A measured completed
exchange maximum was 1,271 us, while the seven-sample production period was
875 us. This is a failed timing bring-up result, not a reliability pass.

Wire version 2 in 0.4.0+2 removes repeated metadata and carries eight samples in
176 payload bytes, rather than seven samples in 240 bytes. The transmitter also
waits 20 us between successful exchanges for receiver rearming. That revision still failed
larger loss bursts. Revision +3 added the receive-window guard and increased
channel dwell to 90 ms; +4 corrects receiver idle status. Earlier failed captures
are retained privately and excluded from acceptance totals.

Continuous transmitter-restart capture on RX +4 exposed one stale frame from the
previous session. Purging the USB queue wakes a blocked `k_msgq_get` with `-ENOMSG`;
the forwarding thread had ignored that return and submitted its previous stack
frame. RX +5 checks the result and retries the read. The repeated continuous
restart capture observed one session change, zero within-session gaps and zero
duplicates, with an approximately 920 ms report interruption. This interruption
is expected during reboot and is not counted as loss-free continuous service.
