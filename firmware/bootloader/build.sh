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
west build -p always -b mx54be13/nrf54lm20a/cpuapp "$root" -d "$root/build" -- "-DEXTRA_CFLAGS=-include $root/../port/compat/include/compiler_compat.h" -DBOARD_ROOT="$root/../port" -DZephyr_DIR="$ZEPHYR_BASE/share/zephyr-package/cmake"
