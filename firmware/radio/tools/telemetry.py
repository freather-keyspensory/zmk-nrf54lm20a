"""Read-only receiver telemetry, feature report zero, wire version 1."""
import struct

FIELDS = (
    "uptime_ms", "frequency_mhz", "radio_completed", "radio_fresh",
    "radio_missing", "radio_duplicate", "radio_bad_crc", "radio_bad_frame",
    "usb_submitted", "usb_completed", "usb_dropped", "usb_errors",
    "drop_not_ready", "drop_queue_full", "drop_wait_timeout", "drop_no_buffer",
    "drop_submit_failed", "queue_used", "queue_high_water", "usb_buffers_used",
    "usb_ready", "reserved",
)
COUNTERS = FIELDS[2:17]


def decode(data):
    data = bytes(data)
    # HIDAPI backends may include the unnumbered report's leading zero ID.
    if len(data) == 97 and data[0] == 0:
        data = data[1:]
    if len(data) != 96 or data[:4] != b"M8ST":
        raise ValueError("Expected 96-byte M8ST receiver telemetry")
    words = struct.unpack("<24I", data)
    if words[1] != 1 or words[-1] != 0:
        raise ValueError("Unsupported receiver telemetry version/reserved field")
    return dict(zip(FIELDS, words[2:]))


def read(device):
    return decode(device.get_feature_report(0, 97))


def difference(before, after):
    if before["frequency_mhz"] != after["frequency_mhz"]:
        raise ValueError("Receiver channel changed during capture")
    elapsed = (after["uptime_ms"] - before["uptime_ms"]) & 0xffffffff
    if not 0 < elapsed < 120000:
        raise ValueError("Receiver reset or invalid telemetry window")
    return dict(counter_window_ms=elapsed,
                counters={k: (after[k] - before[k]) & 0xffffffff for k in COUNTERS})
