#!/usr/bin/env python3
"""Check signed-image/UF2 byte layout independently of the packer."""
import hashlib,struct,sys
from pathlib import Path
p=Path(sys.argv[1]);signed=(p/'zmk.signed.bin').read_bytes();uf2=(p/'update.uf2').read_bytes()
assert len(uf2)%512==0
count=len(uf2)//512;decoded=bytearray()
for n in range(count):
 b=uf2[n*512:(n+1)*512]
 h=struct.unpack_from('<8I',b)
 assert h==(0x0a324655,0x9e5d5157,0x2000,0x20000+n*256,256,n,count,0x9b7316e2)
 assert struct.unpack_from('<I',b,508)[0]==0x0ab16f30
 decoded+=b[32:288]
assert decoded[:len(signed)]==signed and decoded[len(signed):]==b'\xff'*(len(decoded)-len(signed))
magic,load,header_size,protected_size,image_size,flags=struct.unpack_from('<IIHHII',signed)
assert magic==0x96f3b83d and header_size==0x800 and image_size>0
assert len(signed)<=0xcf000
sp,pc=struct.unpack_from('<II',signed,header_size)
assert 0x20000000<sp<=0x20080000 and pc&1 and 0x20800<=pc<0x20000+len(signed)
factory=(p/'factory.bin').read_bytes();boot=(p/'bootloader.bin').read_bytes()
assert factory[:len(boot)]==boot and factory[len(boot):0x20000]==b'\xff'*(0x20000-len(boot))
assert factory[0x20000:]==signed
print(f'Validated {count} UF2 blocks, slot bounds, aligned vectors, and factory layout; SHA256 {hashlib.sha256(signed).hexdigest()}')
