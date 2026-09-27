#!/usr/bin/env python3
"""Find methods whose bytecode references a target method index. Offline research only.
usage: dexcalls.py <base.apk> <Lclass;> <method>
Prints caller class/method pairs. Heuristic: scans code units for the 16-bit method index.
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
    return types, methods

def callers(data, wanted_cls, wanted_method):
    types, methods = setup(data)
    targets = {i for i, (cls, name) in enumerate(methods) if cls == wanted_cls and name == wanted_method}
    if not targets:
        return []
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
            insns_size = struct.unpack_from('<I', data, code_off + 12)[0]
            start = code_off + 16
            for k in range(insns_size - 1):
                if struct.unpack_from('<H', data, start + k * 2)[0] in targets:
                    out.append(methods[method_idx])
                    break
    return out

def main():
    apk, cls, method = sys.argv[1], sys.argv[2], sys.argv[3]
    seen = set()
    with zipfile.ZipFile(apk) as z:
        for name in sorted(n for n in z.namelist() if n.startswith('classes') and n.endswith('.dex')):
            for caller in callers(z.read(name), cls, method):
                if caller not in seen:
                    seen.add(caller)
                    print('%s %s' % caller)

if __name__ == '__main__':
    main()
