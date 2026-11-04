#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
role="${1:-}"
layout="${2:-standalone}"
case "$role" in tx|rx) ;; *) echo 'Usage: build.sh tx|rx [standalone|mcuboot]' >&2; exit 2 ;; esac
case "$layout" in standalone|mcuboot) ;; *) echo 'Unknown layout' >&2; exit 2 ;; esac
export PATH="$root/../port/.venv/bin:/opt/homebrew/bin:$PATH"
export ZEPHYR_BASE="$root/../port/zephyr"
unset ZEPHYR_SDK_INSTALL_DIR
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export CROSS_COMPILE="$root/../toolchains/zephyr-sdk-0.16.8/arm-zephyr-eabi/bin/arm-zephyr-eabi-"
export CMAKE_POLICY_VERSION_MINIMUM=3.5
cd "$root/../port"
extra=("-DRADIO_ROLE=$role")
conf="$root/prj.conf"
overlays=""
if [[ "$role" == rx ]]; then
  conf+=";$root/rx.conf"
  overlays="$root/rx.overlay"
fi
if [[ "$layout" == mcuboot ]]; then
  conf+=";$root/slot.conf"
  overlays="${overlays:+$overlays;}$root/slot.overlay"
fi
extra+=("-DCONF_FILE=$conf" "-DDTC_OVERLAY_FILE=$overlays")
build_name="$role"
if [[ "$layout" == mcuboot ]]; then build_name="$role-mcuboot"; fi
west build -p auto -b mx54be13/nrf54lm20a/cpuapp "$root" -d "$root/build/$build_name" -- \
  -DRADIO_ROLE="$role" -DBOARD_ROOT="$root/../port" \
  -DZephyr_DIR="$ZEPHYR_BASE/share/zephyr-package/cmake" \
  "-DEXTRA_CFLAGS=-include $root/../port/compat/include/compiler_compat.h" "${extra[@]}"
