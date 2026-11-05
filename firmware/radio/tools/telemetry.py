"""Read-only receiver telemetry, feature report zero, schema versions 1..12."""
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
LINK_FIELDS = (
    "link_local_epoch", "link_remote_epoch", "link_state", "link_ack_sent",
    "link_data_packets", "link_retries", "link_busy", "link_channel", "link_hops",
    "link_injected_data", "link_injected_ack", "link_restarts", "link_backpressure",
    "link_timeouts", "link_discarded", "link_queue_overflow", "link_samples",
    "link_max_queue", "link_max_ack_us", "link_data_sent", "link_ack_received",
    "link_losses", "link_sample_late", "link_reserved",
)
LINK_COUNTERS = tuple(k for k in LINK_FIELDS if k not in (
    "link_local_epoch", "link_remote_epoch", "link_state", "link_channel",
    "link_max_queue", "link_max_ack_us", "link_reserved"))
SECURITY_FIELDS = ("security_state", "security_sessions", "security_failures",
                   "security_wire_sent", "security_wire_received", "security_app_sent",
                   "security_app_received", "security_pair_id_crc", "security_provision_status", "security_pair_generation")


def decode(data):
    data = bytes(data)
    # HIDAPI backends may include the unnumbered report's leading zero ID.
    if data[:5] == b"\0M8ST":
        data = data[1:]
    if len(data) not in (96, 112, 208, 248) or data[:4] != b"M8ST":
        raise ValueError("Expected M8ST receiver telemetry")
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    if (words[1], len(data)) not in ((1, 96), (2, 112), (3, 208), (4, 208), (5, 208), (6, 208), (7, 208), (8, 248), (9, 248), (10, 248), (11, 248), (12, 248)) or words[23] != 0:
        raise ValueError("Unsupported receiver telemetry version/reserved field")
    result = dict(zip(FIELDS, words[2:24]))
    if words[1] >= 2:
        result.update(zip(REPORT_FIELDS, words[24:28]))
        if result["usb_report_size"] not in (32, 64):
            raise ValueError("Unsupported input report size")
    if words[1] in (3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
        result.update(zip(LINK_FIELDS, words[28:52]))
        result["link_wire_version"] = 4 if words[1] >= 9 else 3 if words[1] == 8 else 2 if words[1] >= 4 else 1
        result["link_receive_window_guard"] = words[1] >= 5
        result["link_idle_detection"] = words[1] >= 6
        result["link_session_purge_safe"] = words[1] >= 7
        result["control_report_size"] = len(data)
        if result["link_reserved"] and words[1] < 12:
            raise ValueError("Unsupported link telemetry reserved field")
    if words[1] in (8, 9, 10, 11, 12):
        result.update(zip(SECURITY_FIELDS, words[52:62]))
        result["security_dtls"] = True
        result["security_window_packets"] = 4 if words[1] >= 9 else 1
        result["security_async_receiver"] = words[1] >= 10
        result["security_wire_channel_hold"] = words[1] >= 11
        result["security_provision_ack"] = words[1] >= 12
        if words[1] >= 12:
            result["security_provision_token"] = result["link_reserved"]
        if result["security_state"] > 2:
            raise ValueError("Unknown security state")
    return result


def read(device):
    return decode(device.get_feature_report(0, 249))


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
    if before.get("link_local_epoch") != after.get("link_local_epoch"):
        raise ValueError("Receiver boot session changed during capture")
    if "link_local_epoch" in before:
        keys += list(LINK_COUNTERS)
    if before.get("security_dtls") != after.get("security_dtls"):
        raise ValueError("Security profile changed during capture")
    if before.get("security_dtls"):
        keys += list(SECURITY_FIELDS[1:7])
    return dict(counter_window_ms=elapsed,
                counters={k: (after[k] - before[k]) & 0xffffffff for k in keys})
