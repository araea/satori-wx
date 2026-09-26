#!/usr/bin/env python3
"""Extract (class descriptor, method name) pairs from classes*.dex inside an APK.

Used only for offline reverse-engineering: it does not touch the device or WeChat.
usage: dexmethods.py <base.apk> [substring-filter]
"""
import struct
import sys
import zipfile

def uleb128(data, pos):
    result = 0
    shift = 0
    while True:
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, pos
        shift += 7

def parse(data):
    string_ids_size, string_ids_off = struct.unpack_from('<II', data, 0x38)
    type_ids_size, type_ids_off = struct.unpack_from('<II', data, 0x40)
    method_ids_size, method_ids_off = struct.unpack_from('<II', data, 0x58)
    strings = []
    for i in range(string_ids_size):
        off = struct.unpack_from('<I', data, string_ids_off + i * 4)[0]
        _, p = uleb128(data, off)
        end = data.index(b'\x00', p)
        strings.append(data[p:end].decode('utf-8', 'replace'))
    types = []
    for i in range(type_ids_size):
        idx = struct.unpack_from('<I', data, type_ids_off + i * 4)[0]
        types.append(strings[idx] if idx < len(strings) else '?')
    methods = []
    for i in range(method_ids_size):
        class_idx, _, name_idx = struct.unpack_from('<HHI', data, method_ids_off + i * 8)
        cls = types[class_idx] if class_idx < len(types) else '?'
        name = strings[name_idx] if name_idx < len(strings) else '?'
        methods.append((cls, name))
    return methods

def main():
    apk = sys.argv[1]
    needle = sys.argv[2].lower() if len(sys.argv) > 2 else ''
    seen = set()
    with zipfile.ZipFile(apk) as z:
        names = [n for n in z.namelist() if n.endswith('.dex') and n.startswith('classes')]
        for name in sorted(names):
            try:
                for cls, method in parse(z.read(name)):
                    if needle and needle not in method.lower() and needle not in cls.lower():
                        continue
                    pair = (cls, method)
                    if pair in seen:
                        continue
                    seen.add(pair)
                    print('%s %s' % (cls, method))
            except Exception as error:
                print('# %s: %s' % (name, error), file=sys.stderr)

if __name__ == '__main__':
    main()
