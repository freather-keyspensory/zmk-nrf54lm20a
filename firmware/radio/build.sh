#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
role="${1:-}"
layout="${2:-standalone}"
variant="${3:-legacy}"
case "$variant" in legacy|reliable|crypto-probe|dtls-probe|secure) ;; *) echo 'Unknown variant' >&2; exit 2 ;; esac
case "$role" in tx|rx) ;; *) echo 'Usage: build.sh tx|rx [standalone|mcuboot] [legacy|reliable|crypto-probe|dtls-probe|secure]' >&2; exit 2 ;; esac
case "$layout" in standalone|mcuboot) ;; *) echo 'Unknown layout' >&2; exit 2 ;; esac
export PATH="$root/../port/.venv/bin:/opt/homebrew/bin:$PATH"
export ZEPHYR_BASE="$root/../port/zephyr"
unset ZEPHYR_SDK_INSTALL_DIR
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export CROSS_COMPILE="$root/../toolchains/zephyr-sdk-0.16.8/arm-zephyr-eabi/bin/arm-zephyr-eabi-"
export CMAKE_POLICY_VERSION_MINIMUM=3.5
cd "$root/../port"
extra=("-DRADIO_ROLE=$role")
if [[ "$variant" != legacy ]]; then extra+=("-DCONFIG_MINEW_RADIO_RELIABLE=y");
else extra+=("-DCONFIG_MINEW_RADIO_RELIABLE=n"); fi
if [[ "$variant" == crypto-probe || "$variant" == dtls-probe ]]; then
 extra+=("-DCONFIG_MINEW_RADIO_CRYPTO_PROBE=y" "-DCONFIG_MBEDTLS_ENABLE_HEAP=y" "-DCONFIG_MBEDTLS_HEAP_SIZE=98304" "-DCONFIG_MAIN_STACK_SIZE=8192" "-DCONFIG_SPEED_OPTIMIZATIONS=y")
else extra+=("-DCONFIG_MINEW_RADIO_CRYPTO_PROBE=n"); fi
if [[ "$variant" == dtls-probe ]]; then
 extra+=("-DCONFIG_MINEW_RADIO_DTLS_PROBE=y" "-DCONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=512" "-DCONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=224")
else extra+=("-DCONFIG_MINEW_RADIO_DTLS_PROBE=n"); fi
if [[ "$variant" == secure ]]; then
 extra+=("-DCONFIG_MINEW_RADIO_SECURE=y" "-UCONFIG_COMPILER_OPT")
else extra+=("-DCONFIG_MINEW_RADIO_SECURE=n"); fi
if [[ "$variant" != legacy && "$role" == rx ]]; then extra+=("-DCONFIG_USBD_THREAD_STACK_SIZE=2048"); fi
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
if [[ "$variant" == secure ]]; then conf+=";$root/secure.conf"; fi
if [[ "$variant" == secure && "$role" == rx ]]; then conf+=";$root/secure_rx.conf"; fi
extra+=("-DCONF_FILE=$conf" "-DDTC_OVERLAY_FILE=$overlays")
build_name="$role"
if [[ "$layout" == mcuboot ]]; then build_name="$role-mcuboot"; fi
if [[ "$variant" != legacy ]]; then build_name+="-$variant"; fi
west build -p auto -b mx54be13/nrf54lm20a/cpuapp "$root" -d "$root/build/$build_name" -- \
  -DRADIO_ROLE="$role" -DBOARD_ROOT="$root/../port" \
  -DZephyr_DIR="$ZEPHYR_BASE/share/zephyr-package/cmake" \
  "-DEXTRA_CFLAGS=-include $root/../port/compat/include/compiler_compat.h" "${extra[@]}"
