"""Real SPDX graph shapes preserve declaration gaps without inventing inclusion."""
import copy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from component_review import dependency_review
from component_review import review
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
