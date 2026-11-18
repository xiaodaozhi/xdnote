"""Only checksum-bound serializations with equal review fields can resolve refs."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from spdx_serialization import equivalent, project_text
from component_review import dependency_review
import test_component_review as fixtures


class SpdxSerializationTests(unittest.TestCase):
    def test_multiline_comments_do_not_create_elements_and_used_fields_must_match(self):
        text = '''SPDXVersion: SPDX-2.3
DocumentNamespace: fixture
CreatorComment: <text>comment
PackageName: not-a-package</text>
PackageName: Core
SPDXID: Core
PackageLicenseConcluded: MIT
FileName: ./core.dll
SPDXID: file
Relationship: Core CONTAINS file
'''
        doc = {'spdxVersion': 'SPDX-2.3', 'documentNamespace': 'fixture', 'packages': [{'SPDXID': 'Core', 'name': 'Core', 'licenseConcluded': 'MIT'}],
               'files': [{'SPDXID': 'file', 'fileName': './core.dll'}], 'relationships': [{'spdxElementId': 'Core', 'relationshipType': 'CONTAINS', 'relatedSpdxElement': 'file'}]}
        self.assertTrue(equivalent(text, doc)); changed = copy.deepcopy(doc); changed['packages'][0]['licenseConcluded'] = 'BSD-2-Clause'
        self.assertFalse(equivalent(text, changed))
        with self.assertRaises(ValueError): project_text(text + 'PackageComment: <text>unfinished')
        with self.assertRaises(ValueError): project_text(text + 'SPDXID: duplicate')

    def test_unverified_alternative_hash_cannot_resolve_a_reference(self):
        docs = fixtures.ComponentReviewTests().fixture()
        variant = {'projectionEquivalent': False, 'sha1': 'a'*40, 'fileName': 'base.spdx'}
        hashes = {'base': {'sha1': 'b'*40, 'verifiedVariants': [variant]}}
        self.assertTrue(dependency_review(docs, {('svg', 'svg.dll')}, hashes)['unresolvedReferences'])
        variant['projectionEquivalent'] = True
        result = dependency_review(docs, {('svg', 'svg.dll')}, hashes)
        self.assertFalse(result['unresolvedReferences']); self.assertEqual(result['externalDocumentChecks'][0]['matchedSerialization'], 'base.spdx')
