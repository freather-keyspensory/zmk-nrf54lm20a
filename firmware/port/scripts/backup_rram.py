#!/usr/bin/env python3
import argparse,hashlib,json,time
from pathlib import Path
from raw_swd import RawSWD
p=argparse.ArgumentParser();p.add_argument('directory',type=Path);a=p.parse_args()
a.directory.mkdir(parents=True,exist_ok=True)
s=RawSWD(a.directory/'read-logs'/f'run-{time.time_ns()}',speed=1000)
identity=s.identify();(a.directory/'identity.json').write_text(json.dumps(identity,indent=2))
image=a.directory/'rram.bin'
offset=image.stat().st_size if image.exists() else 0
if offset % 1024 or offset > 0x1fd000: raise SystemExit('Invalid partial backup')
if offset and s.read(0,min(offset,1024)) != image.read_bytes()[:1024]:
 raise SystemExit('Existing backup prefix differs from target')
with image.open('ab') as f:
 while offset < 0x1fd000:
  block=s.read(offset,min(0x10000,0x1fd000-offset))
  f.write(block);f.flush();offset+=len(block)
  print(f'Backed up {offset} / {0x1fd000} bytes',flush=True)
data=image.read_bytes()
(a.directory/'sha256.txt').write_text(hashlib.sha256(data).hexdigest()+'  rram.bin\n')
(a.directory/'complete.json').write_text(json.dumps({'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()},indent=2))
print('RRAM backup complete',flush=True)
