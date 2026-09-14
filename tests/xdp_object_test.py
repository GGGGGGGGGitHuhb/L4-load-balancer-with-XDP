"""Check the build-only XDP_PASS contract without root, LLVM tools or libbpf."""
import pathlib
import struct
import sys


def verify(data):
    data = bytes(data)
    if len(data) < 64 or data[:5] != b'\x7fELF\x02' or data[5] not in (1, 2):
        raise ValueError('expected ELF64 object')
    endian = '<' if data[5] == 1 else '>'
    header = struct.unpack_from(endian + 'HHIQQQIHHHHHH', data, 16)
    if header[:3] != (1, 247, 1):
        raise ValueError('expected relocatable EM_BPF object')
    offset, size, count, names_index = header[5], header[10], header[11], header[12]
    if size != 64 or not count or names_index >= count or offset + size * count > len(data):
        raise ValueError('invalid section table')
    sections = [struct.unpack_from(endian + 'IIQQQQIIQQ', data, offset + i * size)
                for i in range(count)]
    def section_bytes(s):
        start, length = s[4:6]
        if start + length > len(data):
            raise ValueError('section outside file')
        return data[start:start + length]
    names = section_bytes(sections[names_index])
    by_name = {}
    for section in sections:
        start = section[0]
        end = names.find(b'\0', start)
        if start >= len(names) or end < 0:
            raise ValueError('invalid section name')
        by_name[names[start:end]] = section
    if b'xdp' not in by_name or b'license' not in by_name:
        raise ValueError('missing xdp/license section')
    xdp = by_name[b'xdp']
    if xdp[1] != 1 or not xdp[2] & 4:
        raise ValueError('xdp section is not executable PROGBITS')
    instructions = bytes.fromhex('b7000000020000009500000000000000') if endian == '<' else bytes.fromhex('b7000000000000029500000000000000')
    if section_bytes(xdp) != instructions:
        raise ValueError('expected only r0=XDP_PASS and exit')
    if section_bytes(by_name[b'license']) != b'GPL\0':
        raise ValueError('expected GPL license')
    if b'maps' in by_name or b'.maps' in by_name:
        raise ValueError('S1 must not define maps')
    return xdp[4], endian


def main():
    data = pathlib.Path(sys.argv[1]).read_bytes()
    instruction_offset, endian = verify(data)
    for name, offset, replacement in [('machine', 18, struct.pack(endian + 'H', 62)),
                                      ('action', instruction_offset + 4, struct.pack(endian + 'I', 1))]:
        bad = bytearray(data)
        bad[offset:offset + len(replacement)] = replacement
        try:
            verify(bad)
        except ValueError:
            continue
        raise ValueError(f'negative mutation {name} was accepted')
    print('PASS: ELF64 EM_BPF, XDP_PASS, GPL, no maps; machine/action mutations rejected')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, struct.error) as exc:
        print(f'FAIL: {exc}', file=sys.stderr)
        sys.exit(1)
