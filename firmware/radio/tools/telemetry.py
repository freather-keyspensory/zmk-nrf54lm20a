"""Read-only receiver telemetry, feature report zero, wire versions 1/2."""
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
REPORT_FIELDS = ("usb_reports_submitted", "usb_reports_completed", "usb_dual_reports", "usb_report_size")


def decode(data):
    data = bytes(data)
    # HIDAPI backends may include the unnumbered report's leading zero ID.
    if data[:5] == b"\0M8ST":
        data = data[1:]
    if len(data) not in (96, 112) or data[:4] != b"M8ST":
        raise ValueError("Expected 96/112-byte M8ST receiver telemetry")
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    if (words[1], len(data)) not in ((1, 96), (2, 112)) or words[23] != 0:
        raise ValueError("Unsupported receiver telemetry version/reserved field")
    result = dict(zip(FIELDS, words[2:24]))
    if words[1] == 2:
        result.update(zip(REPORT_FIELDS, words[24:]))
        if result["usb_report_size"] not in (32, 64):
            raise ValueError("Unsupported input report size")
    return result


def read(device):
    return decode(device.get_feature_report(0, 113))


def difference(before, after):
    if before["frequency_mhz"] != after["frequency_mhz"]:
        raise ValueError("Receiver channel changed during capture")
    elapsed = (after["uptime_ms"] - before["uptime_ms"]) & 0xffffffff
    if not 0 < elapsed < 120000:
        raise ValueError("Receiver reset or invalid telemetry window")
    keys = list(COUNTERS)
    if before.get("usb_report_size") != after.get("usb_report_size"):
        raise ValueError("USB report format changed during capture")
    if "usb_report_size" in before:
        keys += list(REPORT_FIELDS[:3])
    return dict(counter_window_ms=elapsed,
                counters={k: (after[k] - before[k]) & 0xffffffff for k in keys})
