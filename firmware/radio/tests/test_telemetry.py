"""Malformed telemetry rejection and wrap-aware measurement windows."""
import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import telemetry
from capture import unpack_report, valid
from control import encode
from native_capture import decode as decode_native


class TelemetryTest(unittest.TestCase):
    def setUp(self):
        self.wire = struct.pack("<24I", 0x5453384d, 1, 100, 2408, *range(18), 1, 0)

    def test_decode_and_unnumbered_report(self):
        a = telemetry.decode(self.wire)
        self.assertEqual(a, telemetry.decode(b"\0" + self.wire))
        self.assertEqual(a["frequency_mhz"], 2408)
        self.assertEqual(a["usb_completed"], 7)
        self.assertEqual(a["drop_queue_full"], 11)

    def test_reject_malformed(self):
        for data in (self.wire[:-1], self.wire + b"\0", b"BAD!" + self.wire[4:],
                     self.wire[:4] + struct.pack("<I", 8) + self.wire[8:],
                     self.wire[:-4] + struct.pack("<I", 1)):
            with self.subTest(data=data), self.assertRaises(ValueError):
                telemetry.decode(data)

    def test_batch_report_telemetry(self):
        data = self.wire[:4] + struct.pack("<I", 2) + self.wire[8:]
        data += struct.pack("<4I", 1000, 999, 7, 64)
        a = telemetry.decode(b"\0" + data)
        self.assertEqual(a["usb_report_size"], 64)
        b = dict(a, uptime_ms=200, usb_reports_completed=1001)
        self.assertEqual(telemetry.difference(a, b)["counters"]["usb_reports_completed"], 2)
        with self.assertRaises(ValueError):
            telemetry.decode(data[:-4] + struct.pack("<I", 63))
        with self.assertRaises(ValueError):
            telemetry.difference(telemetry.decode(self.wire), b)

    def test_counter_and_uptime_wrap(self):
        a = telemetry.decode(self.wire)
        a.update(uptime_ms=0xfffffff0, radio_fresh=0xfffffffe)
        b = dict(a, uptime_ms=0x20, radio_fresh=3)
        delta = telemetry.difference(a, b)
        self.assertEqual(delta["counter_window_ms"], 48)
        self.assertEqual(delta["counters"]["radio_fresh"], 5)

    def test_reject_reset_or_changed_channel(self):
        a = telemetry.decode(self.wire)
        for b in (dict(a, uptime_ms=10), dict(a, uptime_ms=200, frequency_mhz=2480)):
            with self.assertRaises(ValueError):
                telemetry.difference(a, b)

    def test_one_or_two_frames_without_padding_duplicates(self):
        def frame(seq):
            return b"M8\x01\0" + struct.pack("<I", seq) + bytes((seq + i * 17) & 255 for i in range(8, 32))
        a, b = frame(0xffffffff), frame(0)
        self.assertTrue(valid(a) and valid(b))
        self.assertEqual(unpack_report(a), (a,))
        self.assertEqual(unpack_report(a + bytes(32)), (a,))
        self.assertEqual(unpack_report(a + b), (a, b))
        corrupt = bytearray(b); corrupt[-1] ^= 1
        self.assertFalse(valid(unpack_report(a + corrupt)[1]))
        for data in (b"", a[:-1], a + b"x", a + b[:-1], a + b + b"x"):
            with self.assertRaises(ValueError):
                unpack_report(data)

    def test_session_frame_and_bounded_control(self):
        data = bytearray(b"M8\x02\0" + struct.pack("<I", 100) + bytes((100 + i * 17) & 255 for i in range(8, 28)) + struct.pack("<I", 123))
        self.assertTrue(valid(data))
        data[28:] = bytes(4)
        self.assertFalse(valid(data))
        self.assertEqual(len(encode("restart", 1)), 208)
        for command, value in (("restart", 0), ("pause-radio", 101), ("drop-ack", 65), ("channel", 3), ("drop-data", -1)):
            with self.assertRaises(ValueError):
                encode(command, value)

    def test_reliable_telemetry(self):
        data = self.wire[:4] + struct.pack("<I", 3) + self.wire[8:]
        data += struct.pack("<4I", 1000, 999, 7, 64)
        fields = [0] * 24; fields[0] = 111; fields[1] = 222; fields[2] = 1; fields[7] = 2408
        a = telemetry.decode(data + struct.pack("<24I", *fields))
        self.assertEqual(a["link_remote_epoch"], 222)
        v5 = data[:4] + struct.pack("<I", 5) + data[8:]
        guarded = telemetry.decode(v5 + struct.pack("<24I", *fields))
        self.assertTrue(guarded["link_receive_window_guard"])
        self.assertEqual(guarded["link_wire_version"], 2)
        v6 = data[:4] + struct.pack("<I", 6) + data[8:]
        self.assertTrue(telemetry.decode(v6 + struct.pack("<24I", *fields))["link_idle_detection"])
        v7 = data[:4] + struct.pack("<I", 7) + data[8:]
        self.assertTrue(telemetry.decode(v7 + struct.pack("<24I", *fields))["link_session_purge_safe"])
        b = dict(a, uptime_ms=200, link_injected_ack=8)
        self.assertEqual(telemetry.difference(a, b)["counters"]["link_injected_ack"], 8)
        with self.assertRaises(ValueError):
            telemetry.difference(a, dict(b, link_local_epoch=333))

    def test_native_capture_bounds_and_clock(self):
        header = struct.pack("<8s4I5Q", b"M8CAP\0\0\0", 1, 80, 125, 3, 100, 200, 1, 0, 0)
        record = struct.pack("<QII64s", 150, 32, 0, bytes(32))
        reports, start, stop = decode_native(header + record)
        self.assertEqual(reports, [(6250, bytes(32))])
        self.assertEqual((start, stop), (4166, 8333))
        for blob in (b"", header, header + record[:-1], header + record + b"x",
                     header + struct.pack("<QII64s", 201, 32, 0, bytes(32)),
                     header + struct.pack("<QII64s", 150, 65, 0, bytes(64)),
                     header + struct.pack("<QII64s", 150, 32, 1, bytes(32))):
            with self.subTest(blob=blob), self.assertRaises(ValueError):
                decode_native(blob)

    def test_native_capture_rejects_overflow_or_callback_error(self):
        for errors, overflow in ((1, 0), (0, 1)):
            header = struct.pack("<8s4I5Q", b"M8CAP\0\0\0", 1, 80, 1, 1, 100, 200, 0, errors, overflow)
            with self.assertRaises(ValueError):
                decode_native(header)


if __name__ == "__main__":
    unittest.main()
