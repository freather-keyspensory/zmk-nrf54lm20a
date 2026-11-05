# Instructions for coding agents

Read `README.md`, `docs/build.md`, `docs/testing.md`, and `firmware/bootloader/README.md` before changing firmware. Preserve the `firmware/` and `hardware/` separation.

## Scope and evidence

- Target: Minew MX54BE13/ME54BS13 with nRF54LM20A. Original XIAO goals do not prove XIAO hardware compatibility.
- Use the pinned ZMK, Zephyr, MCUboot, and west dependencies. Keep compatibility patches small. Do not silently upgrade the stack.
- Distinguish source implemented, build passed, host tests passed, and real hardware tests passed. Never infer USB/BLE HID success from enumeration/advertising alone.
- Check the current test report before asserting bootloader/update/rollback readiness. Simulator or host verification is not a power-loss test.
- Radio `legacy` and `reliable` variants have different addresses and build directories. Preserve the default legacy path and explicitly select reliable packages for both peers. Read `docs/radio-reliability.md` before changing that protocol.
- Distinguish 8 kHz diagnostic sample production, 125 us USB polling, batched delivery, and actual keyboard latency. ACK/retry recovery is not evidence that no physical RF packet was lost. On macOS use the direct IOHID collector for acceptance captures and inspect end-to-end sequences; completion callbacks alone do not prove host delivery.

## Device operations

- A normal code change does not authorize flashing a connected device. Use the user's explicit session authorization and identify the intended probe/SoC before writes.
- Preserve the full initial RRAM backup, partition map, settings, and a tested SWD recovery path before changing boot layout.
- Never automatically erase, unlock debug protection, write UICR/OTP, change irreversible security settings, or touch unrelated devices. Recovery tools are explicit destructive utilities, not automatic fixes.
- Do not guess LED, matrix, reset-button, power, or USB pin wiring. Read the actual board schematic and ask for missing physical information.
- Resolve debugger symbol addresses from the exact flashed ELF. Never reuse an address from another build.
- UF2 input addresses are untrusted: only bounded secondary-slot writes are allowed. Keep bootloader/settings out of the update path. Require complete coverage and MCUboot signature validation before activation.
- Retain locking for Studio; use a deliberate unlock action.

## Secrets and public contributions

- Never commit or upload signing keys, full RRAM dumps, Bluetooth bond data, credentials, private logs, desktop screenshots, local toolchains, or downloaded upstream trees.
- Generate private signing keys locally, keep them ignored, and publish only public material. Do not use upstream example private keys for distributed trusted builds.
- Do not claim the experimental VID/PID or UF2 family ID is allocated for production distribution.
- Preserve upstream license notices. Follow each upstream checkout's own contributor instructions when changing or submitting there.
- Do not add human DCO sign-offs on anyone's behalf.

## Validation

Run `bash firmware/bootloader/tests/run.sh` for parser/converter changes. Build the affected target using the documented scripts. For packaging changes, validate signatures, malformed images, slot/header/vector bounds, and UF2 reconstruction. Record actual hardware behavior separately, including failures and remaining tests. Avoid duplicating tests that only restate implementation.

## Starter prompt for contributors

> Read AGENTS.md and the current hardware test report. Reproduce the pinned build without flashing hardware. Identify the smallest change needed for the issue. Keep bootloader, application slots, and persistent settings separate. Add or run meaningful tests for malformed updates and bounds. Do not publish secrets or make irreversible device changes. Report exact revisions, what changed, what you tested, and what remains unverified. Ask only for physical details or authorization that the current session does not already provide.
