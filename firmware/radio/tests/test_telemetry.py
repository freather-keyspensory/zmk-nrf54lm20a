"""Malformed telemetry rejection and wrap-aware measurement windows."""
import pathlib
import struct
import sys
import unittest
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import telemetry
from capture import unpack_report, valid
from control import encode
from native_capture import decode as decode_native
from pairing import record as pair_record, usb_payload as pair_payload, load_key, image_hash_location
import json
from install_uf2 import target_disks, validate as validate_install


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
                     self.wire[:4] + struct.pack("<I", 11) + self.wire[8:],
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
        v8 = data[:4] + struct.pack("<I", 8) + data[8:]
        security = [2, 1, 0, 100, 100, 80, 80, 1234, 2, 1]
        secured = telemetry.decode(v8 + struct.pack("<24I10I", *fields, *security))
        self.assertTrue(secured["security_dtls"])
        self.assertEqual(secured["control_report_size"], 248)
        self.assertEqual(secured["link_wire_version"], 3)
        self.assertEqual(secured["security_pair_id_crc"], 1234)
        v9 = data[:4] + struct.pack("<I", 9) + data[8:]
        windowed = telemetry.decode(v9 + struct.pack("<24I10I", *fields, *security))
        self.assertEqual(windowed["link_wire_version"], 4)
        self.assertEqual(windowed["security_window_packets"], 4)
        v10 = data[:4] + struct.pack("<I", 10) + data[8:]
        self.assertTrue(telemetry.decode(v10 + struct.pack("<24I10I", *fields, *security))["security_async_receiver"])
        v12 = data[:4] + struct.pack("<I", 12) + data[8:]
        fields[23] = 12345
        ack = telemetry.decode(v12 + struct.pack("<24I10I", *fields, *security))
        self.assertEqual(ack["security_provision_token"], 12345)
        self.assertTrue(ack["security_provision_ack"])
        fields[23] = 0
        v11 = data[:4] + struct.pack("<I", 11) + data[8:]
        self.assertTrue(telemetry.decode(v11 + struct.pack("<24I10I", *fields, *security))["security_wire_channel_hold"])
        self.assertEqual(len(encode("drop-data", 8, 248)), 248)
        security[0] = 3
        with self.assertRaises(ValueError):
            telemetry.decode(v8 + struct.pack("<24I10I", *fields, *security))
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

    def test_wired_pair_record_bounds_and_corruption(self):
        blob = pair_record(bytes(range(16)), bytes(range(32)))
        wire = pair_payload(blob)
        self.assertEqual(len(blob), 64)
        self.assertEqual(len(wire), 248)
        self.assertEqual(struct.unpack_from("<III", wire, 4), (3, 7, 0))
        self.assertEqual(wire[16:80], blob)
        self.assertEqual(struct.unpack_from("<I", wire, 80)[0], 1)
        self.assertEqual(wire[84:], bytes(164))
        self.assertEqual(struct.unpack_from("<I", pair_payload(blob, True), 12)[0], 1)
        for token in (0, -1, 1 << 32):
            with self.assertRaises(ValueError):
                pair_payload(blob, token=token)
        corrupted = bytearray(blob); corrupted[32] ^= 1
        reserved = bytearray(blob); reserved[60] = 1
        for bad in (blob[:-1], blob+b"x", corrupted, reserved):
            with self.assertRaises(ValueError):
                pair_payload(bad)
        for pair_id, secret in ((bytes(16), bytes(range(32))), (bytes(range(16)), bytes(32)),
                                (bytes(15), bytes(range(32))), (bytes(range(16)), bytes(33))):
            with self.assertRaises(ValueError):
                pair_record(pair_id, secret)

    def test_provisioning_ignores_stale_success_and_failure(self):
        from unittest.mock import patch, Mock
        from types import SimpleNamespace
        import pairing
        import reliability_suite
        import zlib
        blob = pair_record(bytes(range(16)), bytes(range(32)))
        token = 12345
        before = dict(security_provision_ack=True, control_report_size=248,
                      security_provision_token=100, security_provision_status=2,
                      security_pair_id_crc=zlib.crc32(blob[8:24]),
                      security_pair_generation=1, security_state=2)
        with tempfile.TemporaryDirectory() as tmp:
            identity = pathlib.Path(tmp)/"test-identity.json"
            uid = bytes(range(8)); identity.write_text(json.dumps(dict(device_id_private=uid.hex())))
            args = SimpleNamespace(identity=identity, replace=False)
            fake_hid = SimpleNamespace(enumerate=lambda *a: [dict(usage_page=0xff00, serial_number=uid[::-1].hex().upper())])
            for final_status in (2, (1 << 32)-17):
                h = Mock(); h.send_feature_report.return_value = 249
                stale = dict(before, security_provision_status=(1 << 32)-22)
                final = dict(before, security_provision_token=token, security_provision_status=final_status,
                             security_pair_generation=2)
                with patch.dict(sys.modules, hid=fake_hid), patch.object(reliability_suite, "open_receiver", return_value=h), \
                     patch.object(pairing.secrets, "randbits", return_value=token), \
                     patch.object(pairing.time, "sleep"), patch.object(telemetry, "read", side_effect=[before, before, stale, final]):
                    if final_status == 2:
                        self.assertEqual(pairing.provision_b(args, blob)["generation"], 2)
                    else:
                        with self.assertRaisesRegex(RuntimeError, "rejected provisioning: -17"):
                            pairing.provision_b(args, blob)
                h.close.assert_called_once()
                self.assertEqual(struct.unpack_from("<I", h.send_feature_report.call_args.args[0], 81)[0], token)

    def test_credential_permissions_and_schema(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp)/"public-test-credentials.json"
            path.write_text(json.dumps(dict(schema=1, pair_id=bytes(range(16)).hex(), secret=bytes(range(32)).hex())))
            path.chmod(0o644)
            with self.assertRaises(ValueError):
                load_key(path)
            path.chmod(0o600)
            self.assertEqual(load_key(path), pair_record(bytes(range(16)), bytes(range(32))))
            path.write_text(json.dumps(dict(schema=2)))
            with self.assertRaises(ValueError):
                load_key(path)

    def test_pairing_image_hash_rejects_ambiguous_and_corrupt_tlvs(self):
        import hashlib
        image = bytearray(0x810)
        struct.pack_into("<IIHHII", image, 0, 0x96f3b83d, 0, 0x800, 0, 16, 0)
        digest = hashlib.sha256(image).digest()
        tlv = struct.pack("<BBH", 0x10, 0, 32) + digest
        signed = image + struct.pack("<HH", 0x6907, 4+len(tlv)) + tlv
        self.assertEqual(image_hash_location(signed), (len(image)+8, digest))
        duplicate = image + struct.pack("<HH", 0x6907, 4+2*len(tlv)) + tlv+tlv
        corrupt = bytearray(signed); corrupt[40] ^= 1
        missing = image + struct.pack("<HH", 0x6907, 4)
        bad_length = bytearray(signed); struct.pack_into("<H", bad_length, len(image)+6, 33)
        for bad in (duplicate, corrupt, missing, bad_length, signed[:-1]):
            with self.assertRaises(ValueError):
                image_hash_location(bad)

    def test_usb_disk_identity_excludes_inherited_interface_properties(self):
        serial = "0011223344556677"
        interface = dict(IOObjectClass="IOUSBHostInterface", IORegistryEntryID=2,
                         idVendor=0x2fe3, idProduct=8, **{"USB Serial Number": serial,
                         "IORegistryEntryChildren": [{"BSD Name": "disk9"}]})
        device = dict(IOObjectClass="IOUSBHostDevice", IORegistryEntryID=1,
                      idVendor=0x2fe3, idProduct=8, **{"USB Serial Number": serial,
                      "IORegistryEntryChildren": [interface]})
        self.assertEqual(target_disks([device], serial), {"disk9"})
        self.assertEqual(target_disks([device], "BAD"), set())
        other = dict(device, IORegistryEntryID=3)
        with self.assertRaises(RuntimeError):
            target_disks([device, other], serial)

    def test_installer_reconstruction_and_application_bounds(self):
        image = bytearray(0x910)
        struct.pack_into("<IIHHII", image, 0, 0x96f3b83d, 0, 0x800, 0, 0x100, 0)
        struct.pack_into("<II", image, 0x800, 0x20001000, 0x20809)
        padded = image + bytes([255]) * (-len(image) % 256)
        blocks = []
        for i in range(len(padded)//256):
            block = bytearray(512)
            struct.pack_into("<8I", block, 0, 0x0a324655, 0x9e5d5157, 0x2000,
                             0x20000+i*256, 256, i, len(padded)//256, 0x9b7316e2)
            block[32:288] = padded[i*256:(i+1)*256]
            struct.pack_into("<I", block, 508, 0x0ab16f30)
            blocks.append(block)
        uf2 = b"".join(blocks)
        validate_install(uf2, image)
        bad_address = bytearray(uf2); struct.pack_into("<I", bad_address, 12, 0)
        bad_image = bytearray(image); struct.pack_into("<I", bad_image, 0x804, 0x1001)
        corrupt = bytearray(uf2); corrupt[32+40] ^= 1
        for data, signed in ((uf2[:-1], image), (uf2[:-512], image),
                             (bad_address, image), (corrupt, image), (uf2, bad_image)):
            with self.assertRaises(ValueError):
                validate_install(data, signed)


if __name__ == "__main__":
    unittest.main()
