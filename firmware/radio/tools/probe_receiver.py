#!/usr/bin/env python3
"""Pinned nRF54LM20A receiver counters and reversible scheduling experiment.

No reset, halt, flash, UICR or protection writes. Optional RSSI sampling writes
only RADIO.TASKS_RSSISTART. Optional priority test temporarily raises RADIO to
Zephyr normal priority 0 (encoded 0x20), then restores its original priority.
Run a separate host capture while this tool's sampling window is active.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "port/scripts"))
from swd_dll import DllSWD

RADIO = 0x5008A000
RADIO_PRIORITY_WORD = 0xE000E488  # IRQ 138 is byte 2; pinned nRF54LM20A DT.
USB_PRIORITY_WORD = 0xE000E458    # IRQ 90 is byte 2.
MASK = 0x00FF0000


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--elf", type=Path, required=True)
    p.add_argument("--nm", type=Path, required=True)
    p.add_argument("--identity", type=Path, required=True)
    p.add_argument("--logs", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--seconds", type=float, default=15)
    p.add_argument("--sample-rssi", action="store_true")
    p.add_argument("--raise-priority", action="store_true")
    a = p.parse_args()
    if not 1 <= a.seconds <= 60:
        p.error("--seconds must be 1..60")
    symbols = {}
    for line in subprocess.check_output([str(a.nm), str(a.elf)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    names = ["radio_" + n for n in ("completed", "fresh", "missing", "duplicate",
             "bad_crc", "bad_frame", "usb_submitted", "usb_completed", "usb_dropped", "usb_errors")]
    if any(n not in symbols for n in names):
        p.error("Expected exact receiver ELF with USB and RF counters")
    expected = json.loads(a.identity.read_text())["device_id_private"]
    result = dict(priority_test=a.raise_priority, rssi_samples_dbm=[])
    with DllSWD(a.logs) as s:
        s.identify()
        if s.read(0xFFC304, 8).hex() != expected:
            raise RuntimeError("Probe is not on the expected receiver")
        original = s.read32(RADIO_PRIORITY_WORD)
        if ((original >> 16) & 255) != 0x60 or ((s.read32(USB_PRIORITY_WORD) >> 16) & 255) != 0x40:
            raise RuntimeError("Unexpected pinned IRQ configuration; refusing experiment")

        def volatile_write(address, value):
            if address == RADIO + 0x14:
                if value != 1:
                    raise ValueError("RSSI task takes only 1")
            elif address == RADIO_PRIORITY_WORD:
                current = s.read32(address)
                if (current ^ value) & ~MASK or (value & MASK) not in (original & MASK, 0x00200000):
                    raise ValueError("Only the receiver's verified normal RADIO priority may change")
            else:
                raise ValueError("Volatile diagnostic write outside fixed whitelist")
            s.j.coresight_write(1, address, ap=True)
            s.j.coresight_write(3, value, ap=True)
            s.check()

        def counters():
            return {n: s.read32(symbols[n]) for n in names}

        changed = False
        try:
            if a.sample_rssi:
                for _ in range(24):
                    volatile_write(RADIO + 0x14, 1)
                    time.sleep(.001)
                    result["rssi_samples_dbm"].append(-(s.read32(RADIO + 0x718) & 127))
                print("Unsynchronized RSSI samples:", result["rssi_samples_dbm"], flush=True)
            if a.raise_priority:
                changed = True
                volatile_write(RADIO_PRIORITY_WORD, (original & ~MASK) | 0x00200000)
                if s.read32(RADIO_PRIORITY_WORD) & MASK != 0x00200000:
                    raise RuntimeError("Priority readback failed")
            result["before"] = counters()
            start = time.monotonic()
            print("Receiver measurement window active; temporary priority:", a.raise_priority, flush=True)
            time.sleep(a.seconds)
            result["after"] = counters()
            result["seconds"] = time.monotonic() - start
            result["deltas"] = {n: (result["after"][n] - result["before"][n]) & 0xFFFFFFFF for n in names}
        finally:
            if changed:
                if s.read(0xFFC304, 8).hex() != expected:
                    raise RuntimeError("Receiver changed; cannot restore priority")
                current = s.read32(RADIO_PRIORITY_WORD)
                volatile_write(RADIO_PRIORITY_WORD, (current & ~MASK) | (original & MASK))
                result["priority_restored"] = s.read32(RADIO_PRIORITY_WORD) & MASK == original & MASK
                if not result["priority_restored"]:
                    raise RuntimeError("Priority restore failed; reboot receiver before continuing")
            a.output.write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
