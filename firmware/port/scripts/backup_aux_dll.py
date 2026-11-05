#!/usr/bin/env python3
"""Read-only AP1 backup when main CPU debug is protected.

Does not halt/reset/unlock/erase or write target memory. The CPU may be running;
completion requires two identical complete RRAM reads and stable UICR contents.
Use only for an identified nRF54LM20A with open auxiliary AHB access.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import time

from swd_dll import DllSWD

SIZE = 0x1FD000
CHUNK = 4096


def digest(data):
    return hashlib.sha256(data).hexdigest()


class AuxSWD(DllSWD):
    def __init__(self, logs):
        super().__init__(logs)
        try:
            self.j.coresight_write(2, 0x010000F0, ap=False)
            if self.j.coresight_read(3, ap=True) != 0x84770001:
                raise RuntimeError("Unexpected auxiliary access port")
            self.j.coresight_write(2, 0x01000000, ap=False)
            csw = self.j.coresight_read(0, ap=True)
            if csw & ((1 << 6) | (1 << 23)) != ((1 << 6) | (1 << 23)):
                raise RuntimeError("Auxiliary memory access is protected")
            self.j.coresight_write(0, 0x23000052, ap=True)
            self.check()
        except BaseException:
            self.close()
            raise

    def write32(self, address, value):
        raise RuntimeError("Read-only auxiliary backup: target memory writes prohibited")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    root = args.directory
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(root, 0o700)
    if (root / "complete.json").exists():
        parser.error("Completed backup already exists; use a new directory")
    identity_path = root / "identity.json"
    index_path = root / "chunks.json"
    chunks = root / "chunks"
    chunks.mkdir(exist_ok=True)
    proof = json.loads(index_path.read_text()) if index_path.exists() else {}
    expected = json.loads(identity_path.read_text()) if identity_path.exists() else None
    swd = None
    attempt = 0

    def connect():
        nonlocal swd, attempt, expected
        while (root / f"transport-{attempt}").exists():
            attempt += 1
        swd = AuxSWD(root / f"transport-{attempt}")
        attempt += 1
        identity = swd.identify()
        identity["device_id_private"] = swd.read(0xFFC304, 8).hex()
        if expected is None:
            expected = identity
            identity_path.write_text(json.dumps(identity, indent=2))
        elif identity != expected:
            raise RuntimeError("Target identity differs from checkpoint; refusing resume")

    def read(address, size):
        nonlocal swd
        for retry in range(3):
            try:
                if swd is None:
                    connect()
                return swd.read(address, size)
            except (RuntimeError, ValueError):
                raise
            except Exception as exc:
                if swd:
                    swd.close()
                    swd = None
                print(f"Transport retry {retry + 1}: {type(exc).__name__}", flush=True)
                if retry == 2:
                    raise
                time.sleep(1)

    try:
        uicr = read(0xFFD000, CHUNK)
        uicr_path = root / "uicr.bin"
        if uicr_path.exists() and uicr_path.read_bytes() != uicr:
            raise RuntimeError("UICR differs from initial checkpoint")
        uicr_path.write_bytes(uicr)
        for address in range(0, SIZE, CHUNK):
            key = f"{address:08x}"
            file = chunks / (key + ".bin")
            if key in proof:
                data = file.read_bytes()
                if len(data) != CHUNK or digest(data) != proof[key]:
                    raise RuntimeError("Corrupt backup checkpoint")
                continue
            data = read(address, CHUNK)
            file.write_bytes(data)
            proof[key] = digest(data)
            temporary = index_path.with_suffix(".tmp")
            temporary.write_text(json.dumps(proof))
            temporary.replace(index_path)
            if address % 65536 == 0:
                print(f"Pass 1: saved {address + CHUNK}/{SIZE} bytes", flush=True)
        image = b"".join((chunks / f"{a:08x}.bin").read_bytes() for a in range(0, SIZE, CHUNK))
        (root / "rram.bin").write_bytes(image)
        print("Pass 1 complete; independently checking all RRAM bytes", flush=True)
        for address in range(0, SIZE, CHUNK):
            if read(address, CHUNK) != image[address:address + CHUNK]:
                raise RuntimeError(f"Running target changed at {address:#x}; backup NOT complete")
            if address % 65536 == 0:
                print(f"Pass 2: verified {address + CHUNK}/{SIZE} bytes", flush=True)
        if read(0xFFD000, CHUNK) != uicr:
            raise RuntimeError("UICR changed; backup NOT complete")
        result = dict(bytes=SIZE, sha256=digest(image), chunks=len(proof),
                      uicr_sha256=digest(uicr),
                      verification="two identical complete RRAM reads; identical before/after UICR",
                      core_halt_attempted=False, cpu_state_observable=False,
                      access_port=1, target_memory_writes=False)
        (root / "complete.json").write_text(json.dumps(result, indent=2))
        print("Read-only two-pass backup verified; no target memory writes", flush=True)
    finally:
        if swd:
            swd.close()


if __name__ == "__main__":
    main()
