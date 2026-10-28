# SPDX-License-Identifier: GPL-2.0-or-later
"""Native loader checks using tiny synthetic images, no proprietary game data."""
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'out/iss-probe.exe'

def boot(code=b'\xc3', entry=0, segments=None, names=(), relocs=(), magic=b'ISSPRB01', payload=None):
    segments = segments if segments is not None else [(0, 4096, 5), (4096, 4096, 6)]
    payload = payload if payload is not None else code.ljust(8192, b'\0')
    return (struct.pack('<8s5Q', magic, len(payload), entry, len(segments), len(relocs), len(names))
            + b''.join(struct.pack('<3Q', *s) for s in segments)
            + b''.join(n if len(n) == 128 else n.ljust(128, b'\0') for n in names)
            + b''.join(struct.pack('<4Q', *r) for r in relocs) + payload)

class NativeProbeTests(unittest.TestCase):
    def run_image(self, data, entry=False):
        with tempfile.TemporaryDirectory(prefix='iss-probe-', dir=ROOT / 'out') as directory:
            path = Path(directory) / 'fixture.bin'
            path.write_bytes(data)
            return subprocess.run([str(EXE), str(path), '--entry' if entry else '--map-only'],
                                  capture_output=True, text=True, timeout=5)

    def reject(self, data, reason):
        r = self.run_image(data)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn(reason, r.stderr)

    def test_map_does_not_execute_ud2(self):
        r = self.run_image(boot(b'\x0f\x0b'))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('"guest_executed":false', r.stdout)

    def test_sysv_argument_and_function_import(self):
        # Read argc through RDI, assert 1, then JMP through the relocated import GOT.
        code = bytes.fromhex('8b07 83f801 7506 ff25f30f0000 0f0b')
        r = self.run_image(boot(code, names=[b'test#A#A'], relocs=[(4096, 1, 0, 0)]), True)
        self.assertEqual(r.returncode, 20, r.stdout + r.stderr)
        event = json.loads(next(x[12:] for x in r.stdout.splitlines() if x.startswith('PROBE_EVENT ')))
        self.assertEqual(event['status'], 'unsupported_import')
        self.assertEqual(event['symbol'], 'test#A#A')

    def test_data_import_stops_on_read(self):
        # MOVABS RAX,data-import; MOV EAX,[RAX]; RET.
        code = bytes.fromhex('48b8') + bytes(8) + bytes.fromhex('8b00c3')
        r = self.run_image(boot(code, names=[b'data#A#A'], relocs=[(2, 2, 0, 0)]), True)
        self.assertEqual(r.returncode, 20, r.stdout + r.stderr)
        self.assertIn('unsupported_data_import', r.stdout)

    def test_relative_relocation_applied_before_protection(self):
        # Address of data is base+0x1000; verify its value before returning.
        code = bytes.fromhex('48b8') + bytes(8) + bytes.fromhex('48813834120000 7501 c3 0f0b')
        payload = bytearray(code.ljust(8192, b'\0'))
        struct.pack_into('<Q', payload, 4096, 0x1234)
        r = self.run_image(boot(payload=bytes(payload), relocs=[(2, 0, 4096, 0)]), True)
        self.assertEqual(r.returncode, 22, r.stdout + r.stderr)
        self.assertIn('entry_returned', r.stdout)

    def test_guest_fault_has_diagnostic_exit(self):
        r = self.run_image(boot(b'\x0f\x0b'), True)
        self.assertEqual(r.returncode, 21)
        self.assertIn('guest_fault', r.stdout)

    def test_bad_magic(self):
        self.reject(boot(magic=b'UNKNOWN!'), 'bad boot magic')

    def test_nonexecutable_entry(self):
        self.reject(boot(entry=4096), 'entry is not executable')

    def test_segment_overlap(self):
        self.reject(boot(segments=[(0, 4097, 5), (4096, 4096, 6)]), 'overlapping segment pages')

    def test_relocation_crossing_segment(self):
        self.reject(boot(relocs=[(4092, 0, 0, 0)]), 'invalid relocation')

    def test_bad_import_index(self):
        self.reject(boot(names=[b'test#A#A'], relocs=[(4096, 1, 1, 0)]), 'invalid relocation')

    def test_unknown_relocation(self):
        self.reject(boot(relocs=[(4096, 16, 0, 0)]), 'invalid relocation')

    def test_truncated_image(self):
        self.reject(boot()[:-1], 'wrong image payload length')

    def test_trailer_rejected(self):
        self.reject(boot() + b'x', 'wrong image payload length')

    def test_unterminated_name(self):
        self.reject(boot(names=[b'A' * 128]), 'invalid import name')

    def test_json_unsafe_name_rejected(self):
        self.reject(boot(names=[b'test"#A#A']), 'unexpected import spelling')

if __name__ == '__main__':
    unittest.main()
