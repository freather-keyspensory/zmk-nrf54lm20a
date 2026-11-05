#!/usr/bin/env python3
"""Plan an explicitly authorized nRF54LM20A recovery using an AP1 backup.

Default: read-only plan. --execute-erase-rram-and-uicr erases the target's
firmware and UICR through Nordic's documented CTRL-AP recovery sequence.
Never invoke the execution flag without explicit authorization for that board.
OTP is not written. A verified full RRAM and UICR backup is mandatory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time

from backup_aux_dll import AuxSWD
from swd_dll import DllSWD


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--backup", type=Path, required=True)
    p.add_argument("--logs", type=Path, required=True)
    p.add_argument("--execute-erase-rram-and-uicr", action="store_true")
    args = p.parse_args()
    backup = args.backup
    proof = json.loads((backup / "complete.json").read_text())
    expected = json.loads((backup / "identity.json").read_text())
    image = (backup / "rram.bin").read_bytes()
    uicr = (backup / "uicr.bin").read_bytes()
    if (len(image) != 0x1FD000 or hashlib.sha256(image).hexdigest() != proof["sha256"] or
        len(uicr) != 4096 or hashlib.sha256(uicr).hexdigest() != proof["uicr_sha256"] or
        proof.get("access_port") != 1 or proof.get("target_memory_writes") is not False):
        p.error("Verified full AP1 RRAM and UICR backup required")
    args.logs.mkdir(parents=True, exist_ok=False, mode=0o700)
    with AuxSWD(args.logs / "control") as s:
        identity = s.identify()
        uid = s.read(0xFFC304, 8).hex()
        if dict(identity, device_id_private=uid) != expected:
            raise RuntimeError("Target differs from backed-up device")

        def read(offset):
            s.j.coresight_write(2, 0x02000000 | (offset & 0xF0), ap=False)
            value = s.j.coresight_read((offset & 15) // 4, ap=True)
            s.check()
            return value

        def write(offset, value):
            s.j.coresight_write(2, 0x02000000 | (offset & 0xF0), ap=False)
            s.j.coresight_write((offset & 15) // 4, value, ap=True)
            s.check()

        before = {"idr": read(0xFC), "partno": read(0x30), "hwrevision": read(0x34),
                  "approtect": read(0x14), "eraseprotect": read(0x0C)}
        if before != dict(idr=0x32880000, partno=0x29, hwrevision=1,
                          approtect=1, eraseprotect=0):
            raise RuntimeError("Unexpected protection/identity state; refusing recovery")
        plan = dict(identity=identity, device_id_private=uid, before=before,
                    backup_sha256=proof["sha256"], uicr_sha256=proof["uicr_sha256"],
                    effect="erase RRAM, SRAM and UICR; no OTP writes",
                    execution_requested=args.execute_erase_rram_and_uicr)
        (args.logs / "plan.json").write_text(json.dumps(plan, indent=2))
        print("Verified target matches full RRAM/UICR backup; main debug protected, erase permitted", flush=True)
        if not args.execute_erase_rram_and_uicr:
            print("Read-only plan complete; no erase, unlock, reset or target memory writes", flush=True)
            return
        # Nordic NAN-047: ERASEALL, wait ReadyToReset, hard reset, release.
        write(4, 1)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            status = read(8)
            if status != 2:
                break
            time.sleep(.1)
        else:
            raise RuntimeError("Recovery timed out; no automatic retry")
        if status != 1:
            raise RuntimeError(f"Unexpected erase status {status}; no automatic retry")
        write(0, 2)
        write(0, 0)

    with DllSWD(args.logs / "main-port") as s:
        if s.identify() != identity or s.read(0xFFC304, 8).hex() != uid:
            raise RuntimeError("Recovered target identity mismatch")
        cpuid = s.read32(0xE000ED00)
        if cpuid & 0xFF00FFF0 != 0x4100D210:
            raise RuntimeError("Expected Cortex-M33 before halt")
        s.write32(0xE000EDF0, 0xA05F0003)
        if not s.read32(0xE000EDF0) & (1 << 17):
            raise RuntimeError("Recovered CPU did not halt")
        result = dict(identity=identity, device_id_private=uid,
                      backup_sha256=proof["sha256"], rram_and_uicr_erased=True,
                      otp_programmed=False, core_left_halted=True)
        (args.logs / "result.json").write_text(json.dumps(result, indent=2))
        print("Recovery complete; RRAM/UICR erased, CPU halted. Take a new baseline backup before flashing.", flush=True)


if __name__ == "__main__":
    main()
