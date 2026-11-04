#!/usr/bin/env python3
"""Program only application RRAM through raw SWD, never UICR/OTP.

Requires a complete pre-existing full-device RRAM backup. Uses unbuffered
RRAMC writes according to the pinned Nordic HAL. Does not erase or unlock.
"""
import argparse, hashlib, json, struct, time
from pathlib import Path
from raw_swd import RawSWD

APP_LIMIT = 0x1F4000
RRAMC = 0x5004E000

def validate_image(data):
    if not 8 <= len(data) <= APP_LIMIT:
        raise ValueError('Invalid application image length')
    sp, reset = struct.unpack_from('<II', data)
    if not 0x20000000 < sp <= 0x20080000 or sp % 8:
        raise ValueError('Invalid initial stack pointer')
    if not reset & 1 or not 0 <= (reset & ~1) < len(data):
        raise ValueError('Reset vector must be Thumb code inside this image')
    return data + b'\xff' * (-len(data) % 16)

def main():
    p=argparse.ArgumentParser()
    p.add_argument('image',type=Path)
    p.add_argument('--backup',type=Path,required=True)
    p.add_argument('--logs',type=Path,required=True)
    p.add_argument('--resume-from',type=Path)
    p.add_argument('--previous-image',type=Path)
    p.add_argument('--previous-proof',type=Path)
    p.add_argument('--speed',type=int,default=100)
    a=p.parse_args()
    image=validate_image(a.image.read_bytes())
    backup=a.backup/'rram.bin'
    proof=json.loads((a.backup/'complete.json').read_text())
    old=backup.read_bytes()
    if len(old)!=0x1fd000 or hashlib.sha256(old).hexdigest()!=proof['sha256']:
        raise SystemExit('Complete verified RRAM backup is required')
    if a.logs.exists(): raise SystemExit('Use a new log directory')
    s=RawSWD(a.logs,speed=a.speed)
    identity=s.identify()
    predecessor=None
    if bool(a.previous_image)!=bool(a.previous_proof):
        raise SystemExit('Previous image and proof must be supplied together')
    if a.previous_image:
        previous=validate_image(a.previous_image.read_bytes())
        verified_previous=json.loads(a.previous_proof.read_text())
        predecessor=hashlib.sha256(previous).hexdigest()
        if (verified_previous['identity']!=identity or verified_previous['sha256']!=predecessor
            or verified_previous['readback_sha256']!=predecessor or verified_previous['bytes']!=len(previous)):
            raise SystemExit('Previous image proof does not match')
        old=previous+old[len(previous):]
    plan={'identity':identity,'image_sha256':hashlib.sha256(image).hexdigest(),'backup_sha256':proof['sha256']}
    if predecessor: plan['previous_image_sha256']=predecessor
    if a.resume_from and json.loads(a.resume_from.read_text())!=plan:
        raise SystemExit('Resume plan does not match image, target and backup')
    (a.logs/'program-plan.json').write_text(json.dumps(plan,indent=2))
    initial=s.read_words([RRAMC+0x400,RRAMC+0x404,RRAMC+0x500,RRAMC+0x10c])
    if initial[:2]!=[1,1] or initial[2]!=0 or initial[3]!=0:
        raise SystemExit(f'Unexpected RRAM controller state: {initial}')
    # Keep the core halted throughout programming and verification.
    s.run(s.write_ops(0xE000EDF0,0xA05F0003))
    for attempt in range(20):
        if s.read_words([0xE000EDF0])[0] & (1<<17):
            break
        time.sleep(0.05)
    else:
        raise SystemExit('Could not halt target')
    # Check that the backed-up application region still matches this target.
    before=s.read(0,len(image))
    if a.resume_from:
        for n in range(0,len(image),4):
            if before[n:n+4] not in (old[n:n+4],image[n:n+4]):
                raise SystemExit(f'Unexpected target contents at {n:#x}; refusing resume')
    elif before!=old[:len(image)]:
        raise SystemExit('Target contents changed since backup; take a new backup')
    try:
        s.run(s.write_ops(RRAMC+0x500,1))  # WEN=1, WRITEBUFSIZE=0
        if s.read_words([RRAMC+0x500])[0]!=1:
            raise RuntimeError('RRAM write-enable did not take effect')
        for offset in range(0,len(image),256):
            chunk=image[offset:offset+256]
            if before[offset:offset+len(chunk)]==chunk:
                continue
            ops=[]
            for index in range(0,len(chunk),4):
                value=struct.unpack_from('<I',chunk,index)[0]
                ops+=s.write_ops(offset+index,value)
            s.run(ops)
            ready,next_ready,error=s.read_words([RRAMC+0x400,RRAMC+0x404,RRAMC+0x10c])
            if (ready,next_ready,error)!=(1,1,0):
                raise RuntimeError(f'RRAM error at {offset:#x}: {(ready,next_ready,error)}')
            if s.read(offset,len(chunk))!=chunk:
                raise RuntimeError(f'Read-back mismatch at {offset:#x}')
            if offset%4096==0: print(f'Verified {offset+len(chunk)}/{len(image)} bytes',flush=True)
    finally:
        s.run(s.write_ops(RRAMC+0x500,0))
    verified=s.read(0,len(image))
    if verified!=image: raise RuntimeError('Final full image comparison failed')
    (a.logs/'programming-result.json').write_text(json.dumps({
        'identity':identity,'bytes':len(image),'sha256':hashlib.sha256(image).hexdigest(),
        'readback_sha256':hashlib.sha256(verified).hexdigest(),
        'uicr_or_otp_programmed':False,'core_left_halted':True},indent=2))
    print('Programming verified. Core remains halted; boot is not yet verified.')

if __name__=='__main__': main()
