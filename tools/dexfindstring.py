#!/usr/bin/env python3
"""Find (class, method) that reference a string constant, by scanning DEX bytecode.

Offline research only; does not touch the device.
usage: dexfindstring.py <base.apk> <substring>
"""
import struct
import sys
import zipfile

def uleb(data, pos):
    result = 0
    shift = 0
    while True:
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, pos
        shift += 7

def parse_strings(data):
    size, off = struct.unpack_from('<II', data, 0x38)
    out = []
    for i in range(size):
        p = struct.unpack_from('<I', data, off + i * 4)[0]
        _, q = uleb(data, p)
        end = data.index(b'\x00', q)
        out.append(data[q:end].decode('utf-8', 'replace'))
    return out

def scan(data, needle):
    strings = parse_strings(data)
    targets = {i for i, s in enumerate(strings) if needle in s}
    if not targets:
        return []
    type_size, type_off = struct.unpack_from('<II', data, 0x40)
    method_size, method_off = struct.unpack_from('<II', data, 0x58)
    class_size, class_off = struct.unpack_from('<II', data, 0x60)
    types = [strings[struct.unpack_from('<I', data, type_off + i * 4)[0]] for i in range(type_size)]
    methods = []
    for i in range(method_size):
        cls, _, name = struct.unpack_from('<HHI', data, method_off + i * 8)
        methods.append((types[cls], strings[name]))
    hits = []
    for i in range(class_size):
        base = class_off + i * 32
        class_idx = struct.unpack_from('<I', data, base)[0]
        class_data_off = struct.unpack_from('<I', data, base + 24)[0]
        if not class_data_off:
            continue
        pos = class_data_off
        static_size, pos = uleb(data, pos)
        instance_size, pos = uleb(data, pos)
        direct_size, pos = uleb(data, pos)
        virtual_size, pos = uleb(data, pos)
        for _ in range(static_size + instance_size):
            _, pos = uleb(data, pos)
            _, pos = uleb(data, pos)
        method_idx = 0
        for _ in range(direct_size + virtual_size):
            diff, pos = uleb(data, pos)
            method_idx += diff
            _, pos = uleb(data, pos)
            code_off, pos = uleb(data, pos)
            if not code_off:
                continue
            insns_size = struct.unpack_from('<I', data, code_off + 12)[0]
            start = code_off + 16
            found = False
            for k in range(insns_size - 1):
                unit = struct.unpack_from('<H', data, start + k * 2)[0]
                if (unit & 0xFF) == 0x1A:
                    idx = struct.unpack_from('<H', data, start + (k + 1) * 2)[0]
                    if idx in targets:
                        found = True
                        break
            if found and method_idx < len(methods):
                hits.append(methods[method_idx])
    return hits

def main():
    apk, needle = sys.argv[1], sys.argv[2]
    seen = set()
    with zipfile.ZipFile(apk) as z:
        for name in sorted(n for n in z.namelist() if n.startswith('classes') and n.endswith('.dex')):
            for cls, method in scan(z.read(name), needle):
                if (cls, method) not in seen:
                    seen.add((cls, method))
                    print('%s %s' % (cls, method))

if __name__ == '__main__':
    main()
