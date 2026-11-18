"""Validate source-proof code evidence without loading an inspected DLL."""
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from verify_qt_replacement import pe_code_sections
from linkage_audit import classify, linux_text_evidence, macos_text_evidence, selected_members, windows_text_evidence


class QtReplacementEvidence(unittest.TestCase):
    def test_macos_text_reports_only_live_symbols_and_separates_unconfirmed_names(self):
        link_map = '''# Arch: arm64
# Object files:
[ 1] libxdnote_services.a(std-abc.first.o)
[ 2] libxdnote_services.a(core-def.second.o)
# Sections:
0x1000 0x10 __TEXT __text
# Symbols:
0x1000 0x04 [ 1] _confirmed
0x1004 0x04 [ 1] _map_only
# Dead Stripped Symbols:
0x1008 0x04 [ 2] _dead
'''
        sections = 'sectname __text\nsegname __TEXT\naddr 0x1000\nsize 0x10\noffset 8\n'
        symbols = '0000000000001000 t _confirmed\n'
        binary = bytes(24)
        evidence = macos_text_evidence(link_map, sections, symbols, binary)
        self.assertEqual(evidence['textBytes'], 16)
        self.assertEqual(evidence['members'], [
            {'member': 'core-def.second.o', 'mappedSymbols': 0, 'mappedBytes': 0,
             'confirmedSymbols': 0, 'confirmedBytes': 0},
            {'member': 'std-abc.first.o', 'mappedSymbols': 2, 'mappedBytes': 8,
             'confirmedSymbols': 1, 'confirmedBytes': 4},
        ])
        with self.assertRaisesRegex(ValueError, 'text bounds'):
            macos_text_evidence(link_map, sections.replace('0x10\noffset', '0x11\noffset'), symbols, binary)
        with self.assertRaisesRegex(ValueError, 'overlapping'):
            macos_text_evidence(link_map.replace('0x1004 0x04', '0x1002 0x04'), sections, symbols, binary)
        with self.assertRaisesRegex(ValueError, 'No mapped Rust text symbols'):
            macos_text_evidence(link_map, sections, '', binary)

    def test_toolchain_attribution_requires_the_archive_hash(self):
        members = ['std-abc.std.cgu.0.rcgu.o', 'std-def.std.cgu.0.rcgu.o']
        matching, changed = classify(members, [], [('std', 'abc')])
        self.assertEqual(matching['kind'], 'Rust toolchain source unit')
        self.assertEqual(matching['archiveHash'], 'abc')
        self.assertEqual(changed['kind'], 'unclassified Rust source unit')

    def test_msvc_archive_members_and_import_descriptors_keep_distinct_evidence(self):
        text = '''0001:00000270 symbol 0000000140001270 f xdnote_services:xdnote_markdown-f9b650fb0abc18ad.xdnote_markdown.cgu.07.rcgu.o
0001:00000300 symbol 0000000140001300 f xdnote_services.lib:std_detect-2e160def0ea9d132.std_detect.cgu.0.rcgu.o
0002:00000400 symbol 0000000140001400 xdnote_services:api-ms-win-core-synch-l1-2-0.dll'''
        members = selected_members(text)
        self.assertEqual(len(members), 3)
        records = classify(members, [], [('std_detect', '2e160def0ea9d132')])
        self.assertEqual({r['kind'] for r in records}, {'project code', 'Rust toolchain source unit', 'PE import descriptor'})
        self.assertEqual(records[0]['name'], 'api-ms-win-core-synch-l1-2-0.dll')

    def test_gnu_text_requires_live_map_and_real_matching_symbol(self):
        link_map = """Discarded input sections
 .text.dead 0x0 0x4 libxdnote_services.a(dead-abc.o)
Linker script and memory map
.text 0x1000 0x10
 .text.unlikely.confirmed
                0x1000 0x04 libxdnote_services.a(core-def.o)
 .text.alias    0x1004 0x04 libxdnote_services.a(core-def.o)
.rodata 0x2000 0x02
 .text.unrelated 0x2000 0x02 libxdnote_services.a(other-abc.o)
"""
        symbols = 'confirmed t 1000 4\ndifferent_name T 1004 4\n'
        row, = linux_text_evidence(link_map, (0x1000, 4, 16), symbols, bytes(20))['members']
        self.assertEqual(row, dict(member='core-def.o', mappedRanges=2, mappedBytes=8,
                                   confirmedRanges=1, confirmedBytes=4))
        with self.assertRaisesRegex(ValueError, 'bounds differ'):
            linux_text_evidence(link_map, (0x1004, 4, 16), symbols, bytes(20))
        with self.assertRaisesRegex(ValueError, 'overlap'):
            linux_text_evidence(link_map.replace('0x1004 0x04', '0x1002 0x04'),
                                (0x1000, 4, 16), symbols, bytes(20))

    def test_windows_text_requires_unambiguous_unwind_address(self):
        data = bytearray(1024); data[:2] = b'MZ'
        struct.pack_into('<I', data, 0x3c, 64)
        data[64:68] = b'PE\0\0'
        struct.pack_into('<HH', data, 68, 0x8664, 2)
        struct.pack_into('<H', data, 84, 240); struct.pack_into('<H', data, 88, 0x20b)
        struct.pack_into('<Q', data, 112, 0x140000000)
        struct.pack_into('<I', data, 196, 16)
        struct.pack_into('<II', data, 224, 8192, 24)
        struct.pack_into('<8sIIIIIIHHI', data, 328, b'.text', 32, 4096, 32, 512, 0, 0, 0, 0, 0x60000020)
        struct.pack_into('<8sIIIIIIHHI', data, 368, b'.pdata', 24, 8192, 24, 544, 0, 0, 0, 0, 0x40000040)
        struct.pack_into('<IIIIII', data, 544, 4096, 4100, 10000, 4100, 4104, 10008)
        link_map = """ Preferred load address is 0000000140000000
 0001:00000000 f1 0000000140001000 f xdnote_services:std-abc.o
 0001:00000004 f2 0000000140001004 f xdnote_services.lib:std-abc.o
 0001:00000004 alias 0000000140001004 f other.lib:alias.obj
 0001:00000008 leaf 0000000140001008 f xdnote_services:std-abc.o
"""
        row, = windows_text_evidence(link_map, data)['members']
        self.assertEqual(row, dict(member='std-abc.o', mappedAddresses=3, confirmedRanges=1,
                                   confirmedBytes=4, ambiguousAddresses=1, mapOnlyAddresses=1))
        with self.assertRaisesRegex(ValueError, 'disagrees with'):
            windows_text_evidence(link_map.replace('0000000140001008', '0000000140001009'), data)

    def test_pe_evidence_selects_actual_code_and_rejects_truncation(self):
        data = bytearray(1024); data[:2] = b'MZ'; struct.pack_into('<I', data, 0x3c, 64)
        data[64:68] = b'PE\0\0'; struct.pack_into('<HH', data, 68, 0x8664, 2)
        struct.pack_into('<H', data, 84, 240); struct.pack_into('<H', data, 88, 0x20b)
        struct.pack_into('<8sIIIIIIHHI', data, 328, b'.text', 4, 4096, 4, 512, 0, 0, 0, 0, 0x60000020)
        struct.pack_into('<8sIIIIIIHHI', data, 368, b'.data', 4, 8192, 4, 516, 0, 0, 0, 0, 0xc0000040)
        data[512:520] = b'codeDATA'; baseline = pe_code_sections(data)
        data[516] ^= 1; self.assertEqual(pe_code_sections(data), baseline)
        data[512] ^= 1; self.assertNotEqual(pe_code_sections(data), baseline)
        for changed in (data[:90], data[:514], b'not executable'):
            with self.assertRaises(ValueError): pe_code_sections(changed)
        struct.pack_into('<I', data, 364, 0x40000040)
        with self.assertRaises(ValueError): pe_code_sections(data)
