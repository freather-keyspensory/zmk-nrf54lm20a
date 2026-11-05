"""Malformed telemetry rejection and wrap-aware measurement windows."""
import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import telemetry
from capture import unpack_report, valid


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
                     self.wire[:4] + struct.pack("<I", 3) + self.wire[8:],
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


if __name__ == "__main__":
    unittest.main()
