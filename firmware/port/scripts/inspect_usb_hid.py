#!/usr/bin/env python3
"""Read the Minew USB HID configuration descriptors without claiming its interface."""
import argparse,json,os
from ctypes.util import find_library
from pathlib import Path
import usb.core
import usb.backend.libusb1
p=argparse.ArgumentParser();p.add_argument('--output',type=Path)
p.add_argument('--pid',type=lambda x:int(x,0),default=0x0007,help='Minew experimental product ID (default: 0x0007)')
a=p.parse_args()
if not 0<=a.pid<=0xffff:p.error('--pid must be a 16-bit USB product ID')
library=os.environ.get('LIBUSB_LIBRARY') or find_library('usb-1.0')
if not library and Path('/opt/homebrew/lib/libusb-1.0.dylib').exists():library='/opt/homebrew/lib/libusb-1.0.dylib'
backend=usb.backend.libusb1.get_backend(find_library=lambda _:library)
if backend is None:raise SystemExit('libusb library not found')
dev=usb.core.find(idVendor=0x2fe3,idProduct=a.pid,backend=backend)
if dev is None:raise SystemExit('Minew development HID identity is not enumerated/approved')
endpoints=[]
for cfg in dev:
 for iface in cfg:
  if iface.bInterfaceClass!=3:continue
  for ep in iface:
   interval=ep.bInterval
   # libusb speed 3 is High Speed; HS interrupt bInterval uses 125 us units.
   us=125*(1<<(interval-1)) if dev.speed==3 and 1<=interval<=16 else 1000*interval
   endpoints.append({'configuration':cfg.bConfigurationValue,'interface':iface.bInterfaceNumber,
    'subclass':iface.bInterfaceSubClass,'protocol':iface.bInterfaceProtocol,
    'endpoint':hex(ep.bEndpointAddress),'attributes':ep.bmAttributes,
    'max_packet_size':ep.wMaxPacketSize,'bInterval':interval,
    'interval_us':us,'polling_hz':1000000/us if us else 0})
result={'vid':'0x2fe3','pid':f'0x{a.pid:04x}','libusb_speed':dev.speed,'hid_endpoints':endpoints}
text=json.dumps(result,indent=2);print(text)
if a.output:a.output.write_text(text+'\n')
