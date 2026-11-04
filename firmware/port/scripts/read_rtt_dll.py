#!/usr/bin/env python3
"""Capture RTT using persistent raw CoreSight access, without core attachment."""
import argparse,struct,time
from pathlib import Path
from swd_dll import DllSWD
p=argparse.ArgumentParser();p.add_argument('--address',type=lambda x:int(x,0),required=True)
p.add_argument('--seconds',type=float,default=12);p.add_argument('--logs',type=Path,required=True)
p.add_argument('--output',type=Path,required=True);a=p.parse_args()
a.output.parent.mkdir(parents=True,exist_ok=True)
with DllSWD(a.logs) as s,a.output.open('wb') as output:
 s.identify();end=time.monotonic()+a.seconds;initialized=False;received=0
 def read_bytes(address,size):
  base=address&~3
  return s.read(base,(address-base+size+3)&~3)[address-base:address-base+size]
 while time.monotonic()<end:
  header=s.read(a.address,24)
  if header[:10]!=b'SEGGER RTT':time.sleep(0.1);continue
  initialized=True;count=struct.unpack_from('<I',header,16)[0]
  if not 1<=count<=16:raise RuntimeError('Invalid RTT channel count')
  for n in range(count):
   descriptor=a.address+24+n*24
   name,buffer,size,write,read,flags=struct.unpack('<6I',s.read(descriptor,24))
   if not buffer or not size:continue
   if not (0x20000000<=buffer<0x20080000 and buffer+size<=0x20080000 and 0<size<=65536 and max(write,read)<size):
    raise RuntimeError('Invalid RTT buffer descriptor')
   if write==read:continue
   if write>read:data=read_bytes(buffer+read,write-read)
   else:data=read_bytes(buffer+read,size-read)+read_bytes(buffer,write)
   output.write(data);output.flush();received+=len(data)
   print(data.decode(errors='replace'),end='',flush=True)
   s.write32(descriptor+16,write)
  time.sleep(0.5)
 s.record('rtt_capture',initialized=initialized,bytes_received=received)
 if not initialized or not received:raise SystemExit('No initialized RTT output captured')
