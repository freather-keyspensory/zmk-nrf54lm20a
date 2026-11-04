#!/usr/bin/env python3
"""Pack an image linked at the DRAFT primary-slot address. Not an installer."""
import argparse, hashlib, json, struct
from pathlib import Path
BASE, LIMIT, FAMILY = 0x20000, 0xf0000, 0x9b7316e2

def pack(data):
    if not data or len(data) > LIMIT - BASE:
        raise ValueError('Image must fit the draft primary slot')
    count = (len(data) + 255) // 256
    result = bytearray()
    for n in range(count):
        payload = data[n*256:(n+1)*256].ljust(256, b'\xff')
        result += struct.pack('<8I', 0x0a324655, 0x9e5d5157, 0x2000,
                              BASE+n*256, 256, n, count, FAMILY)
        result += payload + bytes(220) + struct.pack('<I', 0x0ab16f30)
    return bytes(result)

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('image', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--confirm-linked-at-0x20000', action='store_true', required=True,
                   help='Existing address-zero ZMK binaries cannot be relocated by wrapping them')
    a = p.parse_args()
    data = a.image.read_bytes()
    result = pack(data)
    # Exclusive creation prevents accidentally replacing existing firmware artifacts.
    with a.output.open('xb') as f:
        f.write(result)
    print(json.dumps({'status':'DRAFT ONLY; no validated installation path',
                      'input_bytes':len(data), 'base':hex(BASE), 'family':hex(FAMILY),
                      'sha256':hashlib.sha256(result).hexdigest()}, indent=2))
