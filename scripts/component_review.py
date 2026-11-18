"""Trace supplier-declared Qt dependencies from an actual delivery inventory.

This preserves raw SPDX choices and separates unreachable metadata, unresolved
external references and Rust compilation units. It never decides compliance.
"""
import argparse
from collections import Counter, deque
import hashlib
import json
from pathlib import Path, PurePosixPath
import desktop
from installer_common import build_output, digest
from spdx_serialization import equivalent


def read_json(path):
    if path.stat().st_size > 64 * 1024**2:
        raise ValueError('Evidence exceeds bound')
    return json.loads(path.read_bytes())


def documents(directory, records):
    result = {}
    for record in records:
        name = record['sbom']
        if Path(name).name != name or not name.endswith('.spdx.json') or name in result:
            raise ValueError('Invalid or duplicate supplier document')
        path = directory / name
        if path.is_symlink() or not path.resolve().is_relative_to(directory.resolve()):
            raise ValueError('Supplier document must stay in the SDK SBOM directory')
        if digest(path) != record['sha256']:
            raise ValueError('Supplier document differs from actual product evidence')
        value = read_json(path)
        if value.get('spdxVersion') != 'SPDX-2.3':
            raise ValueError('Only recorded SPDX 2.3 documents are supported')
        result[name] = value
    if not result:
        raise ValueError('No actual Qt module records')
    return result


def dependency_review(docs, seeds, raw_hashes):
    packages = {}; files = {}; namespaces = {}; unresolved = []; reference_checks = []
    for name, doc in docs.items():
        namespace = doc['documentNamespace']
        if namespace in namespaces: raise ValueError('Ambiguous SPDX document namespace')
        namespaces[namespace] = name
        for source, target in [('packages', packages), ('files', files)]:
            for item in doc.get(source, []):
                key = (name, item['SPDXID'])
                if key in target: raise ValueError('Duplicate SPDX element')
                target[key] = item
    external = {}
    for name, doc in docs.items():
        for ref in doc.get('externalDocumentRefs', []):
            target = namespaces.get(ref['spdxDocument']); checksum = ref.get('checksum', {})
            algorithm = checksum.get('algorithm', '').lower(); expected = checksum.get('checksumValue', '').lower()
            actual = raw_hashes.get(target, {}).get(algorithm)
            valid = bool(target and actual and actual == expected)
            serialization = 'json' if valid else None
            if target and not valid:
                for variant in raw_hashes.get(target, {}).get('verifiedVariants', []):
                    if variant.get('projectionEquivalent') and variant.get(algorithm) == expected:
                        actual = variant[algorithm]; valid = True; serialization = variant['fileName']; break
            external[(name, ref['externalDocumentId'])] = target if valid else None
            reference_checks.append({'document': name, 'reference': ref['externalDocumentId'], 'namespace': ref['spdxDocument'],
                                     'expected': checksum, 'actual': actual, 'matches': valid, 'matchedSerialization': serialization})
    def resolve(name, identifier):
        if ':' in identifier:
            alias, identifier = identifier.split(':', 1); name = external.get((name, alias))
        return (name, identifier) if name else None
    edges = {}; owners = {}; missing_edges = {}
    for name, doc in docs.items():
        for relation in doc.get('relationships', []):
            left = resolve(name, relation['spdxElementId']); right = resolve(name, relation['relatedSpdxElement'])
            kind = relation['relationshipType']
            if kind == 'CONTAINS' and left in packages and right in files:
                owners.setdefault(right, set()).add(left)
            if kind in {'DEPENDS_ON', 'STATIC_LINK', 'DYNAMIC_LINK'} and left in packages:
                if right in packages: edges.setdefault(left, set()).add(right)
                else: missing_edges.setdefault(left, []).append({'document': name, 'source': relation['spdxElementId'], 'target': relation['relatedSpdxElement'], 'relationship': kind, 'reason': 'unresolved package dependency or mismatched external document checksum'})
    selected = {}; queue = deque()
    for seed in seeds:
        if seed not in files or not owners.get(seed): raise ValueError('Actual Qt file has no supplier package owner')
        for owner in sorted(owners[seed]):
            if owner not in selected: selected[owner] = {'seedFile': list(seed)}; queue.append(owner)
    while queue:
        parent = queue.popleft()
        unresolved.extend(missing_edges.get(parent, ()))
        for target in sorted(edges.get(parent, ())):
            if target not in selected:
                selected[target] = {'dependencyOf': list(parent)}; queue.append(target)
    rows = []
    for key in sorted(selected):
        item = packages[key]
        fields = {field: item.get(field, 'NOASSERTION') for field in ['name', 'versionInfo', 'licenseConcluded', 'licenseDeclared', 'copyrightText', 'downloadLocation']}
        rows.append({'document': key[0], 'spdxId': key[1], **fields, 'evidence': selected[key],
                     'unresolvedFields': [field for field in ['licenseConcluded', 'licenseDeclared', 'copyrightText'] if item.get(field) in (None, '', 'NOASSERTION')]})
    return {'supplierDependencyPackages': rows, 'unresolvedReferences': unresolved, 'externalDocumentChecks': reference_checks,
            'notReachedPackageCount': len(packages) - len(selected),
            'notReachedMeaning': 'not reached by supplied supplier dependency edges; does not prove absence from binary bytes',
            'inference': 'package owners of actual delivered Qt files plus transitive supplier DEPENDS_ON/STATIC_LINK/DYNAMIC_LINK; compiler BUILD_DEPENDENCY_OF and whole-module CONTAINS are not roots; generic DEPENDS_ON can cover build/run requirements'}


def network_derivation(evidence):
    receipt = evidence['buildIdentity'].get('qtNetworkBackports')
    public = evidence.get('qtSourceAccess', {}).get('networkBackports')
    if receipt is None and public is None:
        return None
    if not isinstance(receipt, dict) or not isinstance(public, dict):
        raise ValueError('Incomplete Qt Network derivation')
    from qt_network_backports import module_paths
    from qt_source_access import config as source_access_config
    qtbase = next(module for module in source_access_config()['modules'] if module['module'] == 'qtbase')
    if set(receipt['files']) != set(module_paths(receipt['system'])) or receipt.get('source') != qtbase:
        raise ValueError('Unexpected Qt Network derivation scope')
    private_fields = {'architecture', 'coreConfiguration', 'originalSignature', 'system', 'files'}
    expected = {name: value for name, value in receipt.items() if name not in private_fields}
    expected['files'] = {
        path: {name: row[name] for name in ('sha256', 'originalSdkSha256')}
        for path, row in receipt['files'].items()
    }
    if public != expected or public.get('sourceCodeInRelease') is not False:
        raise ValueError('Qt Network derivation differs across source access and build evidence')
    return public


def verify_qt_file(qt, path, actual, derivation, backport):
    source = (qt / path).resolve()
    if not source.is_relative_to(qt.resolve()):
        raise ValueError('Supplier path escapes SDK')
    sdk_sha = digest(source)
    row = derivation['files'].get(path) if derivation else None
    if row is None:
        if backport is not None or sdk_sha != actual:
            raise ValueError('Actual Qt file differs from the supplier SDK without a source derivation')
        return None
    if backport != derivation or actual != row['sha256'] or sdk_sha != row['originalSdkSha256']:
        raise ValueError('Derived Qt file lacks matching build and supplier evidence')
    return row


def review(evidence, linkage, qt):
    if evidence.get('ok') is not True or linkage.get('ok') is not True or evidence['buildIdentity'] != linkage['buildIdentity']:
        raise ValueError('Actual inventory and linker evidence must have the same successful build identity')
    if evidence['buildIdentity']['dependencies']['qt']['version'] != '6.8.2': raise ValueError('Unexpected Qt version')
    records = evidence.get('qtModules', evidence.get('qtSbom', [])); docs = documents(qt / 'sbom', records)
    raw_hashes = {name: {algorithm: hashlib.new(algorithm, (qt / 'sbom' / name).read_bytes()).hexdigest() for algorithm in ['sha1', 'sha256']} for name in docs}
    for name, doc in docs.items():
        variant = qt / 'sbom' / name.removesuffix('.json')
        if not variant.is_symlink() and variant.is_file() and variant.resolve().is_relative_to((qt / 'sbom').resolve()) and variant.stat().st_size <= 64 * 1024**2:
            data = variant.read_bytes()
            try: same = equivalent(data.decode('utf-8'), doc)
            except ValueError: same = False
            raw_hashes[name]['verifiedVariants'] = [{'fileName': variant.name, 'projectionEquivalent': same,
                                                   **{algorithm: hashlib.new(algorithm, data).hexdigest() for algorithm in ['sha1', 'sha256']}}]
    shipped = {row['path']: row for row in evidence['files']}; seeds = set()
    derivation = network_derivation(evidence)
    derived_files = {}
    for row in shipped.values():
        material = row.get('evidence', {}); supplier = material.get('supplier')
        if supplier:
            path = material['sdkFile']
            row = verify_qt_file(qt, path, material['sdkSha256'], derivation, material.get('sourceBackport'))
            if row is not None:
                if material.get('nativeSectionsMatchSdk') is not True:
                    raise ValueError('Relocated Qt derivative lacks verified code sections')
                derived_files[path] = row
            seeds.add((supplier['sbom'], supplier['spdxId']))
    if not seeds:  # Windows inventory retains exact, unrelocated Qt DLL bytes.
        for name, doc in docs.items():
            for item in doc.get('files', []):
                relative = PurePosixPath(item.get('fileName', '').removeprefix('./'))
                if relative.is_absolute() or '..' in relative.parts: raise ValueError('Supplier path escapes SDK')
                if not relative.parts or relative.parts[0] not in ('bin', 'plugins'): continue
                target = relative.relative_to(relative.parts[0]).as_posix()
                if target not in shipped or not target.lower().endswith('.dll'): continue
                actual = shipped[target]
                path = relative.as_posix()
                row = verify_qt_file(qt, path, actual['sha256'], derivation, actual.get('evidence', {}).get('sourceBackport'))
                if row is not None:
                    derived_files[path] = row
                seeds.add((name, item['SPDXID']))
    if not seeds: raise ValueError('No actual Qt file roots')
    result = dependency_review(docs, seeds, raw_hashes)
    return {'ok': True, 'buildIdentity': evidence['buildIdentity'], 'supplierDocuments': records, 'actualQtFileRoots': sorted(seeds), **result,
            'supplierSerializationEvidence': raw_hashes,
            'actualQtSourceBackports': derived_files,
            'rustSelectedCompilationUnits': linkage['members'], 'rustSummary': dict(Counter(row['kind'] for row in linkage['members'])),
            'licenseReviewComplete': False, 'releaseReady': False,
            'pending': ['review actual compiled-in third-party source and applicable license choices', 'resolve supplier reference gaps without rewriting upstream evidence',
                        'review full corresponding-source/replacement fulfillment for all shipped modules', 'bind final declarations and acceptance to actual formal artifacts'],
            'scope': 'supplier declaration trace and selected compilation units; not optimized-byte attribution or a legal/release decision'}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['evidence', 'linkage', 'qt-prefix', 'output']: p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args(); output = build_output(a.output)
    if output.exists(): p.error('Preserve existing review evidence')
    result = review(read_json(a.evidence), read_json(a.linkage), desktop.checked_tool_path(a.qt_prefix, 'Qt SDK', sdk=True))
    result['inputs'] = {name: {'sha256': digest(path)} for name, path in [('evidence', a.evidence), ('linkage', a.linkage)]}
    output.parent.mkdir(parents=True, exist_ok=True); output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'ok': True, 'supplierPackages': len(result['supplierDependencyPackages']), 'referenceGaps': len(result['unresolvedReferences']), 'releaseReady': False}))
