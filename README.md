# ZMK on nRF54LM20A — Minew port and UF2 bootloader

Experimental ZMK firmware and a signed MCUboot/UF2 update path for the **Minew MX54BE13 development board with ME54BS13 nRF54LM20A module**.

The working baseline supports USB HID, BLE HID, and native macOS ZMK Studio. The wired HID endpoint is configured for 8 kHz; measured completion rates were approximately 7.985–8.000 kHz. See [USB test details](docs/usb-hid.md). Native USB High Speed MSC transport is also verified. MCUboot signed boot, complete UF2 upgrades, tampered-image rejection, unconfirmed-trial rollback, and incomplete-copy recovery have passed controlled hardware tests. Physical power-loss and broader stress coverage remain outstanding. See the [test report](docs/testing.md) for exact evidence and limitations.

This is a pinned compatibility port of **ZMK v0.3.0 to a newer Zephyr**, not a claim of upstream nRF54LM20A support or a production bootloader. USB HID is verified on macOS; physical cable replug, host reboot, and Windows remain unverified. The original XIAO target remains unverified on XIAO hardware.

## Repository layout

- `firmware/port/`: Minew board, one-key test configuration, legacy kscan compatibility, and SWD tools.
- `firmware/bootloader/`: UF2 virtual drive, parser, signed MCUboot integration, packaging, and tests.
- `firmware/patches/`: minimal patches against exact upstream revisions.
- `firmware/radio/`: experimental two-board proprietary-radio diagnostics; three separate 60-second captures delivered 1,439,995 fresh frames near 8 kHz with zero recorded RF/USB/host loss after the USB catch-up change. Not yet a ZMK wireless keyboard transport.
- `hardware/`: firmware-facing integration notes. Separate local PCB experiments are not included in this firmware baseline.
- `docs/`: public build/test documentation.

## Start here

1. Read the [build instructions](docs/build.md) and [test status](docs/testing.md).
2. Run `bash firmware/bootloader/tests/run.sh` for host protocol checks.
3. Follow the pinned-source setup and build instructions before considering any device writes.
4. Read the [first-install/update instructions](docs/flashing.md).
5. Read the [bootloader notes](firmware/bootloader/README.md) before signing, packaging, or installing firmware.

The first bootloader installation requires SWD and a full backup. Standalone address-zero images and MCUboot slot-linked images are different. Never use an nRF52840 UF2 image on this module.

## Feedback and AI-assisted contributions

Bug reports and focused contributions are welcome. Include board/module, host OS, probe, source revision, image version, steps to reproduce, and sanitized logs. See [CONTRIBUTING.md](CONTRIBUTING.md).

For AI-assisted work, start with [AGENTS.md](AGENTS.md). It includes a reusable contributor prompt, validation requirements, partition rules, and restrictions against exposing keys or making irreversible device changes.

## Licenses

New repository code is MIT unless stated otherwise. Upstream ZMK, Zephyr, MCUboot and dependency licenses remain applicable. See [THIRD_PARTY.md](THIRD_PARTY.md).
