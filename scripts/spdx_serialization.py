"""Compare the SPDX 2.3 fields used by the Qt review across SDK serializations.

This is a bounded projection reader for pinned supplier evidence, not a general
SPDX validator. A text checksum alone never authorizes different JSON metadata.
"""
import re

PACKAGE = {'PackageName': 'name', 'SPDXID': 'SPDXID', 'PackageVersion': 'versionInfo',
           'PackageLicenseConcluded': 'licenseConcluded', 'PackageLicenseDeclared': 'licenseDeclared',
           'PackageCopyrightText': 'copyrightText', 'PackageDownloadLocation': 'downloadLocation'}
FILE = {'FileName': 'fileName', 'SPDXID': 'SPDXID'}


def tags(text):
    lines = iter(text.splitlines())
    for line in lines:
        if not line.strip() or line.lstrip().startswith('#'): continue
        tag, separator, value = line.partition(':')
        if not separator: raise ValueError('Unsupported SPDX tag-value line')
        value = value.strip()
        if value.startswith('<text>'):
            while '</text>' not in value:
                try: value += '\n' + next(lines)
                except StopIteration: raise ValueError('Unclosed SPDX text field') from None
            if not value.endswith('</text>') or value.count('<text>') != 1 or value.count('</text>') != 1:
                raise ValueError('Ambiguous SPDX text field')
            value = value[6:-7]
        yield tag, value


def project_json(doc):
    return {'version': doc.get('spdxVersion'), 'namespace': doc.get('documentNamespace'),
            'packages': sorted([{key: item.get(key) for key in PACKAGE.values()} for item in doc.get('packages', [])], key=lambda x: x['SPDXID']),
            'files': sorted([{key: item.get(key) for key in FILE.values()} for item in doc.get('files', [])], key=lambda x: x['SPDXID']),
            'relationships': sorted({(item['spdxElementId'], item['relationshipType'], item['relatedSpdxElement']) for item in doc.get('relationships', [])}),
            'external': sorted((item['externalDocumentId'], item['spdxDocument'], item['checksum']['algorithm'].upper(), item['checksum']['checksumValue'].lower()) for item in doc.get('externalDocumentRefs', []))}


def project_text(text):
    doc = {'packages': [], 'files': [], 'relationships': [], 'externalDocumentRefs': []}; current = None; fields = None
    for tag, value in tags(text):
        if tag in ('PackageName', 'FileName'):
            fields = PACKAGE if tag == 'PackageName' else FILE; current = {}; doc['packages' if tag == 'PackageName' else 'files'].append(current)
        if tag in ('SPDXVersion', 'DocumentNamespace'):
            key = 'spdxVersion' if tag == 'SPDXVersion' else 'documentNamespace'
            if key in doc: raise ValueError('Duplicate SPDX document field')
            doc[key] = value
        elif tag == 'Relationship':
            parts = value.split()
            if len(parts) != 3: raise ValueError('Unsupported SPDX relationship')
            doc['relationships'].append(dict(zip(['spdxElementId', 'relationshipType', 'relatedSpdxElement'], parts)))
        elif tag == 'ExternalDocumentRef':
            match = re.fullmatch(r'(\S+)\s+(\S+)\s+(SHA1|SHA256):\s*([0-9a-fA-F]+)', value)
            if not match: raise ValueError('Unsupported SPDX external checksum')
            doc['externalDocumentRefs'].append({'externalDocumentId': match[1], 'spdxDocument': match[2], 'checksum': {'algorithm': match[3], 'checksumValue': match[4]}})
        elif current is not None and tag in fields:
            key = fields[tag]
            if key in current: raise ValueError('Duplicate projected SPDX element field')
            current[key] = value
    for group in ['packages', 'files']:
        ids = [item.get('SPDXID') for item in doc[group]]
        if None in ids or len(set(ids)) != len(ids): raise ValueError('Invalid SPDX element identity')
    return project_json(doc)


def equivalent(text, doc):
    return project_text(text) == project_json(doc)
