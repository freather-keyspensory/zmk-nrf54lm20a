#!/usr/bin/env python3
"""Measure diagnostic HID delivery; install Python hidapi separately to use.

This measures host receipt rate and sequence loss, not scan-to-host latency.
Opening the experimental vendor HID device never writes firmware or keys.
"""
import argparse
import gc
import json
import time


def valid(data):
    if len(data) != 32 or bytes(data[:4]) != b"M8\x01\x00":
        return False
    seq = int.from_bytes(bytes(data[4:8]), "little")
    return all(data[i] == (seq + i * 17) & 255 for i in range(8, 32))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--serial", help="Required if several diagnostic receivers are present")
    args = parser.parse_args()
    if not 0 < args.seconds <= 60:
        parser.error("--seconds must be between 0 and 60 (capture buffers reports in memory)")
    import hid  # pip install hidapi; not needed to build firmware
    devices = hid.enumerate(0x2FE3, 0x0009)
    devices = [d for d in devices if d.get("usage_page") == 0xFF00 and
               (args.serial is None or d.get("serial_number") == args.serial)]
    if len(devices) != 1:
        parser.error(f"Expected one matching diagnostic receiver, found {len(devices)}")
    dev = hid.device()
    dev.open_path(devices[0]["path"])
    end = time.perf_counter() + args.seconds
    started = time.perf_counter_ns()
    first = last = previous = first_sequence = None
    received = []
    count = fresh = missing = duplicates = malformed = 0
    gc_was_enabled = gc.isenabled()
    gc.disable()
    try:
        while time.perf_counter() < end:
            data = dev.read(32, 100)
            now = time.perf_counter_ns()
            if not data:
                continue
            received.append((now, bytes(data)))
    finally:
        stopped = time.perf_counter_ns()
        dev.close()
        if gc_was_enabled:
            gc.enable()
    # Validate outside the hot receive loop: HIDAPI's macOS queue is small.
    for now, data in received:
        count += 1
        if not valid(data):
            malformed += 1
            continue
        seq = int.from_bytes(bytes(data[4:8]), "little")
        delta = (seq - previous) & 0xFFFFFFFF if previous is not None else 1
        if 0 < delta < 0x80000000:
            missing += delta - 1
            previous = seq
            fresh += 1
            if first is None:
                first = now
                first_sequence = seq
            last = now
        else:
            duplicates += 1
    observed = (stopped - started) / 1e9
    elapsed = (last - first) / 1e9 if first is not None and last > first else 0
    print(json.dumps(dict(reports=count, fresh=fresh, missing=missing,
                         duplicates_or_backward=duplicates, malformed=malformed,
                         first_sequence=first_sequence, last_sequence=previous,
                         observation_seconds=observed,
                         fresh_reports_per_second=fresh / observed,
                         active_span_reports_per_second=(fresh - 1) / elapsed if elapsed else None,
                         host_capture="buffer first with cyclic GC paused; validate after capture",
                         measurement="host delivery; batching may affect timing"), indent=2))


if __name__ == "__main__":
    main()
