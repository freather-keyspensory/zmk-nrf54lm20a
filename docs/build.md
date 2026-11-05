# Build on macOS

The tested host is Apple Silicon macOS with Python 3.13, CMake, Ninja, dtc, and Zephyr SDK 0.16.8's ARM GCC 12.2 toolchain. This port uses that older compiler against the pinned newer Zephyr through a small compiler compatibility shim. It does not claim the compiler/Zephyr pairing is an upstream supported SDK configuration.

1. Install Python 3.13, CMake, Ninja and dtc.
2. Run `bash firmware/scripts/setup-port.sh` from a fresh source checkout. It fetches pinned upstream repositories and applies the patches. It refuses to overwrite dirty upstream checkouts. It performs no flashing. Dependency downloads may be substantial.
3. Install the official SDK 0.16.8 minimal macOS ARM64 package and ARM toolchain under `firmware/toolchains/zephyr-sdk-0.16.8/`. Verify upstream release checksums: https://github.com/zephyrproject-rtos/sdk-ng/releases/tag/v0.16.8. Linux/other toolchains require adapting the paths and remain unverified here.
4. Build USB/BLE/Studio without a bootloader: `bash firmware/port/scripts/build.sh zmk`.
5. Build the USB validation-only application: `bash firmware/bootloader/build.sh`.
6. For MCUboot, generate your own local development key (never reuse someone else's private key):

```sh
mkdir -p firmware/bootloader/keys
chmod 700 firmware/bootloader/keys
firmware/port/.venv/bin/python firmware/port/bootloader/mcuboot/scripts/imgtool.py keygen \
  -k firmware/bootloader/keys/development-p256.pem -t ecdsa-p256
chmod 600 firmware/bootloader/keys/development-p256.pem
bash firmware/bootloader/build_mcuboot.sh
bash firmware/bootloader/build_zmk.sh
bash firmware/bootloader/package.sh 0.1.0+1
```

Use a new version/output directory for each package. Keep the key securely backed up: an installed bootloader trusts its matching public key, so updates signed by another key will be rejected. The current signing setup is for development; no irreversible device provisioning is involved.

The standalone application links at address zero. The MCUboot application links at `0x20000` with a `0x800` header placeholder. These binaries are incompatible with each other's installation layouts. The Zephyr binary already contains its header placeholder; signing must not add a second one. UF2 wrapping does not relocate an image.

Run host checks with `bash firmware/bootloader/tests/run.sh`. Check a generated package with `python3 firmware/bootloader/tests/check_packages.py firmware/releases/mx54be13-mcuboot/0.1.0+1`.

Do not install a new bootloader by copying its binary to the drive. First installation requires a separate, validated SWD procedure with a full backup. Do not mass erase to resolve a build or connection problem.

The MCUboot scratch cleanup patch is required for this RRAM target and is
applied by `setup-port.sh`. When maintaining an existing pinned workspace,
review and apply `firmware/patches/mcuboot-rram-scratch-cleanup.patch` before
rebuilding MCUboot. An application UF2 cannot repair an older installed
bootloader. See `docs/testing.md` for the reset regression and hardware results.

Builds were verified in the existing pinned development workspace. The fresh-source setup script is syntax-checked; an independent clean-machine bootstrap remains to be verified. It deliberately refuses to overwrite an already patched/dirty upstream checkout.
