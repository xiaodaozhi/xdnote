"""Negative cases for actual-native declarations, notices and sealed archives."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import compatibility_audit as abi
import developer_archive as archive
import runtime_materials as runtime
import qt_source_access as sources


def pe():
    data=bytearray(512); data[:2]=b'MZ'; struct.pack_into('<I',data,60,128); data[128:132]=b'PE\0\0'
    struct.pack_into('<H',data,132,0x8664); struct.pack_into('<H',data,148,240); struct.pack_into('<H',data,152,0x20b)
    struct.pack_into('<HH',data,192,6,0); struct.pack_into('<HH',data,200,6,0)
    return data


def macho(cpu=0x100000c,minimum=13):
    data=bytearray(56); struct.pack_into('<IIIIIIII',data,0,0xfeedfacf,cpu,0,6,1,24,0,0)
    struct.pack_into('<IIIIII',data,32,0x32,24,1,minimum<<16,15<<16,0)
    return data


class DeliveryTests(unittest.TestCase):
    def test_pe_declarations_and_corrupt_headers(self):
        self.assertEqual(abi.pe_header(pe())['architecture'],'x86_64')
        with self.assertRaises(ValueError): abi.pe_header(pe()[:180])
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); data=pe(); struct.pack_into('<H',data,192,11); (root/'app.exe').write_bytes(data)
            with self.assertRaisesRegex(ValueError,'Unsupported PE'): abi.audit(root,'Windows','x86_64')

    def test_only_imported_elf_versions_are_checked(self):
        text="Version definition section '.gnu.version_d':\nName: GLIBC_99.0\nVersion needs section '.gnu.version_r':\nName: GLIBC_2.35\nName: GLIBCXX_3.4.30\nName: CXXABI_1.3.13\n"
        names=abi.needed_versions(text); self.assertNotIn('GLIBC_99.0',names)
        policy=json.loads((abi.ROOT/'tools/delivery-policy.json').read_text())['Linux']
        abi.check_versions(names,policy)
        for bad in ('GLIBC_2.36','GLIBCXX_3.4.31','CXXABI_1.3.14','GLIBC_ABI_DT_RELR'):
            with self.subTest(bad=bad),self.assertRaises(ValueError): abi.check_versions([bad],policy)

    def test_macho_every_slice_and_minimum_must_be_supported(self):
        thin=macho(); self.assertEqual(abi.macho_headers(thin)[0]['minimumOs'],[13,0,0])
        fat=struct.pack('>IIIIIII',0xcafebabe,1,0x100000c,0,28,len(thin),0)+thin
        self.assertEqual(abi.macho_headers(fat)[0]['architecture'],'arm64')
        with self.assertRaises(ValueError): abi.macho_headers(fat[:-1])
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); (root/'app').write_bytes(macho(minimum=14))
            with self.assertRaisesRegex(ValueError,'Unsupported Mach-O'): abi.audit(root,'Darwin','arm64')

    def test_runtime_notice_bytes_and_compiler_identity_are_bound(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); base=root/'toolchain/share/doc/rust'; base.mkdir(parents=True)
            (base/'COPYRIGHT-library.html').write_bytes(b'original copyright')
            config={'version':'fixture','compilerCommit':'abc','files':[{'path':'COPYRIGHT-library.html','sha256':hashlib.sha256(b'original copyright').hexdigest()}]}
            def command(args,**kwargs): return str(root/'toolchain') if '--print' in args else 'commit-hash: abc\n'
            with patch.object(runtime,'config',return_value=config),patch.object(runtime.subprocess,'check_output',side_effect=command):
                runtime.collect(root/'notices'); runtime.verify(root/'notices')
                (root/'notices/COPYRIGHT-library.html').write_bytes(b'changed')
                with self.assertRaisesRegex(ValueError,'digest'): runtime.verify(root/'notices')

    def fixture(self,root):
        payload=root/'payload'; payload.mkdir(); (payload/'xdnote.exe').write_bytes(pe()); (payload/'notice.txt').write_text('license')
        metadata={'ok':True,'files':archive.inventory(payload),'buildIdentity':{'sourceSha256':'fixture'}}
        compatibility={'ok':True,'files':[{'path':'xdnote.exe','sha256':archive.digest(payload/'xdnote.exe')}],'scope':'fixture static check'}
        return payload,metadata,compatibility

    def test_archives_are_reproducible_and_never_publication_ready(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); payload,metadata,compatibility=self.fixture(root)
            result=archive.build(payload,metadata,compatibility,root/'developer.zip')
            self.assertTrue(result['archiveReproducible']); self.assertFalse(result['publicationAllowed']); self.assertFalse(result['releaseReady'])
            archive.build(payload,metadata,compatibility,root/'developer.tar.xz')

    def test_qt_sources_or_stale_and_incomplete_inputs_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); payload,metadata,compatibility=self.fixture(root)
            compatibility['files']=[]
            with self.assertRaisesRegex(ValueError,'incomplete'): archive.build(payload,metadata,compatibility,root/'bad.zip')
            (payload/'qtbase.cpp').write_text('source'); metadata['files']=archive.inventory(payload)
            with self.assertRaisesRegex(ValueError,'Source/SDK'): archive.verify(payload,metadata)
            (payload/'qtbase.cpp').unlink(); metadata['files']=archive.inventory(payload); (payload/'notice.txt').write_text('changed')
            with self.assertRaisesRegex(ValueError,'sealed'): archive.verify(payload,metadata)

    def test_unknown_qt_module_cannot_get_a_fabricated_source_offer(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError,'lacks'): sources.collect(Path(directory),[{'sbom':'unknown-6.8.2.spdx.json'}])


if __name__=='__main__': unittest.main()
