#!/usr/bin/env python3
"""Print string constants referenced by one method. Offline research only.
usage: dexmethodstrings.py <base.apk> <Lclass;> <method>
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

def setup(data):
    ssize, soff = struct.unpack_from('<II', data, 0x38)
    strings = []
    for i in range(ssize):
        p = struct.unpack_from('<I', data, soff + i * 4)[0]
        _, q = uleb(data, p)
        strings.append(data[q:data.index(b'\x00', q)].decode('utf-8', 'replace'))
    tsize, toff = struct.unpack_from('<II', data, 0x40)
    types = [strings[struct.unpack_from('<I', data, toff + i * 4)[0]] for i in range(tsize)]
    msize, moff = struct.unpack_from('<II', data, 0x58)
    methods = []
    for i in range(msize):
        cls, _, name = struct.unpack_from('<HHI', data, moff + i * 8)
        methods.append((types[cls], strings[name]))
    return strings, methods

def scan(data, wanted_cls, wanted_method):
    strings, methods = setup(data)
    class_size, class_off = struct.unpack_from('<II', data, 0x60)
    out = []
    for i in range(class_size):
        base = class_off + i * 32
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
            if not code_off or method_idx >= len(methods):
                continue
            cls, name = methods[method_idx]
            if cls != wanted_cls or name != wanted_method:
                continue
            insns_size = struct.unpack_from('<I', data, code_off + 12)[0]
            start = code_off + 16
            for k in range(insns_size - 1):
                unit = struct.unpack_from('<H', data, start + k * 2)[0]
                if (unit & 0xFF) == 0x1A:
                    idx = struct.unpack_from('<H', data, start + (k + 1) * 2)[0]
                    if idx < len(strings):
                        out.append(strings[idx])
    return out

def main():
    apk, cls, method = sys.argv[1], sys.argv[2], sys.argv[3]
    seen = set()
    with zipfile.ZipFile(apk) as z:
        for name in sorted(n for n in z.namelist() if n.startswith('classes') and n.endswith('.dex')):
            for text in scan(z.read(name), cls, method):
                if text not in seen:
                    seen.add(text)
                    print(repr(text))

if __name__ == '__main__':
    main()
