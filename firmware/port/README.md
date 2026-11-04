# Minew ZMK port

This workspace contains the Minew board definition, one-key test configuration, legacy kscan compatibility, and guarded SWD tools. Upstream ZMK and Zephyr are fetched separately at pinned revisions; they are not vendored here.

From the repository root, follow `docs/build.md`, then run `bash firmware/port/scripts/build.sh zmk` for the standalone BLE/Studio image. For a bootloader-linked image, use `bash firmware/bootloader/build_zmk.sh` instead. These binaries have different installation layouts.

The native macOS Studio app uses BLE. The test key, Studio unlock, and bootloader requests are explicit debugger actions, not automatic typing. Resolve their RAM addresses from the exact flashed ELF.

The persistent J-Link DLL scripts deliberately avoid target-specific attachment/unlock and check the nRF54LM20A identity. Configure the intended probe explicitly. A complete verified RRAM backup is required for the programming tool. Do not treat the existence of recovery scripts as authorization to erase or change security state.

Current hardware results and remaining limitations are in `docs/testing.md` at the repository root.
