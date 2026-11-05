"""Build and read the direct macOS IOHID collector; raw captures stay temporary."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def collect(seconds, serial, ready_file=None, product_id=9):
    if sys.platform != "darwin":
        raise RuntimeError("Direct IOHID capture requires macOS")
    if product_id not in (9,10):
        raise ValueError("Unknown receiver product")
    source = Path(__file__).with_name("capture_macos.c")
    binary = source.parents[1] / "build/host/capture_macos"
    binary.parent.mkdir(parents=True, exist_ok=True)
    if not binary.exists() or source.stat().st_mtime_ns > binary.stat().st_mtime_ns:
        subprocess.run(["clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(source), "-framework", "IOKit", "-framework", "CoreFoundation",
                        "-o", str(binary)], check=True)
    with tempfile.TemporaryDirectory(prefix="minew-capture-") as tmp:
        output = Path(tmp) / "reports.bin"
        subprocess.run([str(binary), str(seconds), str(output), serial,
                        str(ready_file) if ready_file else "", str(product_id)], check=True)
        reports, start, stop = decode(output.read_bytes())
        if (stop - start) / 1e9 < seconds * .99:
            raise RuntimeError("IOHID capture ended before the requested observation window")
        return reports, start, stop


def decode(blob):
    if len(blob) < 64:
        raise ValueError("Truncated native capture header")
    magic, version, size, numer, denom, start, stop, count, errors, overflow = struct.unpack_from("<8s4I5Q", blob)
    if (magic != b"M8CAP\0\0\0" or version != 1 or size != 80 or not numer or not denom or
            errors or overflow or stop <= start or len(blob) != 64 + count * size):
        raise ValueError("Invalid or incomplete native capture")
    reports = []
    previous = start
    for offset in range(64, len(blob), size):
        tick, length, reserved = struct.unpack_from("<QII", blob, offset)
        if reserved or length > 64 or not previous <= tick <= stop:
            raise ValueError("Invalid native capture record")
        reports.append((tick * numer // denom, blob[offset + 16:offset + 16 + length]))
        previous = tick
    return reports, start * numer // denom, stop * numer // denom
