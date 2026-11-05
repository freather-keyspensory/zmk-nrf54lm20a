#!/usr/bin/env python3
"""macOS UF2 install guarded by unique USB identity, disk ancestry and image bounds."""
import argparse
import json
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import sys
import time


def children(node):
    yield node
    for child in node.get("IORegistryEntryChildren", []):
        yield from children(child)


def target_disks(roots, serial):
    matches = {}
    for root in roots:
        for node in children(root):
            if (node.get("IOObjectClass") == "IOUSBHostDevice" and
                    node.get("idVendor") == 0x2fe3 and node.get("idProduct") == 8 and
                    str(node.get("USB Serial Number", "")).upper() == serial.upper()):
                matches[node["IORegistryEntryID"]] = node
    if not matches:
        return set()
    if len(matches) != 1:
        raise RuntimeError("Ambiguous USB identity; refusing to choose a drive")
    return {str(n["BSD Name"]) for n in children(next(iter(matches.values()))) if "BSD Name" in n}


def validate(uf2, signed):
    if (not uf2 or len(uf2) % 512 or len(uf2) > (0xcf000//256)*512 or
            not 0x808 <= len(signed) <= 0xcf000 or signed[:4] != bytes.fromhex("3db8f396")):
        raise ValueError("Expected a signed, slot-linked MCUboot image and complete UF2")
    load, header, protected, size, flags = struct.unpack_from("<IHHII", signed, 4)
    sp, pc = struct.unpack_from("<II", signed, 0x800)
    if (load or header != 0x800 or flags or not size or header+size+protected > len(signed) or
            not 0x20000000 < sp <= 0x20080000 or not pc & 1 or not 0x20800 <= pc < 0x20800+size):
        raise ValueError("Invalid application header, stack or vector bounds")
    payload = bytearray()
    count = len(uf2)//512
    for index in range(count):
        block = uf2[index*512:(index+1)*512]
        m1, m2, flags, address, size, number, total, family = struct.unpack_from("<8I", block)
        if ((m1, m2, flags, size, number, total, family) !=
                (0x0a324655, 0x9e5d5157, 0x2000, 256, index, count, 0x9b7316e2) or
                address != 0x20000+index*256 or address+size > 0xef000 or
                struct.unpack_from("<I", block, 508)[0] != 0x0ab16f30):
            raise ValueError("Malformed UF2, wrong family, or address outside application image")
        payload.extend(block[32:32+size])
    if payload[:len(signed)] != signed or any(x != 255 for x in payload[len(signed):]):
        raise ValueError("UF2 does not reconstruct the supplied signed image")
    if len(payload) < len(signed):
        raise ValueError("Incomplete signed image coverage")


def find_mount(serial):
    roots = plistlib.loads(subprocess.check_output(["ioreg", "-a", "-r", "-c", "IOUSBHostDevice", "-l"]))
    disks = target_disks(roots, serial)
    found = set()
    for disk in disks:
        if not disk.startswith("disk") or not disk.replace("disk", "", 1).replace("s", "").isdigit():
            raise RuntimeError("Unexpected BSD disk identity")
        info = plistlib.loads(subprocess.check_output(["diskutil", "info", "-plist", "/dev/"+disk]))
        mount = info.get("MountPoint")
        if info.get("DeviceIdentifier") == disk and info.get("VolumeName") == "MINEWBOOT" and mount:
            found.add(mount)
    if len(found) > 1:
        raise RuntimeError("Ambiguous recovery mounts for intended target")
    return Path(next(iter(found))) if found else None


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("uf2", type=Path)
    p.add_argument("--signed-image", type=Path, required=True)
    p.add_argument("--identity", type=Path, required=True)
    p.add_argument("--wait-seconds", type=float, default=20)
    a = p.parse_args()
    if sys.platform != "darwin" or not 0 <= a.wait_seconds <= 60:
        p.error("Requires macOS and a bounded wait of 0..60 seconds")
    validate(a.uf2.read_bytes(), a.signed_image.read_bytes())
    uid = bytes.fromhex(json.loads(a.identity.read_text())["device_id_private"])
    if len(uid) != 8:
        p.error("A private eight-byte SoC identity proof is required")
    serial = uid[::-1].hex().upper()
    deadline = time.monotonic()+a.wait_seconds
    while True:
        try:
            mount = find_mount(serial)
        except (subprocess.CalledProcessError, plistlib.InvalidFileException):
            # The USB registry/disk can disappear between reads during reset.
            # Retry discovery only; never relax target matching or retry a write.
            mount = None
        if mount:
            break
        if time.monotonic() >= deadline:
            raise RuntimeError("Intended board's USB recovery volume not found; no image copied")
        time.sleep(.2)
    if not (mount/"INFO_UF2.TXT").is_file():
        raise RuntimeError("Recovery info file missing")
    # Check ancestry again immediately before opening the destination.
    if find_mount(serial) != mount:
        raise RuntimeError("Recovery disk identity changed before copying")
    shutil.copyfile(a.uf2, mount/"UPDATE.UF2")
    print("Signed-image UF2 copied to the uniquely matched board; verify the new application after boot")


if __name__ == "__main__":
    main()
