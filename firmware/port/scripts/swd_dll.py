#!/usr/bin/env python3
"""Persistent raw CoreSight access; no core attach, reset, erase or unlock on open."""
import json,struct,time,os
from pathlib import Path
import pylink

class DllSWD:
 def __init__(self,logs,speed=1000):
  self.logs=Path(logs);self.logs.mkdir(parents=True,exist_ok=False)
  self.events=(self.logs/'events.jsonl').open('a')
  configured=os.environ.get('MINEW_JLINK_SERIAL')
  config_path=Path(__file__).resolve().parents[1]/'.probe.json'
  if configured is None and config_path.exists():configured=json.loads(config_path.read_text()).get('serial')
  if configured is None:raise RuntimeError('Set MINEW_JLINK_SERIAL explicitly; automatic probe selection is disabled')
  serial=int(configured)
  if serial<=0:raise ValueError('A positive J-Link serial number is required')
  self.j=pylink.JLink();self.j.open(serial_no=serial)
  try:
   self.j.set_tif(pylink.enums.JLinkInterfaces.SWD)
   self.j.set_speed(speed);self.j.coresight_configure()
   if self.j.coresight_read(0,ap=False)!=0x6ba02477:raise RuntimeError('Unexpected debug-port ID')
   self.j.coresight_write(0,0x1e,ap=False)
   self.j.coresight_write(1,0x50000000,ap=False)
   self.j.coresight_write(2,0,ap=False)
   self.j.coresight_write(0,0x23000052,ap=True)
   self.record('opened',serial=serial,speed_khz=speed)
  except BaseException:
   self.close();raise
 def record(self,event,**data):
  self.events.write(json.dumps(dict(event=event,time=time.time(),**data))+'\n');self.events.flush()
 def check(self):
  status=self.j.coresight_read(1,ap=False)
  if status&0xa2:raise RuntimeError(f'Debug sticky error: {status:#x}')
 def read32(self,address):
  if address%4:raise ValueError('Unaligned read')
  self.j.coresight_write(1,address,ap=True)
  value=self.j.coresight_read(3,ap=True)
  self.check();return value
 def write32(self,address,value):
  if address%4:raise ValueError('Unaligned write')
  allowed=(0<=address<0x1f4000 or 0x20000000<=address<0x20080000 or
           address in [0x5004e500,0xe000edf0,0xe000edf4,0xe000edf8,0xe000ed0c])
  if not allowed:raise ValueError(f'Write outside permitted RRAM/SRAM/debug registers: {address:#x}')
  self.j.coresight_write(1,address,ap=True)
  self.j.coresight_write(3,value,ap=True);self.check()
 def read(self,address,size):
  if address%4 or size%4:raise ValueError('Read must be aligned')
  data=bytearray()
  while len(data)<size:
   current=address+len(data);length=min(1024-current%1024,size-len(data))
   self.j.coresight_write(1,current,ap=True)
   for _ in range(length//4):data+=struct.pack('<I',self.j.coresight_read(3,ap=True))
   self.check()
  return bytes(data)
 def identify(self):
  part,variant,ram,rram=[self.read32(a) for a in [0xffc31c,0xffc320,0xffc328,0xffc32c]]
  if (part,ram,rram)!=(0x054bc20a,512,2036):raise RuntimeError('Unexpected target identity')
  identity=dict(part=hex(part),variant=hex(variant),ram_kib=ram,rram_kib=rram)
  self.record('identity',**identity);return identity
 def close(self):
  if getattr(self,'j',None):self.j.close();self.j=None
  if getattr(self,'events',None):self.events.close();self.events=None
 def __enter__(self):return self
 def __exit__(self,*args):self.close()
