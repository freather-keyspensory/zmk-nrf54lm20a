#!/usr/bin/env python3
"""Trigger synthetic receiver USB reports with SWD and verify host receipt.

Requires the exact running receiver ELF, the configured J-Link, PyUSB/pylink,
and Python hidapi. Measures USB only; does not stimulate the RF receiver.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "port/scripts"))
from swd_dll import DllSWD
from capture import valid, unpack_report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--elf", type=Path, required=True)
    p.add_argument("--nm", type=Path, required=True)
    p.add_argument("--logs", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    import hid
    symbols = {}
    for line in subprocess.check_output([str(args.nm), str(args.elf)], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3:
            try:
                symbols[parts[2]] = int(parts[0], 16)
            except ValueError:
                pass
    names = ["radio_usb_bench_" + suffix for suffix in
             ("request", "state", "completed", "errors", "elapsed_ns")]
    if any(name not in symbols for name in names):
        p.error("ELF is missing receiver benchmark symbols")
    devices = [d for d in hid.enumerate(0x2FE3, 0x0009) if d.get("usage_page") == 0xFF00]
    if len(devices) != 1:
        p.error(f"Expected one diagnostic HID receiver, found {len(devices)}")
    dev = hid.device()
    dev.open_path(devices[0]["path"])
    try:
        dev.set_nonblocking(1)
        deadline = time.monotonic() + 1
        while dev.read(64):
            if time.monotonic() > deadline:
                p.error("Receiver is not idle; stop RF transmission before benchmarking")
        dev.set_nonblocking(0)
        with DllSWD(args.logs) as swd:
            swd.identify()
            swd.write32(symbols[names[0]], 1)
            count = malformed = duplicate = gaps = 0
            first_seq = last_seq = first_ns = last_ns = None
            first_report_frames = 0
            received = []
            last_received_ns = None
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline:
                data = dev.read(64, 100)
                now = time.perf_counter_ns()
                if not data:
                    if last_received_ns is not None and now - last_received_ns > 500_000_000:
                        break
                    continue
                # Keep reception light: validate after the burst so parsing
                # does not compete with draining HIDAPI's small native queue.
                received.append((now, bytes(data)))
                last_received_ns = now
                if len(data) in (32, 64) and any(int.from_bytes(bytes(data[i+4:i+8]), "little") == 15999 for i in range(0, len(data), 32)):
                    break
            frames = []
            for now, report in received:
                try:
                    frames.extend((now, frame) for frame in unpack_report(report))
                except ValueError:
                    malformed += 1
            for now, data in frames:
                if not valid(data):
                    malformed += 1
                    continue
                seq = int.from_bytes(bytes(data[4:8]), "little")
                if first_seq is None:
                    first_seq, first_ns = seq, now
                if now == first_ns:
                    first_report_frames += 1
                if last_seq is not None:
                    delta = (seq - last_seq) & 0xFFFFFFFF
                    if not 0 < delta < 0x80000000:
                        duplicate += 1
                    else:
                        gaps += delta - 1
                count += 1
                last_seq, last_ns = seq, now
            # Completion callback can finish just after host receives the last report.
            for _ in range(100):
                state = swd.read32(symbols[names[1]])
                if state in (2, 3):
                    break
                time.sleep(.02)
            completed = swd.read32(symbols[names[2]])
            errors = swd.read32(symbols[names[3]])
            ns = int.from_bytes(swd.read(symbols[names[4]], 8), "little")
            batch = "radio_usb_bench_first_frames" in symbols
            first_frames = swd.read32(symbols["radio_usb_bench_first_frames"]) if batch else 1
            device_reports = swd.read32(symbols["radio_usb_bench_reports"]) if batch else completed
            elapsed = last_ns - first_ns if first_ns is not None else 0
            passed = state == 2 and completed == count == 16000 and errors == malformed == duplicate == gaps == 0 and first_seq == 0 and last_seq == 15999
            result = dict(passed=passed, device_state=state, device_completed=completed,
                          device_errors=errors, device_elapsed_ns=ns,
                          device_frame_completion_hz=(completed - first_frames) * 1e9 / ns if ns else None,
                          device_report_completion_hz=(device_reports - 1) * 1e9 / ns if ns else None,
                          host_frames=count, host_reports=len(received), host_first_sequence=first_seq,
                          host_last_sequence=last_seq, host_gaps=gaps,
                          host_duplicates_or_backward=duplicate, host_malformed=malformed,
                          host_active_span_hz=(count - first_report_frames) * 1e9 / elapsed if elapsed else None,
                          host_report_active_span_hz=(len(received) - 1) * 1e9 / elapsed if elapsed else None,
                          host_capture="buffer first, validate after burst; HIDAPI queue may still overflow under host stalls",
                          measurement="SWD-triggered synthetic USB frames; no RF or matrix measurement")
            swd.record("receiver_usb_benchmark", **result)
            args.output.write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, indent=2))
            if not passed:
                raise SystemExit(1)
    finally:
        dev.close()


if __name__ == "__main__":
    main()
