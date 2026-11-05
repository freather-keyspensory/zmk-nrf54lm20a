#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
role="${1:-}"; version="${2:-}"
variant="${3:-legacy}"
case "$variant" in legacy) build_name="$role-mcuboot" ;; reliable) build_name="$role-mcuboot-reliable" ;; *) echo 'Unknown variant' >&2; exit 2 ;; esac
case "$role" in tx|rx) ;; *) echo 'Usage: package.sh tx|rx VERSION [legacy|reliable]' >&2; exit 2 ;; esac
config="$root/build/$build_name/zephyr/.config"
if [[ "$variant" == reliable ]]; then
 rg -q '^CONFIG_MINEW_RADIO_RELIABLE=y$' "$config" || { echo 'Not a reliable build' >&2; exit 2; }
elif rg -q '^CONFIG_MINEW_RADIO_RELIABLE=y$' "$config"; then
 echo 'Refusing to label a reliable build as legacy' >&2; exit 2
fi
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+\+[0-9]+$ ]] || { echo 'Use a version such as 0.3.0+1' >&2; exit 2; }
out="$root/../releases/minew-radio/$role/$version"
mkdir -p "$(dirname "$out")"
mkdir "$out" # Refuse to overwrite any earlier package.
python_bin="$root/../port/.venv/bin/python"
imgtool="$root/../port/bootloader/mcuboot/scripts/imgtool.py"
key="$root/../bootloader/keys/development-p256.pem"
"$python_bin" "$imgtool" sign -k "$key" --align 4 --header-size 0x800 \
  --slot-size 0xd0000 --version "$version" \
  "$root/build/$build_name/zephyr/zephyr.bin" "$out/app.signed.bin"
"$python_bin" "$imgtool" verify -k "$key" "$out/app.signed.bin"
"$python_bin" - "$out/app.signed.bin" <<'PY'
from pathlib import Path
import struct, sys
data = Path(sys.argv[1]).read_bytes()
assert len(data) <= 0xcf000
assert struct.unpack_from('<I', data)[0] == 0x96f3b83d
sp, pc = struct.unpack_from('<II', data, 0x800)
assert 0x20000000 < sp <= 0x20080000
assert pc & 1 and 0x20800 <= pc < 0x20000 + len(data)
PY
"$python_bin" "$root/../bootloader/tools/pack_uf2.py" "$out/app.signed.bin" "$out/update.uf2" --confirm-linked-at-0x20000
cp "$root/build/$build_name/zephyr/zephyr.elf" "$out/app.elf"
"$python_bin" - "$out" <<'PY'
from pathlib import Path
import hashlib, sys
p = Path(sys.argv[1])
(p / 'SHA256SUMS').write_text(''.join(hashlib.sha256(f.read_bytes()).hexdigest() + '  ' + f.name + '\n' for f in sorted(p.iterdir()) if f.is_file()))
PY
