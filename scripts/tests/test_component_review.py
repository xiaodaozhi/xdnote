"""Real SPDX graph shapes preserve declaration gaps without inventing inclusion."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from component_review import dependency_review
from component_review import cocoa_derivation, network_derivation, review
from fetch_delivery_evidence import fetch


class ComponentReviewTests(unittest.TestCase):
    def test_mixed_builds_and_non_numeric_source_runs_are_rejected_before_io(self):
        with self.assertRaises(ValueError): review({'ok': True, 'buildIdentity': {'revision': 'one'}}, {'ok': True, 'buildIdentity': {'revision': 'two'}}, Path('unused'))
        with self.assertRaises(ValueError): fetch('$(untrusted)', 'windows', Path('unused'))
        run = {'conclusion': 'success', 'status': 'completed', 'repository': {'full_name': 'xiaodaozhi/xdnote'},
               'head_repository': {'full_name': 'untrusted/fork'}, 'event': 'workflow_dispatch', 'path': '.github/workflows/qt-replacement.yml'}
        with tempfile.TemporaryDirectory() as folder, patch('fetch_delivery_evidence.api', return_value=run) as calls:
            with self.assertRaises(ValueError): fetch('123', 'windows', Path(folder) / 'output')
            self.assertEqual(calls.call_count, 1)
    def fixture(self):
        package = lambda name: {'SPDXID': name, 'name': name, 'licenseConcluded': 'MIT OR Apache-2.0', 'licenseDeclared': 'NOASSERTION', 'copyrightText': 'upstream'}
        edge = lambda left, kind, right: {'spdxElementId': left, 'relationshipType': kind, 'relatedSpdxElement': right}
        base = {'documentNamespace': 'base', 'packages': [package(x) for x in ['Core', 'Pcre', 'Compiler', 'Unused']],
                'files': [{'SPDXID': 'core.dll'}], 'relationships': [edge('Core', 'CONTAINS', 'core.dll'), edge('Core', 'DEPENDS_ON', 'Pcre'),
                edge('Pcre', 'DEPENDS_ON', 'Core'), edge('Compiler', 'BUILD_DEPENDENCY_OF', 'Core'), edge('Unused', 'DEPENDS_ON', 'MissingUnused')]}
        svg = {'documentNamespace': 'svg', 'packages': [package('Svg')], 'files': [{'SPDXID': 'svg.dll'}],
               'externalDocumentRefs': [{'externalDocumentId': 'DocumentRef-base', 'spdxDocument': 'base', 'checksum': {'algorithm': 'SHA1', 'checksumValue': 'a'*40}}],
               'relationships': [edge('Svg', 'CONTAINS', 'svg.dll'), edge('Svg', 'DEPENDS_ON', 'DocumentRef-base:Core')]}
        return {'base': base, 'svg': svg}

    def test_actual_file_roots_follow_runtime_edges_and_cycles_without_build_tools(self):
        result = dependency_review(self.fixture(), {('svg', 'svg.dll')}, {'base': {'sha1': 'a'*40}})
        rows = result['supplierDependencyPackages']; self.assertEqual({r['spdxId'] for r in rows}, {'Core', 'Pcre', 'Svg'})
        self.assertEqual(result['notReachedPackageCount'], 2); self.assertFalse(result['unresolvedReferences'])
        self.assertEqual(rows[0]['licenseConcluded'], 'MIT OR Apache-2.0'); self.assertEqual(rows[0]['unresolvedFields'], ['licenseDeclared'])

    def test_stale_external_checksum_is_not_repaired_by_matching_package_names(self):
        docs = self.fixture(); original = copy.deepcopy(docs)
        result = dependency_review(docs, {('svg', 'svg.dll')}, {'base': {'sha1': 'b'*40}})
        self.assertEqual({r['spdxId'] for r in result['supplierDependencyPackages']}, {'Svg'})
        self.assertEqual(len(result['unresolvedReferences']), 1); self.assertFalse(result['externalDocumentChecks'][0]['matches'])
        self.assertEqual(docs, original)

    def test_missing_actual_owner_and_duplicate_identity_fail(self):
        docs = self.fixture()
        with self.assertRaises(ValueError): dependency_review(docs, {('base', 'not-delivered')}, {})
        docs['base']['packages'].append(docs['base']['packages'][0])
        with self.assertRaises(ValueError): dependency_review(docs, {('base', 'core.dll')}, {})

    def test_supplier_sdk_reviews_a_derived_linux_library_without_substituting_hashes(self):
        from qt_source_access import config

        with tempfile.TemporaryDirectory() as folder:
            qt = Path(folder)
            path = 'lib/libQt6Network.so.6.8.2'
            binary = qt / path
            binary.parent.mkdir()
            binary.write_bytes(b'original supplier bytes')
            supplier_sha = hashlib.sha256(binary.read_bytes()).hexdigest()
            prepared_sha = hashlib.sha256(b'derived bytes').hexdigest()
            doc = self.fixture()['base']
            doc['spdxVersion'] = 'SPDX-2.3'
            doc['files'][0]['fileName'] = './' + path
            sbom = 'qtbase-6.8.2.spdx.json'
            (qt / 'sbom').mkdir()
            spdx_bytes = json.dumps(doc).encode()
            (qt / 'sbom' / sbom).write_bytes(spdx_bytes)
            source = next(row for row in config()['modules'] if row['module'] == 'qtbase')
            files = {path: {'sha256': prepared_sha, 'originalSdkSha256': supplier_sha}}
            backport = {'source': source, 'files': files, 'sourceCodeInRelease': False}
            receipt = {**backport, 'system': 'Linux', 'files': {path: {**files[path], 'originalBackup': '/ci/private'}}}
            identity = {'dependencies': {'qt': {'version': '6.8.2'}}, 'qtNetworkBackports': receipt}
            linkage = {'ok': True, 'buildIdentity': identity, 'members': []}
            material = {'supplier': {'sbom': sbom, 'spdxId': 'core.dll'}, 'sdkFile': path,
                        'sdkSha256': prepared_sha, 'nativeSectionsMatchSdk': True, 'sourceBackport': backport}
            evidence = {'ok': True, 'buildIdentity': identity, 'files': [{'path': path, 'evidence': material}],
                        'qtModules': [{'sbom': sbom, 'sha256': hashlib.sha256(spdx_bytes).hexdigest()}],
                        'qtSourceAccess': {'networkBackports': backport}}

            self.assertEqual(review(evidence, linkage, qt)['actualQtSourceBackports'], files)
            for key, value in [('sdkSha256', supplier_sha), ('sourceBackport', None), ('nativeSectionsMatchSdk', False)]:
                with self.subTest(key=key), patch.dict(material, {key: value}):
                    with self.assertRaises(ValueError):
                        review(evidence, linkage, qt)
            binary.write_bytes(b'changed supplier bytes')
            with self.assertRaises(ValueError):
                review(evidence, linkage, qt)

    def test_windows_compact_payload_requires_sealed_source_access_digest(self):
        from qt_source_access import config, manifest

        source = next(row for row in config()['modules'] if row['module'] == 'qtbase')
        files = {name: {'sha256': 'a' * 64, 'originalSdkSha256': 'b' * 64}
                 for name in ('bin/Qt6Network.dll', 'lib/Qt6Network.lib')}
        public = {'source': source, 'files': files, 'sourceCodeInRelease': False}
        receipt = {**public, 'system': 'Windows'}
        modules = [{'sbom': 'qtbase-6.8.2.spdx.json'}]
        instructions = {'path': 'licenses/qt/REPLACEMENT.md', 'sha256': 'f' * 64}
        encoded = (json.dumps(manifest(modules, public, replacement_sha256=instructions['sha256']), indent=2)
                   + '\n').replace('\n', '\r\n').encode('utf-8')
        sealed = {'path': 'licenses/qt/source-access.json', 'sha256': hashlib.sha256(encoded).hexdigest()}
        evidence = {'kind': 'unsigned product-only Windows installer payload', 'qtSbom': modules,
                    'buildIdentity': {'qtNetworkBackports': receipt}, 'files': [sealed, instructions]}
        self.assertEqual(network_derivation(evidence), public)
        sealed['sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'Sealed Windows Qt source access digest differs'):
            network_derivation(evidence)

    def test_cocoa_derivation_requires_the_fixed_pair_and_both_receipts(self):
        from qt_cocoa_backports import MODULES, config, qtbase_source

        public = {'system': 'Darwin', 'architecture': 'arm64', 'source': qtbase_source(),
                  'patches': config()['patches'], 'sourceCodeInRelease': False,
                  'files': {name: {'sha256': 'a' * 64, 'originalSdkSha256': 'b' * 64} for name in MODULES}}
        private = {**public, 'originalBackups': {name: '/ci/private' for name in MODULES}}
        identity = {'platform': 'Darwin', 'qtCocoaBackports': private}
        materials = {'cocoaBackports': public}
        evidence = {'buildIdentity': identity, 'qtSourceAccess': materials}
        self.assertEqual(cocoa_derivation(evidence), public)
        with patch.dict(materials, {'cocoaBackports': {**public, 'originalBackups': private['originalBackups']}}):
            with self.assertRaisesRegex(ValueError, 'differs across'):
                cocoa_derivation(evidence)
        with patch.dict(private, {'files': {next(iter(MODULES)): {}}}):
            with self.assertRaisesRegex(ValueError, 'derivation scope'):
                cocoa_derivation(evidence)
