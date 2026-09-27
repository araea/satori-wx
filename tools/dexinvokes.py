#!/usr/bin/env python3
"""List methods invoked by one method (accurate decoding). Offline research only.
usage: dexinvokes.py <base.apk> <Lclass;> <method>
"""
import struct
import sys
import zipfile

import dexlib

def invoked(data):
    strings, types, _, methods = dexlib.header(data)
    msize, moff = struct.unpack_from('<II', data, 0x58)
    return methods, msize, moff

def main():
    apk, wanted_cls, wanted_name = sys.argv[1], sys.argv[2], sys.argv[3]
    seen = set()
    with zipfile.ZipFile(apk) as z:
        for name in sorted(n for n in z.namelist() if n.startswith('classes') and n.endswith('.dex')):
            data = z.read(name)
            methods, msize, moff = invoked(data)
            class_size, class_off = struct.unpack_from('<II', data, 0x60)
            for _, method_idx, code_off in dexlib.method_code(data, class_size, class_off):
                if method_idx >= len(methods):
                    continue
                cls, name_ = methods[method_idx]
                if cls != wanted_cls or name_ != wanted_name:
                    continue
                insns_size = struct.unpack_from('<I', data, code_off + 12)[0]
                base = code_off + 16
                for i, op, _ in dexlib.iter_instructions(data, code_off):
                    if op in (0x6E, 0x6F, 0x70, 0x71, 0x72, 0x74, 0x75, 0x76, 0x77, 0x78) and i + 1 < insns_size:
                        callee = struct.unpack_from('<H', data, base + (i + 1) * 2)[0]
                        if callee < len(methods):
                            line = '%s %s' % methods[callee]
                            if line not in seen:
                                seen.add(line)
                                print(line)

if __name__ == '__main__':
    main()
