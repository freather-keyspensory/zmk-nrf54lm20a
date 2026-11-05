# First installation and firmware updates

These steps are for the exact tested Minew nRF54LM20A target. They do not establish compatibility with another board, flash geometry, or debugger. Native USB data and correct power wiring must already be verified against its schematic.

## First installation through SWD

Install SEGGER's J-Link software/DLL and the pinned Python environment. Select the intended probe explicitly; do not auto-select an attached device:

```sh
export MINEW_JLINK_SERIAL=YOUR_PROBE_SERIAL
```

The DLL tools check the target's part, 512 KiB RAM, and 2036 KiB RRAM. The tested raw CoreSight path works around target-attachment limitations of an older J-Link OB; behavior with other probes remains to be verified.

1. Build and sign a factory package following `build.md`. Keep the private key securely backed up. Check its layout with `tests/check_packages.py`.
2. Capture a complete checkpointed RRAM backup. The target is halted so its memory stays consistent:

```sh
firmware/port/.venv/bin/python firmware/port/scripts/backup_dll.py \
  firmware/backups/my-board-before-mcuboot
```

3. Only after `complete.json` and the full `rram.bin` exist, program the factory image:

```sh
firmware/port/.venv/bin/python firmware/port/scripts/flash_dll.py \
  firmware/releases/mx54be13-mcuboot/0.1.0+1/factory.bin \
  --backup firmware/backups/my-board-before-mcuboot \
  --logs firmware/backups/my-board-factory-programming
```

Use fresh backup/log directories for a different device or installation. The flasher compares the original bytes with the verified backup, validates controller state, writes and reads back each block, and compares the complete image. It leaves the CPU halted with writes disabled. Do not reset/power-cycle an interrupted or unverified image; inspect the exact failure before further operations. Never use mass erase or debug unprotect as an automatic retry.

4. After complete readback verification, reset the verified board with its known reset control or a guarded debug reset. For the DLL transport:

```sh
firmware/port/.venv/bin/python - <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, 'firmware/port/scripts')
from swd_dll import DllSWD
with DllSWD(Path('firmware/backups/my-board-first-reset')) as swd:
    swd.identify()
    swd.write32(0xe000ed0c, 0x05fa0004)
PY
```

5. Confirm the MCUboot and ZMK boot logs, image confirmation, and actual host behavior. Resolve RTT/symbol addresses from the exact ELF; they can change between builds.

Do not distribute full backups: they contain Bluetooth bond material. The factory tool does not alter UICR/OTP, but changing the boot layout is still a substantial device change. Preserve the backup and working SWD recovery path.

## Updating a verified existing prefix over SWD

The persistent DLL flasher also accepts `--previous-prefix` and
`--previous-proof` together when an earlier full-device backup exists but the
current image has since changed through verified updates. The prefix must be
an actual SWD read covering the entire new write range. Its proof contains
`identity`, `device_id_private` (raw FICR bytes as lowercase hex), `bytes`,
`sha256`, and `readback_sha256`. Both hashes must match that captured prefix.
Never invent a physical-read proof from a desired image file.

The flasher checks the original full backup, verifies the prefix proof and
unique target ID, halts the CPU, and independently compares the current target
bytes with the prefix before writing. Resume plans include the predecessor
hash. This remains an address-zero prefix writer, not an application-slot
relocator. To update an MCUboot application using a combined factory prefix,
verify that its complete bootloader region is byte-identical to the installed
bootloader, that changes are confined to the primary application slot, and
that the new application is signed/linked for the installed layout. Identical
blocks are skipped; every block and the complete result are read back.

An address-zero prefix capture can end partway through the old application;
it is a backup of the write range, not necessarily a complete bootable image.
Preserve the full initial backup and complete signed application packages.
This direct SWD method retains existing swap/confirmation metadata; it does
not automatically create an unconfirmed MCUboot trial. The first Board A
transmitter installation used this path because its USB recovery drive was
not enumerating on the Mac. Its bootloader and settings were retained.

## Backup when the main debug port is protected

Some factory-programmed boards have main-core APPROTECT enabled in UICR while
auxiliary AHB access port 1 remains open. CPU registers may read as all ones
through AP1; that does **not** establish that the CPU is halted. Never infer
blank memory or a halted CPU from a blocked access port.

`backup_aux_dll.py` identifies the nRF54LM20A through AP1, records its unique
device ID privately, saves all 2036 KiB of RRAM plus 4 KiB of UICR, then reads
all RRAM again and compares every byte. It also verifies UICR is unchanged.
It performs no target-memory writes, halt, reset, erase or unlock. The CPU's
state is not observable through this port. The two-pass comparison is required
because application firmware may still be running. If any byte differs, the
tool does not mark the backup complete. Keep the output private.

```sh
firmware/port/.venv/bin/python firmware/port/scripts/backup_aux_dll.py \
  firmware/backups/my-board-factory-aux
```

The default invocation of `recover_aux_dll.py` produces a read-only recovery
plan after verifying the complete RRAM/UICR backup and matching the physical
target's unique ID and protection state:

```sh
firmware/port/.venv/bin/python firmware/port/scripts/recover_aux_dll.py \
  --backup firmware/backups/my-board-factory-aux \
  --logs firmware/backups/my-board-recovery-plan
```

Its separate `--execute-erase-rram-and-uicr` flag is **destructive**. Never invoke
it automatically or without explicit authorization for the backed-up board.
Nordic's [NAN-047 recovery procedure](https://docs.nordicsemi.com/r/bundle/nan_047/page/app/nan_production_programming/nrf54l/nrf54l_disable_approtect.html)
uses CTRL-AP ERASEALL followed by a hard reset. Existing firmware, SRAM and UICR
configuration are erased; OTP is not programmed. This is not a firmware update
and does not preserve the original UICR configuration on the running device.
The utility leaves the recovered CPU halted. Take a new verified baseline
backup before using the normal factory flasher, which compares against the
pre-installation bytes. Do not power-cycle an erased or partly programmed board
and assume a working application is installed.

The AP1 tools are experimental. Document actual backup, plan and recovery
results separately; the existence of the utility is not a hardware test result.

## Subsequent application update through USB

Build/sign with the same private key as the installed bootloader, using a new version/output directory. Enter recovery using the verified retained request or double-reset mechanism, check `INFO_UF2.TXT`, and copy the package's `update.uf2` to `MINEWBOOT`.

The drive disappears when the device reboots. A complete file copy does not itself prove acceptance: MCUboot can reject an invalid signature and boot the existing application. Verify the running image/version and confirmation. Physical reset-button timing still needs verification on the actual board; software double-reset detection has been tested.

Never copy `bootloader.bin`, `factory.bin`, or an address-zero standalone ZMK image as an application update. The USB writer accepts only the bounded application-image path; bootloader/key replacement remains an SWD operation in this experimental implementation.
