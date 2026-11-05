#!/usr/bin/env python3
"""Measure diagnostic HID delivery; install Python hidapi separately to use.

This measures host receipt rate and sequence loss, not scan-to-host latency.
Opening the experimental vendor HID device never writes firmware or keys.
"""
import argparse
import gc
import json
import time
import telemetry
from pathlib import Path


def valid(data):
    if len(data) != 32 or bytes(data[:2]) != b"M8" or data[2] not in (1, 2) or data[3]:
        return False
    seq = int.from_bytes(bytes(data[4:8]), "little")
    end = 28 if data[2] == 2 else 32
    if data[2] == 2 and not int.from_bytes(bytes(data[28:32]), "little"):
        return False
    return all(data[i] == (seq + i * 17) & 255 for i in range(8, end))


def unpack_report(data):
    """Legacy 32-byte frame or 64-byte report with an optional second frame."""
    data = bytes(data)
    if len(data) == 32:
        return (data,)
    if len(data) == 64:
        return (data[:32],) if data[32:] == bytes(32) else (data[:32], data[32:])
    raise ValueError("Expected a 32/64-byte diagnostic input report")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--serial", help="Required if several diagnostic receivers are present")
    parser.add_argument("--stats", action="store_true", help="Read receiver RF/USB counters before and after (requires telemetry firmware)")
    parser.add_argument("--shared", action="store_true", help="Opt into HIDAPI's public macOS shared-open mode for a separate controller")
    parser.add_argument("--native-macos", action="store_true", help="Direct buffered IOHID input collector; requires --shared and separate fault controller")
    parser.add_argument("--ready-file", type=Path, help="Create a new marker after opening and validating the receiver")
    parser.add_argument("--inject-command", choices=("drop-data", "drop-ack", "pause-radio", "channel"), help="Explicit bounded test on the same HID handle while capture continues")
    parser.add_argument("--inject-value", type=int)
    parser.add_argument("--inject-after", type=float, default=1)
    args = parser.parse_args()
    if args.native_macos and (not args.shared or args.inject_command):
        parser.error("--native-macos requires --shared and a separate fault controller")
    if not 0 < args.seconds <= 60:
        parser.error("--seconds must be between 0 and 60 (capture buffers reports in memory)")
    payload = None
    if args.inject_command:
        from control import encode
        if not args.stats or args.inject_value is None or not 0 <= args.inject_after < args.seconds:
            parser.error("Injection requires --stats, --inject-value and a time within the capture")
        try:
            payload = encode(args.inject_command, args.inject_value)
        except ValueError as e:
            parser.error(str(e))
    import hid  # pip install hidapi; not needed to build firmware
    if args.shared:
        from hid_options import shared_open
        shared_open(hid)
    devices = hid.enumerate(0x2FE3, 0x0009)
    devices = [d for d in devices if d.get("usage_page") == 0xFF00 and
               (args.serial is None or d.get("serial_number") == args.serial)]
    if len(devices) != 1:
        parser.error(f"Expected one matching diagnostic receiver, found {len(devices)}")
    dev = hid.device()
    dev.open_path(devices[0]["path"])
    first = last = previous = first_sequence = None
    current_epoch = None
    seen_epochs = set()
    session_changes = 0
    sequence_events = []
    received = []
    count = frame_count = fresh = missing = duplicates = malformed = 0
    gc_was_enabled = gc.isenabled()
    before = after = None
    injection = dict(command=args.inject_command, value=args.inject_value, submitted=False)
    attempted_injection = False

    def inject():
        start = time.perf_counter_ns()
        try:
            sent = dev.send_feature_report(b"\0" + payload)
            if sent not in (208, 209):
                raise RuntimeError("Short control transfer")
            injection["submitted"] = True
        except Exception as e:
            injection["error"] = str(e)
        injection["control_call_ns"] = time.perf_counter_ns() - start

    try:
        if args.stats:
            before = telemetry.read(dev)
        if payload:
            if "link_local_epoch" not in before:
                raise RuntimeError("Fault commands require reliable receiver firmware")
        if args.ready_file and not args.native_macos:
            with args.ready_file.open("x") as ready:
                ready.write("ready\n")
        end = time.perf_counter() + args.seconds
        started = time.perf_counter_ns()
        gc.disable()
        if args.native_macos:
            from native_capture import collect
            received, started, stopped = collect(args.seconds, devices[0]["serial_number"], args.ready_file)
        while not args.native_macos and time.perf_counter() < end:
            if payload and not attempted_injection and (time.perf_counter_ns() - started) / 1e9 >= args.inject_after:
                attempted_injection = True
                inject()
                if not injection["submitted"]:
                    raise RuntimeError("Fault injection failed: " + str(injection))
            data = dev.read(64, 1)
            now = time.perf_counter_ns()
            if not data:
                continue
            received.append((now, bytes(data)))
        if not args.native_macos:
            stopped = time.perf_counter_ns()
        if gc_was_enabled:
            gc.enable()
        if args.stats:
            after = telemetry.read(dev)
    finally:
        dev.close()
        if gc_was_enabled:
            gc.enable()
    if payload:
        if not injection["submitted"]:
            raise RuntimeError("Fault injection failed: " + str(injection))
    # Validate outside the hot receive loop: HIDAPI's macOS queue is small.
    frames = []
    for now, report in received:
        count += 1
        try:
            frames.extend((now, frame) for frame in unpack_report(report))
        except ValueError:
            malformed += 1
    for now, data in frames:
        frame_count += 1
        if not valid(data):
            malformed += 1
            continue
        epoch = int.from_bytes(bytes(data[28:32]), "little") if data[2] == 2 else None
        seq = int.from_bytes(bytes(data[4:8]), "little")
        if not seen_epochs or epoch != current_epoch:
            if epoch in seen_epochs:
                duplicates += 1
                if len(sequence_events) < 32:
                    sequence_events.append(dict(kind="retired_session", sequence=seq, session=epoch,
                                                at_seconds=(now-started)/1e9))
                continue
            if seen_epochs:
                session_changes += 1
            current_epoch = epoch
            seen_epochs.add(epoch)
            previous = None
        delta = (seq - previous) & 0xFFFFFFFF if previous is not None else 1
        if 0 < delta < 0x80000000:
            missing += delta - 1
            if delta > 1 and len(sequence_events) < 32:
                sequence_events.append(dict(kind="gap", previous=previous, sequence=seq,
                                            missing=delta-1, at_seconds=(now-started)/1e9))
            previous = seq
            fresh += 1
            if first is None:
                first = now
                first_sequence = seq
            last = now
        else:
            duplicates += 1
            if len(sequence_events) < 32:
                sequence_events.append(dict(kind="duplicate_or_backward", previous=previous,
                                            sequence=seq, session=epoch, at_seconds=(now-started)/1e9))
    observed = (stopped - started) / 1e9
    elapsed = (last - first) / 1e9 if first is not None and last > first else 0
    result = dict(reports=count, frames=frame_count, fresh=fresh, missing=missing,
                         sequence_events=sequence_events,
                         max_interreport_gap_ms=max(((received[i][0]-received[i-1][0])/1e6
                                                    for i in range(1, len(received))), default=0),
                         session_changes=session_changes, sessions_seen=len(seen_epochs),
                         last_session=current_epoch,
                         duplicates_or_backward=duplicates, malformed=malformed,
                         first_sequence=first_sequence, last_sequence=previous,
                         observation_seconds=observed,
                         usb_reports_per_second=count / observed,
                         fresh_frames_per_second=fresh / observed,
                         # Retain the original single-frame tool's field as an alias.
                         fresh_reports_per_second=fresh / observed,
                         active_span_reports_per_second=(fresh - 1) / elapsed if elapsed else None,
                         host_capture=("direct macOS IOHID callback into preallocated native buffer" if args.native_macos
                                       else "buffer first with cyclic GC paused; validate after capture"),
                         measurement="host delivery; batching may affect timing")
    if args.stats:
        result["receiver_before"] = before
        result["receiver_after"] = after
        result["receiver_delta"] = telemetry.difference(before, after)
        result["receiver_window_note"] = "Feature snapshots bracket capture; device uptime and host timing use separate clocks and windows are not identical"
    if payload:
        result["injection"] = injection
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
