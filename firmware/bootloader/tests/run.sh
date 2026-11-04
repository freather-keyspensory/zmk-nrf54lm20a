#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$root/build-host"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$root/include" "$root/tests/test_core.c" "$root/src/uf2_draft.c" "$root/src/virtual_disk.c" -o "$root/build-host/test_core"
"$root/build-host/test_core"
python3 - "$root" <<'PY'
import importlib.util, pathlib, struct, sys
root=pathlib.Path(sys.argv[1])
spec=importlib.util.spec_from_file_location('pack',root/'tools/pack_uf2.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
b=m.pack(bytes(range(256))+b'x')
assert len(b)==1024
assert struct.unpack_from('<8I',b)[3:7]==(m.BASE,256,0,2)
assert b[512+32:512+33]==b'x' and b[512+33:512+288]==b'\xff'*255
for data in (b'',bytes(m.LIMIT-m.BASE+1)):
 try:m.pack(data)
 except ValueError:pass
 else:raise AssertionError('invalid size accepted')
print('UF2 converter block layout, padding, and size guards passed')
PY
