#!/usr/bin/env python3
"""Run explicit, bounded receiver fault tests with a continuously reading host.

Does not flash devices. --faults sends USB diagnostic commands; --restart-rx
restarts the receiver and checks a new boot epoch before a fresh capture.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
import telemetry


def open_receiver(serial=None, shared=False):
    import hid
    if shared:
        from hid_options import shared_open
        shared_open(hid)
    matches = [d for d in hid.enumerate(0x2fe3, 9) if d.get("usage_page") == 0xff00 and
               (serial is None or d.get("serial_number") == serial)]
    if len(matches) != 1:
        raise RuntimeError("Expected one reliable receiver")
    h = hid.device()
    h.open_path(matches[0]["path"])
    return h


def status(serial=None, shared=False):
    h = open_receiver(serial, shared)
    try:
        result = telemetry.read(h)
        if "link_local_epoch" not in result:
            raise RuntimeError("Not the reliable receiver firmware")
        return result
    finally:
        h.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output-dir", type=Path, required=True)
    p.add_argument("--seconds", type=float, default=10)
    p.add_argument("--faults", action="store_true")
    p.add_argument("--security-faults", action="store_true", help="Bounded ciphertext, replay and plaintext rejection tests")
    p.add_argument("--old-session", action="store_true", help="Also inject a saved record after the peer's earlier restart")
    p.add_argument("--restart-rx", action="store_true")
    p.add_argument("--serial")
    p.add_argument("--shared", action="store_true", help="Separate shared-open controller while reader keeps draining")
    p.add_argument("--native-macos", action="store_true", help="Use direct buffered IOHID capture instead of HIDAPI's input queue")
    a = p.parse_args()
    if a.native_macos and not a.shared:
        p.error("Native capture requires --shared")
    if not 5 <= a.seconds <= 60:
        p.error("Capture duration must be 5..60 seconds")
    a.output_dir.mkdir(parents=True, exist_ok=False)
    tool_dir = Path(__file__).resolve().parent
    serial_args = ["--serial", a.serial] if a.serial else []
    if a.shared:
        serial_args += ["--shared"]
    results = []
    controller = None
    controller_report_size = 208

    def command(name, value=None):
        if controller is not None:
            from control import encode
            value = 1 if name in ("restart", "bootloader") and value is None else value
            payload = encode(name, value, controller_report_size)
            sent = controller.send_feature_report(b"\0" + payload)
            if sent not in (len(payload), len(payload)+1):
                raise RuntimeError("Short persistent-controller transfer")
            return
        args = [sys.executable, str(tool_dir / "control.py"), name]
        if value is not None:
            args.append(str(value))
        return subprocess.run(args + serial_args, check=True, capture_output=True, text=True)

    def capture(name, fault=None, value=None):
        output = a.output_dir / (name + ".json")
        ready_file = a.output_dir / (name + ".ready")
        fault_args = ["--inject-command", fault, "--inject-value", str(value), "--inject-after", "1"] if fault and not a.shared else []
        with output.open("w") as stream:
            child = subprocess.Popen([sys.executable, str(tool_dir / "capture.py"), "--stats",
                                      "--seconds", str(a.seconds), "--ready-file", str(ready_file)] + serial_args + fault_args +
                                     (["--native-macos"] if a.native_macos else []), stdout=stream)
            try:
                if fault and a.shared:
                    ready_deadline = time.monotonic() + 10
                    while not ready_file.exists():
                        if child.poll() is not None or time.monotonic() > ready_deadline:
                            raise RuntimeError("Reader did not become ready")
                        time.sleep(.01)
                    time.sleep(1)
                    command(fault, value)
                if child.wait(timeout=a.seconds + 15):
                    raise RuntimeError("Capture failed")
            finally:
                if child.poll() is None:
                    child.terminate()
                    child.wait(timeout=5)
        d = json.loads(output.read_text())
        c = d["receiver_delta"]["counters"]
        before, after = d["receiver_before"], d["receiver_after"]
        checks = {
            "fresh_data": d["fresh"] > 0,
            "near_8k_fresh_rate": 7920 <= d["fresh_frames_per_second"] <= 8080,
            "no_host_gaps": d["missing"] == 0,
            "no_host_duplicates": d["duplicates_or_backward"] == 0,
            "no_malformed": d["malformed"] == 0,
            "same_sender_session": d["session_changes"] == 0 and before["link_remote_epoch"] == after["link_remote_epoch"],
            "same_receiver_session": before["link_local_epoch"] == after["link_local_epoch"],
            "no_usb_drops": c["usb_dropped"] == 0,
            "no_usb_errors": c["usb_errors"] == 0,
            "no_source_queue_overflow": c["link_queue_overflow"] == 0,
            "no_discarded": c["link_discarded"] == 0,
            "no_unrecovered_rf_gaps": c["radio_missing"] == 0,
        }
        if before.get("security_dtls"):
            checks["authenticated"] = before["security_state"] == after["security_state"] == 2
            checks["same_secure_session"] = before["security_sessions"] == after["security_sessions"]
            checks["no_security_failures"] = c["security_failures"] == 0
        if fault in ("drop-data", "drop-ack", "tamper-wire", "replay-wire", "replay-old-session", "inject-plaintext"):
            key = "link_injected_data" if fault == "drop-data" else "link_injected_ack"
            if fault not in ("drop-data", "drop-ack"):
                key = "link_injected_data"
            checks["injection_consumed"] = c[key] == value
        if fault:
            checks["injection_submitted"] = a.shared or d.get("injection", {}).get("submitted") is True
        if fault == "channel":
            channels = [before["frequency_mhz"], 2444, 2474]
            checks["new_channel_acquired"] = after["link_channel"] == channels[value]
        result = dict(name=name, passed=all(checks.values()), checks=checks,
                      fresh=d["fresh"], host_missing=d["missing"], host_duplicates=d["duplicates_or_backward"],
                      fresh_hz=d["fresh_frames_per_second"], receiver_deltas=c,
                      channel=after["link_channel"])
        results.append(result)
        (a.output_dir / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
        print(json.dumps(result), flush=True)

    try:
        if a.shared:
            controller = open_receiver(a.serial, True)
            initial = telemetry.read(controller)
            controller_report_size = initial.get("control_report_size", 208)
        else:
            initial = status(a.serial)
        if not initial["link_state"] or not initial["link_remote_epoch"]:
            p.error("The two-board link is not established")
        if (a.security_faults or a.old_session) and not initial.get("security_dtls"):
            p.error("Security injections require the secure receiver")
        capture("baseline")
        if not results[-1]["passed"]:
            raise SystemExit("Baseline failed; refusing to interpret fault tests as reliability evidence")
        if a.faults:
            for kind in ("drop-data", "drop-ack"):
                for count in (1, 8, 32, 64):
                    capture(f"{kind}-{count}", kind, count)
            for ms in (20, 100):
                capture(f"pause-{ms}ms", "pause-radio", ms)
            s = telemetry.read(controller) if controller is not None else status(a.serial)
            channels = [s["frequency_mhz"], 2444, 2474]
            target = (channels.index(s["link_channel"]) + 1) % len(channels)
            capture("channel-reacquisition", "channel", target)
        if a.security_faults:
            for kind in ("tamper-wire", "replay-wire", "inject-plaintext"):
                for count in (1, 8, 64):
                    capture(f"{kind}-{count}", kind, count)
        if a.old_session:
            capture("replay-old-session", "replay-old-session", 8)
        if a.restart_rx:
            before = telemetry.read(controller) if controller is not None else status(a.serial)
            start = time.monotonic()
            command("restart")
            if controller is not None:
                controller.close(); controller = None
            deadline = start + 15
            while time.monotonic() < deadline:
                time.sleep(.1)
                try:
                    after = status(a.serial, a.shared)
                    if before.get("link_receive_window_guard") and not after.get("link_receive_window_guard"):
                        continue
                    if before.get("link_idle_detection") and not after.get("link_idle_detection"):
                        continue
                    if before.get("link_session_purge_safe") and not after.get("link_session_purge_safe"):
                        continue
                    if before.get("security_dtls") and not after.get("security_dtls"):
                        continue
                    if before.get("security_window_packets") != after.get("security_window_packets"):
                        continue
                    if before.get("security_async_receiver") and not after.get("security_async_receiver"):
                        continue
                    if before.get("security_wire_channel_hold") and not after.get("security_wire_channel_hold"):
                        continue
                    if before.get("security_provision_ack") and not after.get("security_provision_ack"):
                        continue
                    if (after["link_local_epoch"] != before["link_local_epoch"] and after["link_state"] and
                            after["link_remote_epoch"] != before["link_remote_epoch"] and after["radio_fresh"]):
                        break
                except (OSError, RuntimeError, ValueError):
                    pass
            else:
                raise RuntimeError("Receiver did not automatically rejoin after restart")
            recovery = dict(receiver_epoch_changed=True, sender_session_changed=True,
                            observed_recovery_seconds=time.monotonic() - start,
                            note="Restart discontinuity; not a continuous loss-free capture")
            (a.output_dir / "receiver-restart.json").write_text(json.dumps(recovery, indent=2) + "\n")
            print(json.dumps(recovery), flush=True)
            if a.shared:
                controller = open_receiver(a.serial, True)
            capture("after-receiver-restart")
        if not all(row["passed"] for row in results):
            raise SystemExit(1)
    finally:
        if controller is not None:
            controller.close()


if __name__ == "__main__":
    main()
