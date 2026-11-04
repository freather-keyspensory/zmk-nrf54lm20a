#!/usr/bin/env python3
"""Raw SWD memory transport for the connected Minew bring-up target.

No core attachment, auto-unlock, erase, UICR or OTP programming commands.
"""
import re
import struct
import subprocess
from pathlib import Path

class RawSWD:
    def __init__(self, logs, speed=100):
        self.logs = Path(logs)
        self.logs.mkdir(parents=True, exist_ok=True)
        self.speed = speed
        self.sequence = 0

    def run(self, operations):
        prefix = ['si SWD', f'speed {self.speed}', 'SWDSelect',
                  'SWDWriteDP 0 0x1E', 'SWDWriteDP 1 0x50000000',
                  'SWDWriteDP 2 0', 'SWDWriteAP 0 0x23000052']
        commands = prefix + operations + ['SWDReadDP 1', 'exit']
        self.sequence += 1
        log = self.logs / f'transfer-{self.sequence:05d}.log'
        try:
            result = subprocess.run(['JLinkExe', '-NoGui', '1'],
                input='\n'.join(commands)+'\n', text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
        except subprocess.TimeoutExpired as error:
            partial = error.stdout or b''
            if isinstance(partial, bytes): partial = partial.decode(errors='replace')
            log.write_text(partial + '\nHost timeout after 15 seconds\n')
            raise RuntimeError(f'SWD transport timeout: {log}') from error
        log.write_text(result.stdout + f"\nHost process exit code: {result.returncode}\n")
        if result.returncode or re.search(r'ERROR|FAILED', result.stdout, re.I):
            raise RuntimeError(f'SWD transaction failed: {log}')
        if '0x6BA02477' not in result.stdout:
            raise RuntimeError(f'Unexpected or missing debug-port ID: {log}')
        status = re.findall(r'Read DP register 1 = 0x([0-9A-Fa-f]+)', result.stdout)
        if len(status) != 1 or int(status[0], 16) & 0xA2:
            raise RuntimeError(f'SWD sticky error or missing status: {log}')
        return [int(v, 16) for v in re.findall(
            r'Read DP register 3 = 0x([0-9A-Fa-f]+)', result.stdout)]

    @staticmethod
    def read_ops(address):
        return [f'SWDWriteAP 1 0x{address:08X}', 'SWDReadAP 3', 'SWDReadDP 3']

    @staticmethod
    def write_ops(address, value):
        return [f'SWDWriteAP 1 0x{address:08X}', f'SWDWriteAP 3 0x{value:08X}']

    def read_words(self, addresses):
        addresses = list(addresses)
        if any(a % 4 for a in addresses):
            raise ValueError('Unaligned address')
        values = self.run([op for a in addresses for op in self.read_ops(a)])
        if len(values) != len(addresses):
            raise RuntimeError('Incomplete read response')
        return values

    def read(self, address, size):
        if address % 4 or size % 4:
            raise ValueError('Read must be word aligned')
        data = bytearray()
        while len(data) < size:
            current = address + len(data)
            length = min(1024-current%1024, size-len(data))
            # AP reads are posted; the first returned AP value is stale.
            ops = [f'SWDWriteAP 1 0x{current:08X}']
            ops += ['SWDReadAP 3'] * (length//4)
            ops += ['SWDReadDP 3']
            before = self.sequence
            final = self.run(ops)
            output = (self.logs/f'transfer-{before+1:05d}.log').read_text()
            posted = [int(v,16) for v in re.findall(
                r'Read AP register 3 = 0x([0-9A-Fa-f]+)',output)]
            if len(posted) != length//4 or len(final) != 1:
                raise RuntimeError('Incomplete block read')
            words = posted[1:] + final
            data.extend(struct.pack('<'+'I'*len(words),*words))
        return bytes(data)

    def identify(self):
        part, variant, ram, rram = self.read_words(
            [0x00FFC31C, 0x00FFC320, 0x00FFC328, 0x00FFC32C])
        if (part, ram, rram) != (0x054BC20A, 512, 2036):
            raise RuntimeError('Target does not match verified nRF54LM20A')
        return dict(part=hex(part), variant=hex(variant), ram_kib=ram, rram_kib=rram)
