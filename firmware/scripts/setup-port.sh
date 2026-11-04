#!/usr/bin/env bash
# Fetch pinned upstream source and apply the published patches; never flash.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
port_root="$repo_root/firmware/port"
python_bin="${PYTHON:-python3.13}"
"$python_bin" -m venv "$port_root/.venv"
"$port_root/.venv/bin/python" -m pip install -r "$repo_root/firmware/requirements-macos.lock.txt"
export PATH="$port_root/.venv/bin:$PATH"
fetch_checkout() {
 local url="$1" path="$2" revision="$3"
 if [[ ! -d "$path/.git" ]]; then git clone --filter=blob:none --no-checkout "$url" "$path"; fi
 if [[ -n "$(git -C "$path" status --porcelain)" ]]; then
  echo "Refusing to replace dirty upstream checkout: $path" >&2; exit 1
 fi
 git -C "$path" checkout --detach "$revision"
}
fetch_checkout https://github.com/zmkfirmware/zmk "$port_root/zmk" edf5c0814fd3ea202e43aad2d68fd32e882a518c
fetch_checkout https://github.com/zephyrproject-rtos/zephyr "$repo_root/firmware/upstream-zephyr" 0534e997eacb94bae1df6c9a7dd4a9a6cbf1cecf
if [[ ! -e "$port_root/zephyr" ]]; then ln -s ../upstream-zephyr "$port_root/zephyr"; fi
cd "$port_root"
if [[ ! -d .west ]]; then west init -l manifest; fi
west update --narrow
# Apply only after dependency resolution; west update would replace tracked upstream changes.
git -C zmk apply --check "$repo_root/firmware/patches/zmk-v0.3.0-zephyr45.patch"
git -C zmk apply "$repo_root/firmware/patches/zmk-v0.3.0-zephyr45.patch"
git -C zephyr apply --check "$repo_root/firmware/patches/zephyr-nordic-binding-validation.patch"
git -C zephyr apply "$repo_root/firmware/patches/zephyr-nordic-binding-validation.patch"
echo "Pinned source prepared. Install the documented toolchain before building. No device was flashed."
