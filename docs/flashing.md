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

## Subsequent application update through USB

Build/sign with the same private key as the installed bootloader, using a new version/output directory. Enter recovery using the verified retained request or double-reset mechanism, check `INFO_UF2.TXT`, and copy the package's `update.uf2` to `MINEWBOOT`.

The drive disappears when the device reboots. A complete file copy does not itself prove acceptance: MCUboot can reject an invalid signature and boot the existing application. Verify the running image/version and confirmation. Physical reset-button timing still needs verification on the actual board; software double-reset detection has been tested.

Never copy `bootloader.bin`, `factory.bin`, or an address-zero standalone ZMK image as an application update. The USB writer accepts only the bounded application-image path; bootloader/key replacement remains an SWD operation in this experimental implementation.
