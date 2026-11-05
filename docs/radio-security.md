# Authenticated radio diagnostic phase — verified on hardware

The earlier reliable ARQ transport is unauthenticated. The default and
`reliable` builds remain that tested baseline. A `crypto-probe` build runs public
known-answer, alteration-rejection and timing tests before the same unencrypted
ARQv2 diagnostic. It does **not** turn on secure radio or provision a pairing key.

The opt-in secure transport uses the pinned Mbed TLS DTLS implementation,
with a unique, randomly generated pair secret provisioned over a trusted wired
connection. Pair secrets must never be compiled into public firmware, logged,
sent over RF, or committed. Initial provisioning and deliberate replacement
are local maintenance operations; unauthenticated over-air pairing is excluded.

DTLS supplies authenticated session establishment, direction-specific session
keys, record sequence numbers and replay checks. Fresh sessions must be completed
before any application packet is accepted. RF retries must create fresh records
while retaining the ARQ batch identifier, so a lost ACK can recover without
disabling DTLS replay protection or forwarding duplicate samples.

The secure profile is DTLS 1.2 ECDHE-PSK with ChaCha20-Poly1305 and SHA-256,
restricted to that suite and ephemeral P-256. Compilation alone does not prove
successful handshakes or 8 kHz delivery. Fresh session secrets replace a bespoke nonce protocol; application
diagnostic epochs must never be used as cryptographic nonces.
See [DTLS 1.2](https://www.rfc-editor.org/rfc/rfc6347.html),
[ChaCha20-Poly1305 suites](https://www.rfc-editor.org/rfc/rfc7905.html), and
[current key-exchange guidance](https://www.rfc-editor.org/rfc/rfc10015.html).

Acceptance requires actual wrong-key, tamper, replay and previous-session tests;
safe persistent provisioning and re-pairing; both restart recoveries; the existing
RF/USB fault matrix and sustained near-8 kHz sample delivery with security enabled.
No keyboard reports may be forwarded until the authentication gate is tested.
Physical debug access remains trusted in development; debug locks and irreversible
provisioning are outside this phase. Radio jamming cannot be prevented by encryption.

Public-vector probe commands (no device writes):

```sh
bash firmware/radio/build.sh tx mcuboot crypto-probe
bash firmware/radio/package.sh tx NEW_VERSION crypto-probe
```

The probe uses the pinned PSA API and CRACEN-backed random source. The expected
AES-CCM and ChaCha20-Poly1305 outputs are independently generated with the pinned
host Python cryptography library. A successful probe is primitive evidence only;
it is not authenticated pairing or a secure RF throughput result.

## Evidence so far

- On-target AES-CCM and ChaCha20-Poly1305 matched independently generated known
  answers and rejected altered ciphertext, tag, metadata, nonce and wrong key.
- The size-optimized PSA build took approximately 1.032 ms (AES-CCM) or 0.862 ms
  (ChaCha20-Poly1305) for the four primitive operations in a DATA/ACK exchange,
  before RF airtime. That timing cannot support the intended packet budget.
- The speed-optimized DTLS in-memory probe completed its ECDHE-PSK handshake in
  351 ms. Replays, altered tags, previous-session records, reflected records and
  truncated records released no plaintext; valid subsequent records succeeded.
  Wrong-key peers did not establish a session. Encrypt/decrypt record pairs took
  approximately 264 us for 176 bytes and 178 us for 20 bytes on that probe.
  These are measurements without the receiver's USB workload.
- Both secure builds passed; private wired provisioning committed a random pair
  record on each board. A's original settings bytes were unchanged when the
  compatible ZMS backend first mounted. Credential writes verify the stored
  record and the count/digest of every unrelated current settings value.
- The first real encrypted stop-and-wait pair (0.5.0+2) authenticated and accepted
  10,330 samples before source queue overflow. It recorded 41 source-overflow
  samples and 4,106 ambiguous/queued discards. This is a **failed throughput
  result**, not an encrypted 8 kHz pass. Reconnection also needed correction:
  Mbed TLS's `CLIENT_RECONNECT` already resets its context internally, and must
  return to full handshake/authentication before accepting application packets.

- The first windowed receiver (0.5.0+3) still missed packets while decrypting
  and forwarding USB. Its initial capture delivered 7,060 samples before a
  source overflow/stall. This failure is retained. IRQ-driven receive in RX
  0.5.0+4 rearms before decryption and delivered 80,002 samples in 10 seconds,
  with no observed gaps, duplicates, USB drops/errors or security failures.
- RX 0.5.0+4 passed the ordinary RF faults and all replay/plaintext tests, but
  a 64-packet altered-tag burst caused unnecessary channel scanning and a new
  session (7,418 samples/s over that window). RX 0.5.0+6 keeps the physical
  channel stable during well-framed wire traffic without treating that traffic
  as authenticated peer liveness. The repeated 64-packet tamper test then passed
  near 8,001 samples/s in the same authenticated session.

Wire version 4 uses four retained ten-sample DATA packets per
cumulative-ACK window. Batches are sent as soon as produced. The receiver commits
only an in-order batch accepted into its USB queue; a cumulative ACK identifies
the exact missing suffix. Lost ACKs use an authenticated commit-point poll rather
than replaying the whole window. Retransmissions use fresh DTLS records while
retaining original sample bytes and application sequence numbers. No sample is
reconstructed from its pattern or omitted to meet the throughput target.

That window has portable bounds, loss/deduplication, ACK-progress and wrap tests.
Ten-sample
batching can add 1,125 us before encryption/RF, so this is not a 125 us keyboard
latency claim. The experimental secure variant contains no public
probe keys and has no plaintext RF fallback.

Wired pair credentials live in an ignored private `firmware/radio/keys/` file
(mode 0600) and the existing ZMS settings namespace `minew/rf/pair`. A is
provisioned through a UID/image-checked SRAM mailbox and B through its uniquely
matched USB feature interface. Initial provisioning is distinct from explicit
replacement. The mailbox is cleared after processing; telemetry never returns
the secret. Updates must preserve the settings partition and its ZMS geometry.

## Secure implementation and maintenance

Select `secure` explicitly for both boards; it never communicates with the
plaintext variants. Builds use the pinned Mbed TLS/PSA stack, one DTLS 1.2 suite
(`TLS_ECDHE_PSK_WITH_CHACHA20_POLY1305_SHA256`) and ephemeral P-256. Session
resumption is disabled. Cookie-checked client reconnect support is explicitly
enabled on RX; every reconnect requires a complete new authenticated handshake.
The shared crypto provider is serialized across radio and settings operations.

```sh
bash firmware/radio/build.sh tx mcuboot secure
bash firmware/radio/build.sh rx mcuboot secure
# Choose unused local package versions; preserve the exact ELF for SWD symbols.
bash firmware/radio/package.sh tx NEW_VERSION secure
bash firmware/radio/package.sh rx NEW_VERSION secure
python firmware/radio/tools/pairing.py new firmware/radio/keys/my-pair.json
```

Use the workspace virtual environment for host tools. The provisioning CLI
`provision-a --help` requires the exact installed signed image/ELF, toolchain nm,
private target identity proof and a fresh log directory. It checks both target
UID and installed image hash before writing the bounded SRAM mailbox. The
`provision-b --help` command matches the receiver's USB serial to its private
identity proof. Supply the same private pair file to both. The receiver echoes a per-operation transaction token only after processing;
host tools ignore earlier success/failure responses. Initial provisioning
refuses to replace a different credential; intentional rotation uses `--replace`
on each peer. No key is returned by telemetry. Normal updates and resets retain
the key in ZMS; invalid or missing credentials leave RF delivery disabled.

The 248-byte secure feature report contains status/counters only. Schema 12
identifies IRQ-driven RX, corrected channel tracking and transaction-acknowledged
wired provisioning. A completed RF
packet is snapshotted and reception rearmed before decryption and USB work. Only
authenticated application records may advance delivery or peer liveness.
Well-framed CRC-valid traffic can hold the physical channel during corruption
bursts; this is not proof of authentication and cannot prevent radio jamming.

The diagnostic fault interface accepts bounded, local USB commands. Security
injections feed altered ciphertext, saved current/previous-session ciphertext,
or plaintext into the pre-authentication decoder. If the library ever releases
plaintext from a known injected bad/replayed record, the test records a security
failure and resets the connection, even when ARQ would otherwise deduplicate it.
These are on-target decoder tests, not an independent over-air attacker test.

```sh
python firmware/radio/tools/reliability_suite.py \
  --output-dir PRIVATE_NEW_RESULTS --seconds 10 --faults --security-faults \
  --shared --native-macos --restart-rx
```

Add `--old-session` only after a peer restart while RX remained powered and
retained a prior record. Host captures measure fresh sample delivery through the
native macOS IOHID callback. USB uses two 32-byte diagnostic frames per 64-byte
report when available, so 8,000 samples/s commonly yields 4,000 reports/s; the
High Speed endpoint's polling interval is still 125 us. This does not measure
physical key scanning, one keyboard report per microframe, or scan-to-host latency.

Development trust limits: the pair secret is stored unencrypted in settings,
physical SWD access remains trusted/open, and local wired maintenance can rotate
credentials. There is no over-air pairing, key export, debug locking or protection
change in this phase. The experimental USB identifiers are not a production allocation.

A hardware re-pair test found a stale-response bug in the first host provisioner:
when the pair identity stayed the same, a cached success could be mistaken for
completion of a new secret update. RX 0.5.0+7 uses a version-3 provisioning
command with a nonzero transaction token; schema 12 echoes the completed token
alongside status. The host waits for that exact acknowledgement before reporting
success or rejection. A host regression test injects both stale successes and
stale failures, and hardware verifies same-identity wrong-secret rejection,
explicit replacement and idempotent provisioning with the corrected protocol.

For sustained acceptance, inspect the transmitter as well as host delivery.
`probe_transmitter.py` requires a private UID proof and exact installed signed
image/ELF; reads do not halt the core. Its optional `--restart` performs a normal
software reset only after identity, image hash and idle-writer checks. Compare
source overflow/discard/link-loss counters before and after the sustained windows,
and bound source queue high-water. A short near-8 kHz capture alone can conceal a
steadily growing source queue. Restart discontinuities and deliberate mismatched
keys are separate recovery tests, not continuous loss-free capture intervals.

In this session A's USB recovery volume did not enumerate despite running
recovery firmware; its application updates used the guarded SWD path. B's
uniquely matched recovery drive accepted the signed UF2 updates. This radio
phase does not resolve or qualify A's current recovery-USB connection, physical
power-loss safety, Windows behavior or host reboot recovery.

## Final acceptance — 2026-11-06

Board A runs signed secure TX **0.5.0+3** and Board B signed secure RX
**0.5.0+7** (schema 12). The complete final matrix passed **23 capture windows**:
baseline, eleven RF fault cases, nine tamper/current-replay/plaintext cases,
previous-session replay after a verified peer restart, and delivery after an
automatic receiver restart. Both independent software restarts re-established
authenticated delivery using persistent credentials, without reprovisioning or
sequence RAM repair. The transmitter was observed rejoined by the eight-second
post-reset check; this is a bound, not measured reconnect latency.

Three subsequent 60-second native macOS captures delivered **1,440,008 fresh
samples**, at **7999.99–8000.05 samples/s**. Each recorded zero
host sequence gaps, duplicates, malformed frames or session changes, and zero
receiver RF gaps, USB drops/errors or security failures. The transmitter counters
also showed zero new source overflow, discarded samples or link losses across
those sustained windows; queue high-water since restart was **12 samples**.
The largest host callback gap was **8.98 ms**, illustrating why sample
throughput is not a keyboard latency result.

Same-identity wrong-secret peers delivered zero fresh samples during a six-second
observation after settling. Implicit replacement was rejected on both boards;
corrupt records preserved the active credential/link. Explicit replacement
recovered automatically, and repeating the same provisioning was idempotent.
The corrected receiver transaction acknowledgement was verified on hardware.

A's complete signed application matched SWD readback, the patched bootloader hash
was unchanged, the primary image was confirmed, scratch trailer clear, core
running and write-enable off. Its rotated pair record remained present after
restart. B's signed UF2 update, schema 12, key retention and software restart were
checked; its latest full image was not reread over SWD. Final package signatures,
tampered-payload rejection, bounded UF2 reconstruction, secure builds, legacy/
reliable regression builds, sanitizer codecs and 16 Python host tests passed.

See [sanitized numeric results](radio-security-results.json). Earlier failed
throughput, tamper-burst and stale-provisioning results remain recorded separately.
This completes the bounded authenticated diagnostic phase. Real ZMK keyboard
report transport, physical scanning/latency and broader interference/security
qualification remain subsequent work.
