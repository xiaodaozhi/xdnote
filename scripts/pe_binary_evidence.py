"""Read file-backed PE64 code and x64 exception ranges without loading the image."""
import hashlib
import struct
from typing import NamedTuple


class Section(NamedTuple):
    name: str
    address: int
    virtual_size: int
    size: int
    offset: int
    flags: int

    def contains(self, start, end):
        return self.address <= start < end <= self.address + min(self.size, self.virtual_size)


def layout(data):
    def read(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ValueError('Truncated PE code evidence')
        return struct.unpack_from(fmt, data, offset)

    if data[:2] != b'MZ':
        raise ValueError('Not a PE image')
    header, = read('<I', 0x3c)
    if data[header:header + 4] != b'PE\0\0' or read('<H', header + 4)[0] != 0x8664:
        raise ValueError('PE64 x86_64 required')
    count, = read('<H', header + 6)
    optional_size, = read('<H', header + 20)
    optional = header + 24
    if optional_size < 112 or read('<H', optional)[0] != 0x20b:
        raise ValueError('PE64 optional header required')
    base, = read('<Q', optional + 24)
    directories, = read('<I', optional + 108)
    if 112 + directories * 8 > optional_size:
        raise ValueError('PE directories escape optional header')
    exception = read('<II', optional + 112 + 3 * 8) if directories > 3 else (0, 0)
    sections = []
    for index in range(count):
        offset = optional + optional_size + index * 40
        name, virtual_size, address, size, start, _, _, _, _, flags = read('<8sIIIIIIHHI', offset)
        if size and start + size > len(data):
            raise ValueError('PE section escapes image')
        sections.append(Section(name.rstrip(b'\0').decode('ascii'), address, virtual_size, size, start, flags))
    return base, sections, exception


def pe_code_sections(data):
    _, sections, _ = layout(data)
    result = {f'{index}:{sec.name}': {'bytes': sec.size, 'sha256': hashlib.sha256(data[sec.offset:sec.offset + sec.size]).hexdigest()}
              for index, sec in enumerate(sections) if sec.flags & 0x20000000 and sec.size}
    if not result:
        raise ValueError('No PE executable sections')
    return result


def function_ranges(data, sections, exception):
    address, size = exception
    container = next((sec for sec in sections if sec.contains(address, address + size)), None)
    if container is None or size % 12:
        raise ValueError('Missing or malformed PE exception directory')
    offset = container.offset + address - container.address
    code = [sec for sec in sections if sec.flags & 0x20000000]
    result = {}
    for start, end, unwind in struct.iter_unpack('<III', data[offset:offset + size]):
        if not (start or end or unwind):
            continue
        if not any(sec.contains(start, end) for sec in code) or start in result:
            raise ValueError('Invalid or ambiguous PE function range')
        result[start] = end
    return result
