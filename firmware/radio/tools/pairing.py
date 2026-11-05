#!/usr/bin/env python3
"""Create private random pair credentials and provision only over trusted wires.

No RF pairing, embedded test key, erase, unlock or protection changes. A uses a
UID/header-guarded SRAM mailbox; B uses its UID-matched USB feature interface.
"""
import argparse
import json
import os
from pathlib import Path
import secrets
import stat
import struct
import subprocess
import sys
import time
import zlib
import hashlib


def record(pair_id, secret):
    if len(pair_id) != 16 or len(secret) != 32 or not any(pair_id) or not any(secret):
        raise ValueError("Pair ID/key must be nonzero 16/32-byte values")
    body = b"S8PK" + struct.pack("<I", 1) + pair_id + secret
    return body + struct.pack("<II", zlib.crc32(body), 0)


def check_private(path):
    root = Path(__file__).resolve().parents[3]
    resolved = path.resolve()
    if resolved.is_relative_to(root):
        if subprocess.run(["git", "check-ignore", "-q", "--", str(resolved.relative_to(root))], cwd=root).returncode:
            raise ValueError("Credential files inside the repository must be git-ignored")


def load_key(path):
    check_private(path)
    if stat.S_IMODE(path.stat().st_mode) & 0o077:
        raise ValueError("Credential file must be private (chmod 600)")
    d = json.loads(path.read_text())
    if d.get("schema") != 1:
        raise ValueError("Unknown private credential schema")
    pair_id, secret = bytes.fromhex(d["pair_id"]), bytes.fromhex(d["secret"])
    return record(pair_id, secret)


def usb_payload(blob, replace=False, token=1):
    if len(blob) != 64 or blob != record(blob[8:24], blob[24:56]):
        raise ValueError("Invalid credential record")
    if not 0 < token <= 0xffffffff:
        raise ValueError("A nonzero uint32 provisioning transaction is required")
    return b"R8CT" + struct.pack("<III", 3, 7, int(replace)) + blob + struct.pack("<I", token) + bytes(164)


def expected_uid(path):
    uid = bytes.fromhex(json.loads(path.read_text())["device_id_private"])
    if len(uid) != 8:
        raise ValueError("Expected a private unique SoC identity proof")
    return uid


def image_hash_location(image):
    if len(image) < 32 or image[:4] != bytes.fromhex("3db8f396"):
        raise ValueError("Expected a signed MCUboot image")
    header_size, protected, image_size = struct.unpack_from("<HHI", image, 8)
    end = header_size+image_size+protected
    if header_size != 0x800 or not image_size or end+4 > len(image):
        raise ValueError("Invalid signed-image bounds")
    magic, length = struct.unpack_from("<HH", image, end)
    if magic != 0x6907 or length < 4 or end+length > len(image):
        raise ValueError("Invalid image authentication TLVs")
    cursor, hashes = end+4, []
    while cursor < end+length:
        if cursor+4 > end+length:
            raise ValueError("Truncated image TLV")
        kind, pad, size = struct.unpack_from("<BBH", image, cursor)
        cursor += 4
        if pad or cursor+size > end+length:
            raise ValueError("Invalid image TLV bounds")
        if kind == 0x10:
            if size != 32:
                raise ValueError("Invalid SHA-256 image TLV")
            hashes.append((cursor, image[cursor:cursor+32]))
        cursor += size
    if len(hashes) != 1 or hashes[0][1] != hashlib.sha256(image[:end]).digest():
        raise ValueError("Image hash missing, ambiguous or inconsistent")
    return hashes[0]


def provision_a(a, blob):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "port/scripts"))
    from swd_dll import DllSWD
    symbols = {}
    for line in subprocess.check_output([str(a.nm), "-S", str(a.elf)], text=True).splitlines():
        f = line.split()
        if len(f) == 4:
            symbols[f[3]] = (int(f[0], 16), int(f[1], 16))
    if symbols.get("radio_pair_mailbox", (0, 0))[1] != 80 or symbols.get("radio_security_stats", (0, 0))[1] != 40:
        raise ValueError("An exact secure-image ELF is required")
    if "radio_crypto_probe_results" in symbols or "radio_dtls_probe_results" in symbols:
        raise ValueError("Refusing to provision a public-vector test image")
    address = symbols["radio_pair_mailbox"][0]
    if address % 4 or not 0x20000000 <= address <= 0x20080000-80:
        raise ValueError("Mailbox outside application SRAM")
    image = a.signed_image.read_bytes()
    hash_offset, expected_hash = image_hash_location(image)
    header = image[:32]
    if len(header) != 32 or header[:4] != bytes.fromhex("3db8f396"):
        raise ValueError("Matching signed MCUboot image required")
    with DllSWD(a.logs) as s:
        s.identify()
        if s.read(0xffc304, 8) != expected_uid(a.identity) or s.read(0x20000, 32) != header:
            raise RuntimeError("Unique target or installed image mismatch")
        base = (0x20000+hash_offset) & ~3
        skip = (0x20000+hash_offset)-base
        actual_hash = s.read(base, (skip+32+3)&~3)[skip:skip+32]
        if actual_hash != expected_hash:
            raise RuntimeError("Installed authenticated image hash differs from the exact ELF package")
        if s.read32(0x5004e500) & 1 or s.read32(0xe000edf0) & (1 << 17):
            raise RuntimeError("Application must run with RRAM writer idle")
        if (s.read32(0xe000ed08) != 0x20800 or "radio_started" not in symbols or
                s.read32(symbols["radio_started"][0]) != 1):
            raise RuntimeError("Secure application must be running before writing its SRAM mailbox")
        if s.read32(address) or s.read32(address+4) == 1:
            raise RuntimeError("A provisioning operation is already pending")
        s.write32(address+4, 0)
        for offset in range(0, 64, 4):
            s.write32(address+8+offset, struct.unpack_from("<I", blob, offset)[0])
        if s.read(address+8, 64) != blob:
            raise RuntimeError("SRAM transfer did not verify")
        s.write32(address, 0x32503853 if a.replace else 0x31503853)
        deadline = time.monotonic()+15
        while time.monotonic() < deadline:
            status = s.read32(address+4)
            if status not in (0, 1):
                break
            time.sleep(.02)
        else:
            raise RuntimeError("Firmware did not finish persistent provisioning")
        if status != 2:
            raise RuntimeError("Firmware rejected provisioning: " + str(struct.unpack("<i", struct.pack("<I", status))[0]))
        if s.read(address+8, 64) != bytes(64) or s.read32(0x5004e500) & 1:
            raise RuntimeError("Provisioning mailbox not cleared or writer still enabled")
        generation = s.read32(address+72)
    return dict(board="A", unique_target_verified=True, committed=True, mailbox_cleared=True,
                rram_write_enable_off=True, generation=generation)


def provision_b(a, blob):
    from reliability_suite import open_receiver
    import telemetry
    serial = expected_uid(a.identity)[::-1].hex().upper()
    import hid
    matches = [d for d in hid.enumerate(0x2fe3, 0) if d.get("product_id") in (9, 10) and d.get("usage_page") == 0xff00 and
               d.get("serial_number", "").upper() == serial]
    if len(matches) != 1:
        raise RuntimeError("Expected exactly one receiver with the backed-up unique identity")
    serial = matches[0]["serial_number"]
    h = open_receiver(serial, True)
    try:
        before = telemetry.read(h)
        if not before.get("security_provision_ack") or before["control_report_size"] != 248:
            raise ValueError("Expected secure receiver firmware with transaction acknowledgements")
        token = 0
        while not token or token == before["security_provision_token"]:
            token = secrets.randbits(32)
        payload = usb_payload(blob, a.replace, token)
        sent = h.send_feature_report(b"\0"+payload)
        if sent not in (248, 249):
            raise RuntimeError("Short wired provisioning transfer")
        deadline = time.monotonic()+15
        expected = zlib.crc32(blob[8:24])
        while time.monotonic() < deadline:
            d = telemetry.read(h)
            status = d["security_provision_status"]
            if d["security_provision_token"] != token:
                time.sleep(.01)
                continue
            if status == 2 and d["security_pair_id_crc"] == expected:
                return dict(board="B", unique_target_verified=True, committed=True,
                            generation=d["security_pair_generation"], state=d["security_state"])
            if status & 0x80000000:
                raise RuntimeError("Firmware rejected provisioning: " + str(struct.unpack("<i", struct.pack("<I", status))[0]))
            time.sleep(.05)
        raise RuntimeError("Receiver did not confirm the persistent pair record")
    finally:
        h.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    commands = p.add_subparsers(dest="command", required=True)
    new = commands.add_parser("new", help="Create a private random pair ID and 256-bit secret")
    new.add_argument("key_file", type=Path)
    for name in ("provision-a", "provision-b"):
        s = commands.add_parser(name)
        s.add_argument("key_file", type=Path)
        s.add_argument("--identity", type=Path, required=True)
        s.add_argument("--replace", action="store_true", help="Explicitly replace a different existing pair")
        if name == "provision-a":
            for flag in ("elf", "signed-image", "nm", "logs"):
                s.add_argument("--"+flag, type=Path, required=True)
    a = p.parse_args()
    if a.command == "new":
        check_private(a.key_file)
        a.key_file.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        d = dict(schema=1, pair_id=secrets.token_bytes(16).hex(), secret=secrets.token_bytes(32).hex())
        fd = os.open(a.key_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as out:
            json.dump(d, out); out.write("\n")
        print("Private random pair credentials created; contents are not logged")
    else:
        blob = load_key(a.key_file)
        print(json.dumps(provision_a(a, blob) if a.command == "provision-a" else provision_b(a, blob)))


if __name__ == "__main__":
    main()
