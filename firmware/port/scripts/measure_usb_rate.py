#!/usr/bin/env python3
"""Explicit development-only neutral-report benchmark; resolve symbols from flashed ELF."""
import argparse,json,subprocess,time
from pathlib import Path
from swd_dll import DllSWD
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--elf',type=Path,required=True)
p.add_argument('--nm',type=Path,required=True)
p.add_argument('--logs',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
symbols={}
for line in subprocess.check_output([str(a.nm),'-a',str(a.elf)],text=True).splitlines():
 parts=line.split()
 if len(parts)==3:
  try:symbols[parts[2]]=int(parts[0],16)
  except ValueError:pass
names=['minew_test_key_request','minew_usb_bench_state','minew_usb_bench_completed','minew_usb_bench_errors','minew_usb_bench_elapsed_ns']
for name in names:
 if name not in symbols:raise SystemExit('Missing ELF symbol: '+name)
with DllSWD(a.logs) as swd:
 swd.identify()
 swd.write32(symbols[names[0]],7)
 started=False;end=time.monotonic()+12
 while time.monotonic()<end:
  state=swd.read32(symbols[names[1]])
  if state==1:started=True
  if state==3:raise SystemExit('Benchmark failed or USB interface is not ready')
  if started and state==2:break
  time.sleep(.1)
 else:raise SystemExit('Benchmark did not complete')
 completed=swd.read32(symbols[names[2]]);errors=swd.read32(symbols[names[3]])
 ns=int.from_bytes(swd.read(symbols[names[4]],8),'little')
 result={'completed_reports':completed,'errors':errors,'first_to_last_completion_ns':ns,
         'device_completion_rate_hz':(completed-1)*1e9/ns if ns else 0,
         'measurement':'Device USB completion callbacks timed by Zephyr hardware cycle counter; not a host event-latency measurement'}
 swd.record('usb_benchmark',**result)
 a.output.write_text(json.dumps(result,indent=2)+'\n')
 print(json.dumps(result,indent=2))
