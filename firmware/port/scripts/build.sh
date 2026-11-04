#!/usr/bin/env bash
set -euo pipefail
port_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="$port_root/.venv/bin:/opt/homebrew/bin:$PATH"
unset ZEPHYR_SDK_INSTALL_DIR
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export CROSS_COMPILE="$port_root/../toolchains/zephyr-sdk-0.16.8/arm-zephyr-eabi/bin/arm-zephyr-eabi-"
export CMAKE_POLICY_VERSION_MINIMUM=3.5
export ZEPHYR_BASE="$port_root/zephyr"
cd "$port_root"
case "${1:-zmk}" in
 bringup)
  west build -p always -b mx54be13/nrf54lm20a/cpuapp bringup -d build/bringup -- -DZephyr_DIR="$port_root/zephyr/share/zephyr-package/cmake" -DBOARD_ROOT="$port_root" "-DEXTRA_CFLAGS=-include $port_root/compat/include/compiler_compat.h"
  ;;
 zmk)
  west build -p always -b mx54be13/nrf54lm20a/cpuapp zmk/app -d build/zmk -- -DZephyr_DIR="$port_root/zephyr/share/zephyr-package/cmake" -DBOARD_ROOT="$port_root" "-DEXTRA_CFLAGS=-include $port_root/compat/include/compiler_compat.h" -DZMK_EXTRA_MODULES="$port_root/compat" -DZMK_CONFIG="$port_root/config" -DEXTRA_CONF_FILE="$port_root/config/mx54be13.conf" -DKEYMAP_FILE="$port_root/config/mx54be13.keymap"
  ;;
 *) echo 'Usage: build.sh [bringup|zmk]' >&2; exit 2 ;;
esac
