# ZMK keyboard reports over the encrypted receiver link

This opt-in mode runs the pinned ZMK keymap/report engine on Board A and the
USB receiver on Board B. Both are Minew MX54BE13/ME54BS13 nRF54LM20A boards.
The existing USB/BLE/Studio image and earlier radio diagnostics remain separate
builds. The transmitter owns RADIO exclusively; BLE and Studio are disabled in
this mode. Restoring the existing USB/BLE image remains a separate firmware update.

## Report path

The two small published ZMK hooks start the radio service after settings load
and route `zmk_endpoints_send_report()` into the report adapter only when
`CONFIG_MINEW_ZMK_RADIO_TX=y`. Keymaps, layers and behaviors still generate the
reports through ZMK. The default configuration does not execute these hooks.

The adapter queues report transitions and retains the latest keyboard and
consumer state. TIMER21 samples at 125 us, dequeuing a transition or refreshing a
complete current state. A reconnect retires queued pre-session transitions and
refreshes the current state; it does not replay typing accumulated while offline.
Queue errors and retired transitions have explicit transmitter counters.

ARQ wire version 5, a separate PHY address and frame version 3 distinguish
keyboard payloads from the earlier diagnostic patterns. Each received 20-byte
payload carries a bounded report length, reserved zero, report bytes and zero
padding. The pinned profile is HKRO with six keys (9 bytes including ID 1) and
six full 12-bit consumer usages (13 bytes including ID 2). Build assertions check the report sizes; the TX configuration selects this exact
layout. The receiver checks IDs, lengths, reserved
bytes, padding and consumer usage bounds after DTLS authentication.

DTLS pairing, encryption, anti-replay, cumulative ACK and suffix retry remain
from the [authenticated diagnostic transport](radio-security.md). Each packet
contains ten report-state samples; four packets share a cumulative ACK window.
The keyboard variant uses a 150 us packet guard for recovery headroom; the
previous secure diagnostic retains its tested 350 us guard.

The receiver accepts delivery only when both USB interfaces are ready. Its
keyboard interface forwards changed reports, while complete repeated states
refresh link health. An independent worker submits neutral keyboard and consumer
reports after 300 ms without fresh authenticated state, on session change, or
a local delivery failure. Resuming the link restores current state through a
new authenticated session when needed. This is a bounded development watchdog;
USB suspension/disconnection prevents delivery until the host resumes polling.

The second, vendor interface exports sequence/epoch metadata and counters for
sample-delivery measurement. It redacts all 20 key-payload bytes before USB
submission and tags these metadata records as frame version 4. No missing sample
is synthesized: each metadata frame corresponds to one accepted received sample.
Actual key bytes use the standard keyboard interface. Schema 17 keeps keyboard
delivery counters separate from the receiver's real link-loss counter and
identifies the dual-interface readiness gate. Pair record/generation publication
uses the same scheduler lock as snapshots, including when ZMK's maintenance
thread runs below the RF worker. A failed TLS context, changed pair generation
or 300 ms without authenticated ARQ setup feedback returns to handshake setup;
authenticated BUSY replies keep a healthy USB-unready peer alive.

The local bounded `hold-not-ready` fault command can gate RF delivery for at most
10 seconds while leaving the USB control interface available. Zero clears it;
it also expires or clears on reset. This enables testing key replacement during
authenticated HELLO/BUSY setup without physical cable handling. It never disables
DTLS authentication or replay checks.

## Build and install

From the pinned workspace prepared by `firmware/scripts/setup-port.sh`:

```sh
bash firmware/radio/zmk/build.sh
bash firmware/radio/build.sh rx mcuboot keyboard
bash firmware/radio/package.sh tx NEW_VERSION keyboard
bash firmware/radio/package.sh rx NEW_VERSION keyboard
bash firmware/radio/tests/run.sh
```

TX is under `build/tx-mcuboot-keyboard/zephyr/zmk.{elf,bin}` and RX under
`build/rx-mcuboot-keyboard/zephyr/zephyr.{elf,bin}`. The packager selects the correct
stem, verifies the variant, signature, vectors and slot bounds, and creates a
bounded UF2. Use an unused version directory and a private signing key matching
the installed bootloader. Build/package commands do not flash a board.

Both peers must use this keyboard format and the same private, locally provisioned
pair credential. Updates preserve the existing ZMS settings and pair record;
no bootloader replacement or erase is needed. See [pair provisioning](radio-security.md).
Keep backups and use uniquely matched UF2 recovery or guarded SWD. The tested
receiver has experimental VID/PID `2fe3:000a`, vendor usage `ff00:0001`, and
keyboard/consumer usages on a separate HID interface. These identifiers are not
allocated for production distribution.

## Measurement and limits

The receiver's USB High Speed keyboard endpoint advertises a 125 us polling
interval. The radio samples complete ZMK report state at 8 kHz; actual keyboard
USB reports are sent when state changes. These are different rates. Ten-sample
batching can add 1,125 us before encryption/RF, and retransmission can add more.
This phase does not establish 125 us scan-to-host latency or 8,000 independent
keyboard USB changes per second.

Use native macOS captures for sequence continuity and receiver counters for
USB/keyboard errors. The vendor metadata intentionally cannot verify key contents;
actual typing and host key-down/key-up state must be tested separately. The manual
kscan request exercises a mapped key through ZMK; it is not physical switch,
EC or Hall-effect scanning. Active media-key actions, mouse reports, LED output,
NKRO variants, Windows, physical replug/power loss and broader interference/latency
qualification remain outside the currently recorded results.

The initial 350 us-guard keyboard prototype preserved sample sequence continuity
through the RF/security faults, but two five-second windows failed the rate test:
a 64-record tamper burst measured 7,575.80 samples/s and the following window
8,287.51 samples/s as backlog caught up. These are retained failures, not passes.
The revised guard passed the repeated matrix and sustained captures below.


## Verified hardware results — 2026-11-06

The boards are left running signed ZMK radio TX **0.6.0+4** and receiver
**0.6.0+6** (schema 17). The same private pair credential survived both independent
software restarts. The transmitter's complete signed image matched final SWD
readback, its bootloader was unchanged, settings bytes were unchanged since
intentional re-pairing, the primary was confirmed, scratch clear and write-enable off.
The receiver's signed UF2, new capability schema and software restart were checked;
its latest full image was not reread through SWD.

- A mapped manual kscan press/release generated two actual ZMK reports. macOS
  observed key-down and key-up, TextEdit received the mapped A character, and
  the receiver completed two keyboard USB reports with zero delivery errors.
  This was repeated after re-pairing and after both peer restarts.
- During a two-second verified transmitter core halt while the host key was held,
  macOS observed release after **308.3 ms**. It remained released while A
  stayed halted; resuming A automatically restored the authenticated link.
- Re-pairing was exercised during authenticated HELLO/BUSY setup with RF delivery
  temporarily gated. Key publication was atomic; mismatched same-identity
  secrets delivered zero fresh samples in a settled six-second window with the
  readiness gate cleared. B rejected implicit replacement. Explicit replacement
  rejoined without a reset after the key change; repeated provisioning was
  idempotent. Firmware verifies the saved record and unrelated settings digests.
- The final firmware passed **23** capture windows covering baseline, eleven RF
  faults, nine tamper/replay/plaintext cases, previous-session replay and delivery
  after receiver restart. Source and receiver software resets rejoined automatically.
- Its final 60-second window delivered **480,004 snapshots at
  8000.01/s**, with zero host gaps, duplicates,
  malformed metadata, session changes, RF gaps, USB drops/errors, keyboard queue/
  delivery errors or security failures. Source overflow, discarded samples and
  link-loss deltas were also zero; queue high-water since reset was **24 samples**.
- Three preceding 60-second windows on TX 0.6.0+2 / RX 0.6.0+4 qualified the same
  revised timing and report transport, delivering **1,440,012 snapshots** with
  the same zero-loss checks. The later builds add priority-independent key
  publication and setup recovery. All four windows total **1,920,016 snapshots**;
  exact per-window versions are retained in the numeric report.
- Current receiver descriptors show USB High Speed and **125 us / 8 kHz polling**
  on both IN endpoints: vendor metadata at 0x81 and keyboard at 0x82. Vendor
  captures during typing verified frame version 4 and all key bytes redacted.

The default USB/BLE/Studio ZMK build, secure diagnostic builds, reliable TX and
legacy RX regression builds passed. Sanitizer codec tests, 18 Python host tests,
reproduction of the published ZMK hooks against the pinned source, signature/
tampered-image checks and bounded UF2 reconstruction passed.

See [sanitized numeric results](zmk-radio-results.json), including the earlier
failed rate windows. This completes functional ZMK → encrypted radio → USB HID
bring-up on these two boards. Physical scanning, tighter latency and broader
hardware/host qualification remain separate work. A's recovery USB volume did
not enumerate this session, so A used guarded SWD updates; B used its uniquely
matched signed UF2 recovery drive.
