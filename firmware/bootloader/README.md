# Experimental Minew MCUboot + UF2

This directory contains two different targets:

- `build.sh`: the original validation-only USB application. It mounts `MINEWBOOT` and parses UF2 but never writes firmware.
- `build_mcuboot.sh`: the integrated signed MCUboot bootloader and bounded UF2 secondary-slot writer.

See the root `docs/testing.md` for current hardware results. A build or a mounted drive alone does not prove firmware installation or recovery safety.

## Layout

| Range (end exclusive) | Purpose |
|---|---|
| 0x000000–0x020000 | 128 KiB bootloader |
| 0x020000–0x0f0000 | 832 KiB primary slot |
| 0x0f0000–0x1c0000 | 832 KiB secondary slot |
| 0x1c0000–0x1f4000 | 208 KiB scratch |
| 0x1f4000–0x1fd000 | 36 KiB ZMK settings |

The UF2 writer never uses incoming addresses as unrestricted physical destinations. It translates a bounded logical primary-slot range into the secondary slot and reserves the final sector for MCUboot's trailer. It does not write the bootloader, primary, scratch, UICR/OTP, or settings. MCUboot itself performs the validated swap through scratch.

The signed ZMK image has a 0x800-byte header, with vectors at 0x20800. The Zephyr binary already contains its header placeholder. The package script signs it without adding another header.

## Recovery and update flow

1. Request recovery from ZMK, or reset twice within the bootloader's 700 ms detection window. An invalid/no application also enters recovery.
2. The device mounts as `MINEWBOOT`. Confirm the board ID in `INFO_UF2.TXT`.
3. Copy a compatible signed `update.uf2`. The experimental family ID is `0x9b7316e2`; it is not an upstream-registered allocation.
4. The receiver clears the secondary slot, checks blocks and coverage, writes payloads and reads them back. An invalid or incomplete session is not marked pending.
5. A structurally complete image is marked pending for a test upgrade. The bootloader resets after two seconds. MCUboot verifies the signed image before activation and performs the scratch swap.
6. ZMK confirms the trial image after five heartbeats when Bluetooth is ready. This is a basic health gate; it does not exercise every keyboard feature.

The bootloader uses the same development USB identity as the prototype. Replace the sample VID/PID with legitimately allocated identifiers before distributing a production device. The DWC2 DMA driver reports experimental support.

## Build and package

Follow `docs/build.md` from the repository root. Use your own local ECDSA P-256 development key and protect it. No private signing key is published. A device will accept only images signed by the key matching its installed bootloader.

```
bash firmware/bootloader/build_mcuboot.sh
bash firmware/bootloader/build_zmk.sh
bash firmware/bootloader/package.sh 0.1.0+1
```

The package directory under `firmware/releases/mx54be13-mcuboot/` contains:

- `factory.bin`: bootloader plus signed primary image, for a validated first SWD installation only.
- `update.uf2`: signed application update for an already compatible bootloader.
- `bootloader.bin`: bootloader alone; never copy this to the UF2 drive.
- ELFs and hashes for debugging/verification.

Build outputs are intentionally ignored by Git. UF2 wrapping does not relocate an address-zero ZMK binary. No existing nRF52840 UF2 file or bootloader can be substituted.

## Local debugger test requests

Resolve `minew_test_key_request` from the exact application ELF before writing RAM:

- 1: one mapped key press/release.
- 2: explicitly unlock Studio.
- 3: request this bootloader's recovery mode and reset (MCUboot build only).
- With USB next enabled, commands 4–7 select USB/BLE, test software reconnect, and run a development-only neutral-report benchmark. See root `docs/usb-hid.md`.

Configure the probe explicitly with `MINEW_JLINK_SERIAL` or a local ignored `firmware/port/.probe.json`. Never select an arbitrary attached probe. Keep a full backup and working SWD path before changing boot layout. Historical Commander/recovery scripts are experimental diagnostics; do not invoke destructive recovery automatically.

## Limitations

Basic unconfirmed-trial rollback and incomplete-copy software-reset tests passed. Physical power-loss during swap, repeated update stress, Windows, and physical reset-button behavior still require recorded tests. Development signing is not production key provisioning; no irreversible device protection is enabled. Experimental prototypes and production readiness must not be conflated.

References: [UF2 format](https://github.com/microsoft/uf2), [MCUboot](https://docs.mcuboot.com/), and the pinned Zephyr USB/controller sources.
