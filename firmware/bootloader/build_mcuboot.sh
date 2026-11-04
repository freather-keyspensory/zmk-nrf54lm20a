#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="$root/../port/.venv/bin:/opt/homebrew/bin:$PATH"
export ZEPHYR_BASE="$root/../port/zephyr"
unset ZEPHYR_SDK_INSTALL_DIR
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export CROSS_COMPILE="$root/../toolchains/zephyr-sdk-0.16.8/arm-zephyr-eabi/bin/arm-zephyr-eabi-"
export CMAKE_POLICY_VERSION_MINIMUM=3.5
cd "$root/../port"
west build -p always -b mx54be13/nrf54lm20a/cpuapp bootloader/mcuboot/boot/zephyr -d "$root/build-mcuboot" -- -DZEPHYR_EXTRA_MODULES="$root/mcuboot-module" -DBOARD_ROOT="$root/../port" -DZephyr_DIR="$ZEPHYR_BASE/share/zephyr-package/cmake" "-DEXTRA_CFLAGS=-include $root/../port/compat/include/compiler_compat.h" -DDTC_OVERLAY_FILE="$root/integration/mcuboot.overlay" -DCONFIG_BOOT_SIGNATURE_KEY_FILE=\"$root/keys/development-p256.pem\" -DEXTRA_CONF_FILE="$root/integration/mcuboot.conf"
