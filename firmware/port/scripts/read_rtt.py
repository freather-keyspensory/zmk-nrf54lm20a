#!/usr/bin/env python3
"""Read RTT up buffers through raw SWD without a J-Link core attachment."""
import argparse,struct,time
from pathlib import Path
from raw_swd import RawSWD
p=argparse.ArgumentParser()
p.add_argument('--address',type=lambda x:int(x,0),required=True)
p.add_argument('--seconds',type=float,default=10)
p.add_argument('--logs',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
s=RawSWD(a.logs,speed=1000)
def read_bytes(address,size):
 base=address&~3
 return s.read(base,(address-base+size+3)&~3)[address-base:address-base+size]
end=time.monotonic()+a.seconds
with a.output.open('ab') as output:
 while time.monotonic()<end:
  header=s.read(a.address,24)
  if header[:10]!=b'SEGGER RTT':
   raise SystemExit('RTT control block not initialized')
  count=struct.unpack_from('<I',header,16)[0]
  if not 1<=count<=16:raise SystemExit('Invalid RTT channel count')
  for n in range(count):
   descriptor=a.address+24+n*24
   name,buffer,size,write,read,flags=struct.unpack('<6I',s.read(descriptor,24))
   if not buffer or not size:continue
   if not (0x20000000<=buffer<0x20080000 and 0<size<=65536 and max(write,read)<size):
    raise SystemExit('Invalid RTT buffer descriptor')
   if write==read:continue
   if write>read:data=read_bytes(buffer+read,write-read)
   else:data=read_bytes(buffer+read,size-read)+read_bytes(buffer,write)
   output.write(data);output.flush()
   print(data.decode(errors='replace'),end='',flush=True)
   s.run(s.write_ops(descriptor+16,write))
  time.sleep(0.5)
