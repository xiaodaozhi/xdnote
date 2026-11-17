"""Validate source-proof code evidence without loading an inspected DLL."""
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from verify_qt_replacement import pe_code_sections


class QtReplacementEvidence(unittest.TestCase):
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
