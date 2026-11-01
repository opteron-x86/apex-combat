#!/usr/bin/env python3
"""Validate the Windows DLL architecture, SKSE exports, and runtime imports."""
import argparse
import json
from pathlib import Path
import struct


def inspect(path):
    data = Path(path).read_bytes()
    def u16(offset): return struct.unpack_from('<H', data, offset)[0]
    def u32(offset): return struct.unpack_from('<I', data, offset)[0]
    def cstr(offset): return data[offset:data.index(b'\0', offset)].decode('ascii')
    if data[:2] != b'MZ':
        raise ValueError('Not a PE file')
    pe = u32(0x3c)
    if data[pe:pe + 4] != b'PE\0\0' or u16(pe + 4) != 0x8664:
        raise ValueError('Expected an x64 PE image')
    optional = pe + 24
    if u16(optional) != 0x20b or not u16(pe + 22) & 0x2000:
        raise ValueError('Expected a PE32+ DLL')
    sections = optional + u16(pe + 20)
    def offset(rva):
        for i in range(u16(pe + 6)):
            entry = sections + i * 40
            virtual_size, va, raw_size, raw = struct.unpack_from('<IIII', data, entry + 8)
            if va <= rva < va + max(virtual_size, raw_size):
                return raw + rva - va
        raise ValueError(f'Unmapped PE address {rva:#x}')
    export_rva = u32(optional + 112)
    if not export_rva:
        raise ValueError('DLL has no export table')
    exports_base = offset(export_rva)
    names = offset(u32(exports_base + 32))
    exports = sorted(cstr(offset(u32(names + i * 4))) for i in range(u32(exports_base + 24)))
    missing = {'SKSEPlugin_Load', 'SKSEPlugin_Version'} - set(exports)
    if missing:
        raise ValueError('Missing SKSE exports: ' + ', '.join(sorted(missing)))
    imports = []
    import_rva = u32(optional + 120)
    if import_rva:
        entry = offset(import_rva)
        while any(data[entry:entry + 20]):
            imports.append(cstr(offset(u32(entry + 12))))
            entry += 20
    debug = [name for name in imports if name.lower() in {'ucrtbased.dll', 'msvcp140d.dll', 'vcruntime140d.dll', 'vcruntime140_1d.dll'}]
    if debug:
        raise ValueError('Debug C runtime imported: ' + ', '.join(debug))
    return {'architecture': 'x64', 'exports': exports, 'imports': sorted(imports)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dll', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(inspect(args.dll), indent=2))
    except (ValueError, IndexError, struct.error) as error:
        parser.exit(1, f'DLL audit failed: {error}\n')
