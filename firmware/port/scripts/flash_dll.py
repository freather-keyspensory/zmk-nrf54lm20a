#!/usr/bin/env python3
"""Guarded RRAM programming via one persistent J-Link CoreSight session."""
import argparse,hashlib,json,struct,time
from pathlib import Path
from flash_raw import validate_image
from swd_dll import DllSWD
p=argparse.ArgumentParser()
p.add_argument('image',type=Path);p.add_argument('--backup',type=Path,required=True)
p.add_argument('--logs',type=Path,required=True);p.add_argument('--resume-from',type=Path)
a=p.parse_args()
image=validate_image(a.image.read_bytes())
original=(a.backup/'rram.bin').read_bytes();proof=json.loads((a.backup/'complete.json').read_text())
if len(original)!=0x1fd000 or hashlib.sha256(original).hexdigest()!=proof['sha256']:
 raise SystemExit('Verified full RRAM backup required')
if a.logs.exists():raise SystemExit('Use a new log directory')
with DllSWD(a.logs) as s:
 identity=s.identify()
 plan={'identity':identity,'image_sha256':hashlib.sha256(image).hexdigest(),'backup_sha256':proof['sha256']}
 if a.resume_from and json.loads(a.resume_from.read_text())!=plan:raise SystemExit('Resume plan mismatch')
 (a.logs/'program-plan.json').write_text(json.dumps(plan,indent=2))
 controller=[s.read32(x) for x in [0x5004e400,0x5004e404,0x5004e500,0x5004e10c]]
 # Zephyr leaves its write buffer configured after writes: one word in ZMK,
 # 32 words in MCUboot. Accept only these tested configurations with WEN off.
 if controller not in ([1,1,0,0],[1,1,0x100,0],[1,1,0x2000,0]):
  raise SystemExit('Unexpected RRAM controller state')
 s.write32(0xe000edf0,0xa05f0003)
 for attempt in range(20):
  if s.read32(0xe000edf0)&(1<<17):break
  time.sleep(0.05)
 else:raise SystemExit('Could not halt target')
 before=s.read(0,len(image))
 if a.resume_from:
  for i in range(0,len(image),4):
   if before[i:i+4] not in (original[i:i+4],image[i:i+4]):
    raise SystemExit(f'Unexpected target word at {i:#x}')
 elif before!=original[:len(image)]:raise SystemExit('Target differs from original backup')
 s.record('before_validated',bytes=len(image),resume=bool(a.resume_from))
 try:
  s.write32(0x5004e500,1)
  if s.read32(0x5004e500)!=1:raise RuntimeError('RRAM write enable failed')
  for offset in range(0,len(image),256):
   block=image[offset:offset+256]
   if before[offset:offset+len(block)]!=block:
    for i in range(0,len(block),4):s.write32(offset+i,struct.unpack_from('<I',block,i)[0])
   status=[s.read32(x) for x in [0x5004e400,0x5004e404,0x5004e10c]]
   if status!=[1,1,0]:raise RuntimeError(f'RRAM status failure at {offset:#x}: {status}')
   if s.read(offset,len(block))!=block:raise RuntimeError(f'Readback mismatch at {offset:#x}')
   s.record('block_verified',offset=offset,bytes=len(block))
   if offset%8192==0:print(f'Verified {offset+len(block)}/{len(image)} bytes',flush=True)
 finally:
  s.write32(0x5004e500,0)
  if s.read32(0x5004e500)!=0:raise RuntimeError('Could not disable RRAM writes')
 verified=s.read(0,len(image))
 if verified!=image:raise RuntimeError('Final comparison failed')
 result={'identity':identity,'bytes':len(image),'sha256':hashlib.sha256(image).hexdigest(),
 'readback_sha256':hashlib.sha256(verified).hexdigest(),'uicr_or_otp_programmed':False,
 'core_left_halted':True,'transport':'persistent J-Link CoreSight DLL'}
 (a.logs/'programming-result.json').write_text(json.dumps(result,indent=2))
 s.record('programming_complete',**result)
 print('Complete image verified; RRAM writes disabled; core halted',flush=True)
