#!/usr/bin/env python3
"""Explicit bounded USB fault tests for the reliable diagnostic receiver only."""
import argparse
import json
import struct
import telemetry

COMMANDS = {"drop-data": 1, "drop-ack": 2, "pause-radio": 3, "restart": 4, "channel": 5, "bootloader": 6,
            "tamper-wire": 8, "replay-wire": 9, "replay-old-session": 10, "inject-plaintext": 11}


def encode(command, value, report_size=208):
    op = COMMANDS[command]
    valid = ((op in (1, 2, 8, 9, 10, 11) and 0 <= value <= 64) or
             (op == 3 and 1 <= value <= 100) or
             (op in (4, 6) and value == 1) or (op == 5 and 0 <= value <= 2))
    if not valid:
        raise ValueError("Outside bounded receiver test parameters")
    if report_size not in (208, 248):
        raise ValueError("Unknown control report size")
    return b"R8CT" + struct.pack("<III", 1, op, value) + bytes(report_size-16)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("command", choices=COMMANDS)
    p.add_argument("value", type=int, nargs="?")
    p.add_argument("--serial")
    p.add_argument("--shared", action="store_true")
    a = p.parse_args()
    value = 1 if a.command in ("restart", "bootloader") and a.value is None else a.value
    if value is None:
        p.error("A bounded test parameter is required")
    try:
        payload = encode(a.command, value)
    except ValueError as e:
        p.error(str(e))
    import hid
    if a.shared:
        from hid_options import shared_open
        shared_open(hid)
    matches = [d for d in hid.enumerate(0x2fe3, 9) if d.get("usage_page") == 0xff00 and
               (a.serial is None or d.get("serial_number") == a.serial)]
    if len(matches) != 1:
        p.error("Expected exactly one matching diagnostic receiver")
    dev = hid.device()
    dev.open_path(matches[0]["path"])
    try:
        stats = telemetry.read(dev)
        if "link_local_epoch" not in stats:
            p.error("Receiver does not implement the reliable diagnostic protocol")
        if COMMANDS[a.command] >= 8 and not stats.get("security_dtls"):
            p.error("Wire security tests require the secure receiver firmware")
        payload = encode(a.command, value, stats.get("control_report_size", 208))
        sent = dev.send_feature_report(b"\0" + payload)
        if sent not in (len(payload), len(payload)+1):
            raise RuntimeError("Short diagnostic control transfer")
        print(json.dumps(dict(command=a.command, value=value, submitted=True)))
    finally:
        dev.close()


if __name__ == "__main__":
    main()
