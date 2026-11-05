#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
port="$root/../port"
export PATH="$port/.venv/bin:/opt/homebrew/bin:$PATH"
export ZEPHYR_BASE="$port/zephyr"
unset ZEPHYR_SDK_INSTALL_DIR
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export CROSS_COMPILE="$root/../toolchains/zephyr-sdk-0.16.8/arm-zephyr-eabi/bin/arm-zephyr-eabi-"
export CMAKE_POLICY_VERSION_MINIMUM=3.5
cd "$port"
west build -p auto -b mx54be13/nrf54lm20a/cpuapp zmk/app -d "$root/build/tx-mcuboot-keyboard" -- \
 -DZephyr_DIR="$port/zephyr/share/zephyr-package/cmake" -DBOARD_ROOT="$port" \
 "-DEXTRA_CFLAGS=-include $port/compat/include/compiler_compat.h" \
 -DZMK_EXTRA_MODULES="$port/compat" -DZMK_CONFIG="$port/config" \
 "-DEXTRA_CONF_FILE=$port/config/mx54be13.conf;$root/../bootloader/integration/zmk.conf;$root/secure.conf;$root/zmk/tx.conf" \
 -DEXTRA_DTC_OVERLAY_FILE="$root/../bootloader/integration/zmk.overlay" -DKEYMAP_FILE="$port/config/mx54be13.keymap"
