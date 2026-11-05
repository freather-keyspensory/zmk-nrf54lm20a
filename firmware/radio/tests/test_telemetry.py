"""Malformed telemetry rejection and wrap-aware measurement windows."""
import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import telemetry


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
                     self.wire[:4] + struct.pack("<I", 2) + self.wire[8:],
                     self.wire[:-4] + struct.pack("<I", 1)):
            with self.subTest(data=data), self.assertRaises(ValueError):
                telemetry.decode(data)

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


if __name__ == "__main__":
    unittest.main()
