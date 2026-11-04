#!/usr/bin/env python3
"""Explicit, destructive recovery of this verified Minew target's debug access.

ERASEALL erases RRAM, SRAM and UICR. It does not program OTP. Never automatic.
Nordic NAN-047: CTRLAP.ERASEALL=1; wait status=1; RESET=2 then RESET=0.
"""
import argparse,hashlib,json,re,subprocess,time
from pathlib import Path
from raw_swd import RawSWD
p=argparse.ArgumentParser()
p.add_argument('--erase-incomplete-image',action='store_true',required=True)
p.add_argument('--backup',type=Path,required=True)
p.add_argument('--logs',type=Path,required=True)
a=p.parse_args()
original=(a.backup/'rram.bin').read_bytes()
proof=json.loads((a.backup/'complete.json').read_text())
if len(original)!=0x1fd000 or hashlib.sha256(original).hexdigest()!=proof['sha256']:
 raise SystemExit('Verified original full RRAM backup required')
if a.logs.exists(): raise SystemExit('Use a new log directory')
a.logs.mkdir(parents=True)
sequence=0
def run(ops):
 global sequence
 commands=['si SWD','speed 100','SWDSelect','SWDWriteDP 0 0x1E','SWDWriteDP 1 0x50000000']+ops+['exit']
 r=subprocess.run(['JLinkExe','-NoGui','1'],input='\n'.join(commands)+'\n',text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=15)
 sequence+=1;(a.logs/f'control-{sequence:03d}.log').write_text(r.stdout)
 if r.returncode or re.search(r'ERROR|FAILED',r.stdout,re.I) or '0x6BA02477' not in r.stdout:
  raise RuntimeError('Control-AP operation failed')
 return [int(x,16) for x in re.findall(r'Read DP register 3 = 0x([0-9A-Fa-f]+)',r.stdout)]
def read(offset):
 return run([f'SWDWriteDP 2 0x{0x02000000|(offset&0xf0):08X}',f'SWDReadAP {(offset&0xf)//4}','SWDReadDP 3'])[0]
def write(offset,value):
 run([f'SWDWriteDP 2 0x{0x02000000|(offset&0xf0):08X}',f'SWDWriteAP {(offset&0xf)//4} 0x{value:08X}'])
identity={'idr':read(0xfc),'partno':read(0x30),'hwrevision':read(0x34),'eraseprotect':read(0xc),'approtect':read(0x14)}
(a.logs/'before.json').write_text(json.dumps(identity,indent=2))
if (identity['idr'],identity['partno'],identity['hwrevision'])!=(0x32880000,0x29,1):
 raise SystemExit('Control-AP identity differs from this verified Minew board')
if identity['eraseprotect']!=0 or identity['approtect']!=3:
 raise SystemExit('Unexpected protection state; refusing erase')
write(4,1)
for attempt in range(100):
 status=read(8)
 if status!=2:break
 time.sleep(0.1)
if status!=1:raise SystemExit(f'Recovery incomplete; ERASEALLSTATUS={status}')
write(0,2);write(0,0)
recovered=RawSWD(a.logs/'memory-check',speed=100).identify()
(a.logs/'result.json').write_text(json.dumps({'recovered_identity':recovered,'rram_and_uicr_erased':True,'otp_programmed':False},indent=2))
print('Debug access recovered. RRAM and UICR were erased. Program the complete image before power cycling.')
