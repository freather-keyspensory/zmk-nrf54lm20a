#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
version="${1:-0.1.0+1}"
out="$root/../releases/mx54be13-mcuboot/$version"
mkdir -p "$out"
python_bin="$root/../port/.venv/bin/python"
imgtool="$root/../port/bootloader/mcuboot/scripts/imgtool.py"
"$python_bin" "$imgtool" sign --key "$root/keys/development-p256.pem" --align 4 --header-size 0x800 --slot-size 0xd0000 --version "$version" "$root/build-zmk/zephyr/zmk.bin" "$out/zmk.signed.bin"
"$python_bin" "$imgtool" verify -k "$root/keys/development-p256.pem" "$out/zmk.signed.bin"
"$python_bin" "$root/tools/pack_uf2.py" "$out/zmk.signed.bin" "$out/update.uf2" --confirm-linked-at-0x20000
cp "$root/build-mcuboot/zephyr/zephyr.bin" "$out/bootloader.bin"
cp "$root/build-mcuboot/zephyr/zephyr.elf" "$out/bootloader.elf"
cp "$root/build-zmk/zephyr/zmk.elf" "$out/zmk.elf"
"$python_bin" - "$out" <<'PY'
from pathlib import Path
import sys,struct,hashlib,json
p=Path(sys.argv[1]);boot=(p/'bootloader.bin').read_bytes();app=(p/'zmk.signed.bin').read_bytes()
assert len(boot)<=0x20000 and len(app)<=0xcf000
assert struct.unpack_from('<I',app)[0]==0x96f3b83d
sp,pc=struct.unpack_from('<II',app,0x800)
assert 0x20000000<sp<=0x20080000 and pc&1 and 0x20800<=pc<0x20000+len(app)
(p/'factory.bin').write_bytes(boot.ljust(0x20000,b'\xff')+app)
(p/'SHA256SUMS').write_text(''.join(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+f.name+'\n' for f in sorted(p.iterdir()) if f.is_file() and f.name!='SHA256SUMS'))
print(json.dumps({'factory_bytes':(p/'factory.bin').stat().st_size,'signed_bytes':len(app),'initial_sp':hex(sp),'reset_vector':hex(pc)}))
PY
