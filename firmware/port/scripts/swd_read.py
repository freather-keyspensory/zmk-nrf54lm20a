#!/usr/bin/env python3
"""Read memory using J-Link Commander raw SWD commands, without core attach/unlock."""
import argparse, json, re, subprocess
from pathlib import Path
p = argparse.ArgumentParser()
p.add_argument('addresses', nargs='+', type=lambda s: int(s, 0))
p.add_argument('--log', type=Path, required=True)
a = p.parse_args()
cmds = ['si SWD', 'speed 100', 'SWDSelect', 'SWDWriteDP 1 0x50000000',
        'SWDWriteDP 2 0', 'SWDWriteAP 0 0x23000052']
for address in a.addresses:
    if address % 4:
        p.error('addresses must be word aligned')
    cmds += [f'SWDWriteAP 1 0x{address:08X}', 'SWDReadAP 3', 'SWDReadDP 3']
cmds += ['exit']
r = subprocess.run(['JLinkExe', '-NoGui', '1'], input='\n'.join(cmds)+'\n',
                   text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
a.log.parent.mkdir(parents=True, exist_ok=True)
a.log.write_text(r.stdout)
if r.returncode or 'ERROR' in r.stdout or 'FAILED' in r.stdout:
    raise SystemExit('SWD read failed; inspect '+str(a.log))
values = re.findall(r'Read DP register 3 = 0x([0-9A-Fa-f]+)', r.stdout)
if len(values) != len(a.addresses):
    raise SystemExit('Incomplete read; inspect '+str(a.log))
print(json.dumps({f'0x{addr:08X}': f'0x{int(value,16):08X}'
                  for addr, value in zip(a.addresses, values)}, indent=2))
