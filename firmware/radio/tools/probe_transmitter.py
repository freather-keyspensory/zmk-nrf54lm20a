#!/usr/bin/env python3
"""Read pinned transmitter counters; optional explicitly requested software reset.

Requires an exact ELF/signed-image pair and a private unique-target proof.
Never halts, flashes, erases, or changes protection settings.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "port/scripts"))
from swd_dll import DllSWD
from telemetry import LINK_FIELDS
from pairing import image_hash_location


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--elf", type=Path, required=True)
    p.add_argument("--signed-image", type=Path, required=True)
    p.add_argument("--nm", type=Path, required=True)
    p.add_argument("--identity", type=Path, required=True)
    p.add_argument("--logs", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--seconds", type=float, default=0)
    p.add_argument("--restart", action="store_true", help="Explicit Cortex-M system reset, followed by a new read-only session")
    a = p.parse_args()
    if not 0 <= a.seconds <= 30:
        p.error("--seconds must be 0..30")
    if a.output.exists():
        p.error("Refusing to overwrite prior measurement")
    symbols = {}
    for line in subprocess.check_output([str(a.nm), "-S", str(a.elf)], text=True).splitlines():
        f = line.split()
        if len(f) == 4:
            symbols[f[3]] = (int(f[0], 16), int(f[1], 16))
    if symbols.get("radio_link_stats", (0, 0))[1] != 4 * len(LINK_FIELDS):
        p.error("ELF does not have the expected reliable transmitter ABI")
    expected = json.loads(a.identity.read_text())["device_id_private"]
    image = a.signed_image.read_bytes()
    hash_offset, expected_hash = image_hash_location(image)
    header = image[:32]
    if len(header) != 32 or header[:4] != bytes.fromhex("3db8f396"):
        p.error("A signed MCUboot image is required")
    a.logs.mkdir(parents=True, exist_ok=False)

    def snapshot(s):
        s.identify()
        if s.read(0xffc304, 8).hex() != expected or s.read(0x20000, 32) != header:
            raise RuntimeError("Unique target or installed image header mismatch")
        base = (0x20000 + hash_offset) & ~3
        skip = 0x20000 + hash_offset - base
        if s.read(base, (skip + 32 + 3) & ~3)[skip:skip+32] != expected_hash:
            raise RuntimeError("Installed image hash differs from the exact ELF package")
        result = {k: s.read32(symbols[k][0]) for k in
                  ("radio_started", "radio_heartbeat", "radio_fresh", "radio_last_sequence")}
        result["link"] = dict(zip(LINK_FIELDS, struct.unpack("<24I", s.read(symbols["radio_link_stats"][0], 96))))
        result["rram_write_enable_off"] = not bool(s.read32(0x5004e500) & 1)
        return result

    with DllSWD(a.logs / "before") as s:
        before = snapshot(s)
        if a.restart:
            if not before["rram_write_enable_off"] or before["radio_started"] != 1:
                raise RuntimeError("Refusing reset while writer is active or application not started")
            s.write32(0xe000ed0c, 0x05fa0004)
    result = dict(before=before, restart_requested=a.restart,
                  note="Live SWD counters are not an atomic radio/USB snapshot")
    if a.restart or a.seconds:
        wait = a.seconds or 8
        time.sleep(wait)
        with DllSWD(a.logs / "after") as s:
            result["after"] = snapshot(s)
        result["wait_seconds"] = wait
        if a.restart:
            result["sender_epoch_changed"] = before["link"]["link_local_epoch"] != result["after"]["link"]["link_local_epoch"]
            if not result["sender_epoch_changed"] or not result["after"]["link"]["link_state"]:
                raise RuntimeError("Transmitter did not establish a new session after reset")
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
