# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline linker contracts using synthetic ELF metadata, no game binaries."""
from copy import deepcopy
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from link_modules import build_linked

def symbol(name='', section=0, kind=2, binding=1, version=1, module_version=257):
    return dict(name=name, section=section, type=kind, binding=binding, value=0, size=1,
                identity={'nid': 'TestNid', 'library': {'name': 'testlib', 'version_raw': version},
                          'module': {'name': 'testmodule', 'version_raw': module_version}} if name else None)

def fixture():
    ph = [dict(type=1, flags=5, offset=0, vaddr=0, filesz=1, memsz=4096),
          dict(type=1, flags=6, offset=1, vaddr=4096, filesz=0, memsz=4096),
          dict(type=7, flags=4, offset=1, vaddr=4096, filesz=0, memsz=48, align=8),
          dict(type=0x61000001, flags=4, offset=1, vaddr=4200, filesz=0, memsz=72)]
    main = dict(path='eboot.bin', ph=ph, elf=b'\xc3', header=[0,0,0,0,0], tags={12:0},
                symbols=[symbol(), symbol('TestNid#A#A')], relocs=[(4096,7,1,0)])
    lib = deepcopy(main)
    lib['path'] = 'libc.prx'
    lib['symbols'][1]['section'] = 1
    lib['relocs'] = []
    return main, lib

class OfflineLinkTests(unittest.TestCase):
    def test_exact_identity_links(self):
        raw, report, rows = build_linked(fixture())
        self.assertEqual(raw[:8], b'ISSPRB02')
        self.assertEqual(report['bindings'], 1)
        self.assertEqual(rows[0]['status'], 'native:libc.prx')

    def test_library_version_mismatch_remains_unresolved(self):
        main, lib = fixture()
        lib['symbols'][1]['identity']['library']['version_raw'] = 2
        _, report, rows = build_linked([main, lib])
        self.assertEqual(report['bindings'], 0)
        self.assertEqual(rows[0]['status'], 'unresolved')

    def test_module_version_mismatch_remains_unresolved(self):
        main, lib = fixture()
        lib['symbols'][1]['identity']['module']['version_raw'] = 258
        self.assertEqual(build_linked([main, lib])[1]['bindings'], 0)

    def test_library_name_mismatch_remains_unresolved(self):
        main, lib = fixture()
        lib['symbols'][1]['identity']['library']['name'] = 'otherlib'
        self.assertEqual(build_linked([main, lib])[1]['bindings'], 0)

    def test_ambiguous_export_rejected(self):
        main, lib = fixture()
        with self.assertRaisesRegex(ValueError, 'ambiguous'):
            build_linked([main, lib, deepcopy(lib)])

    def test_export_import_type_mismatch_rejected(self):
        main, lib = fixture()
        lib['symbols'][1]['type'] = 1
        with self.assertRaisesRegex(ValueError, 'type mismatch'):
            build_linked([main, lib])

    def test_tls_module_relocation_uses_owner_id(self):
        main, lib = fixture()
        lib['symbols'][0]['type'] = 3
        lib['relocs'] = [(4104,16,0,0)]
        raw, report, _ = build_linked([main, lib])
        image = raw[-report['image_size']:]
        self.assertEqual(struct.unpack_from('<Q', image, 65536 + 4104)[0], 2)

    def test_external_tls_relocation_rejected(self):
        main, lib = fixture()
        lib['relocs'] = [(4104,16,1,0)]
        with self.assertRaisesRegex(ValueError, 'external TLS'):
            build_linked([main, lib])

    def test_deferred_module_keeps_the_stub_entry(self):
        main, lib = fixture()
        game = deepcopy(main)
        game['path'] = 'game.elf'
        game['header'] = [0, 0, 0, 0, 0x80]
        game['deferred'] = True
        game['relocs'] = []
        game['symbols'] = [symbol()]
        raw, report, _ = build_linked([main, lib, game])
        self.assertEqual(report['entry'], '0x0')
        self.assertTrue(report['module_layout'][2]['deferred'])
        self.assertFalse(report['module_layout'][0]['deferred'])
        nmod, = struct.unpack_from('<Q', raw, 48)
        self.assertEqual(nmod, 3)
        metadata = 72
        record = 9 * 8
        for index, expected in enumerate((0, 0, 1)):
            flags, = struct.unpack_from('<Q', raw, metadata + index * record + 8 * 8)
            self.assertEqual(flags, expected)

    def test_invalid_tls_alignment_rejected(self):
        main, lib = fixture()
        next(p for p in lib['ph'] if p['type'] == 7)['align'] = 3
        with self.assertRaisesRegex(ValueError, 'TLS layout'):
            build_linked([main, lib])

if __name__ == '__main__':
    unittest.main()
