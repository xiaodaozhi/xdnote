"""Compare executable sections before/after deployment, without running code."""
import hashlib
import struct


def code_sections(data):
    def unpack(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ValueError('Truncated native binary')
        return struct.unpack_from(fmt, data, offset)
    def section(start, size):
        if start < 0 or start + size > len(data):
            raise ValueError('Native section outside file')
        return hashlib.sha256(data[start:start + size]).hexdigest()
    result = {}
    if data[:4] == b'\x7fELF':
        if data[4:6] != b'\x02\x01': raise ValueError('Expected little-endian ELF64')
        offset, = unpack('<Q', 40); width, count = unpack('<HH', 58)
        if width < 64 or count > 65535: raise ValueError('Invalid ELF section table')
        for index in range(count):
            row = unpack('<IIQQQQIIQQ', offset + index * width)
            if row[1] == 1 and row[2] & 4:
                result['elf:' + str(index)] = section(row[4], row[5])
    elif data[:4] in (b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
        endian = '>' if data[:4] == b'\xca\xfe\xba\xbe' else '<'
        count, = unpack(endian + 'I', 4)
        if count > 32: raise ValueError('Excessive Mach-O architectures')
        for index in range(count):
            cpu, subtype, start, size, _ = unpack(endian + 'IIIII', 8 + index * 20)
            if start + size > len(data): raise ValueError('Mach-O slice outside file')
            for name, digest in code_sections(data[start:start + size]).items():
                result[str(cpu) + ':' + str(subtype) + ':' + name] = digest
    elif data[:4] == b'\xcf\xfa\xed\xfe':
        cpu, subtype, _, count, command_bytes = unpack('<IIIII', 4)
        position = 32; end = position + command_bytes
        if end > len(data) or count > 65535: raise ValueError('Invalid Mach-O commands')
        for _ in range(count):
            command, size = unpack('<II', position)
            if size < 8 or position + size > end: raise ValueError('Invalid Mach-O command')
            if command == 0x19:
                sections, = unpack('<I', position + 64)
                if 72 + sections * 80 > size: raise ValueError('Invalid Mach-O sections')
                for index in range(sections):
                    row = unpack('<16s16sQQIIIIIIII', position + 72 + index * 80)
                    if row[0].rstrip(b'\0') == b'__text' or row[8] & 0x80000400:
                        name = row[1].rstrip(b'\0').decode('ascii') + '/' + row[0].rstrip(b'\0').decode('ascii')
                        result[str(cpu) + ':' + str(subtype) + ':' + name] = section(row[4], row[3])
            position += size
    else:
        raise ValueError('Unsupported native evidence format')
    if not result: raise ValueError('No executable sections found')
    return result
