#!/usr/bin/env python3
"""Checkpointed full RRAM backup. No erase, unlock, reset, or non-debug writes."""
import argparse, hashlib, json, time
from pathlib import Path
from swd_dll import DllSWD
p=argparse.ArgumentParser();p.add_argument('directory',type=Path);a=p.parse_args()
a.directory.mkdir(parents=True,exist_ok=True)
chunks=a.directory/'chunks';chunks.mkdir(exist_ok=True)
index=a.directory/'chunks.json'
proof=json.loads(index.read_text()) if index.exists() else {}
s=None;attempt=0
try:
 for address in range(0,0x1fd000,4096):
  key=f'{address:08x}';file=chunks/(key+'.bin')
  if key in proof:
   data=file.read_bytes()
   if len(data)!=4096 or hashlib.sha256(data).hexdigest()!=proof[key]:raise RuntimeError('Checkpoint corruption')
   continue
  for retry in range(5):
   try:
    if s is None:
     while (a.directory/f'transport-{attempt}').exists():attempt+=1
     s=DllSWD(a.directory/f'transport-{attempt}');attempt+=1;s.identify()
     if not s.read32(0xe000edf0)&(1<<17):
      if proof:raise RuntimeError('Target resumed during backup; start a new consistent snapshot')
      s.write32(0xe000edf0,0xa05f0003)
      if not s.read32(0xe000edf0)&(1<<17):raise RuntimeError('Target did not halt')
    data=s.read(address,4096)
    break
   except RuntimeError:raise
   except Exception as e:
    if s:s.close();s=None
    print(f'Retry {retry+1}: {type(e).__name__}: {e}',flush=True)
    if retry==4:raise
    time.sleep(1)
  file.write_bytes(data);proof[key]=hashlib.sha256(data).hexdigest()
  temp=index.with_suffix('.tmp');temp.write_text(json.dumps(proof));temp.replace(index)
  if address%65536==0:print(f'Backed up {address+4096}/2084864',flush=True)
 data=b''.join((chunks/(f'{x:08x}.bin')).read_bytes() for x in range(0,0x1fd000,4096))
 (a.directory/'rram.bin').write_bytes(data)
 (a.directory/'complete.json').write_text(json.dumps({'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'chunks':len(proof)},indent=2))
 print('Full backup complete; core remains halted',flush=True)
finally:
 if s:s.close()
