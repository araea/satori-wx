#!/usr/bin/env python3
"""Accurate DEX reference finder (uses dexlib instruction boundaries).
usage: dexrefs.py <base.apk> string:<substr>
       dexrefs.py <base.apk> class:<Ldesc;>
       dexrefs.py <base.apk> method:<Lclass;>:<name>
Prints "class method signature" for every referencing method.
"""
import struct
import sys
import zipfile

import dexlib

def protos(data, types):
    psize, poff = struct.unpack_from('<II', data, 0x48)
    out = []
    for i in range(psize):
        _, ret, params = struct.unpack_from('<III', data, poff + i * 12)
        args = []
        if params:
            n = struct.unpack_from('<I', data, params)[0]
            for k in range(n):
                args.append(types[struct.unpack_from('<H', data, params + 4 + k * 2)[0]])
        out.append((args, types[ret]))
    return out

def scan(data, kind, needle_class=None, needle_name=None, needle_text=None):
    strings, types, _, methods = dexlib.header(data)
    proto = protos(data, types)
    method_index = None
    if kind == 'class':
        targets = {i for i, t in enumerate(types) if t == needle_class}
    elif kind == 'string':
        targets = {i for i, s in enumerate(strings) if needle_text in s}
    else:
        method_index = {i for i, (cls, name) in enumerate(methods)
                        if cls == needle_class and name == needle_name}
        targets = set()
    if kind != 'method' and not targets:
        return []
    if kind == 'method' and not method_index:
        return []
    class_size, class_off = struct.unpack_from('<II', data, 0x60)
    msize, moff = struct.unpack_from('<II', data, 0x58)
    out = []
    for _, method_idx, code_off in dexlib.method_code(data, class_size, class_off):
        if method_idx >= len(methods):
            continue
        insns_size = struct.unpack_from('<I', data, code_off + 12)[0]
        base = code_off + 16
        hit = False
        for i, op, _ in dexlib.iter_instructions(data, code_off):
            if i + 1 >= insns_size:
                break
            operand = struct.unpack_from('<H', data, base + (i + 1) * 2)[0]
            if kind == 'string' and op == 0x1A and operand in targets:
                hit = True
            elif kind == 'class' and op in (0x1C, 0x1F, 0x20, 0x22, 0x23) and operand in targets:
                hit = True
            elif kind == 'method' and op in (0x6E, 0x6F, 0x70, 0x71, 0x72, 0x74, 0x75, 0x76, 0x77, 0x78):
                if operand in method_index:
                    hit = True
            if hit:
                break
        if hit:
            cls, name = methods[method_idx]
            cls_idx, proto_idx, _ = struct.unpack_from('<HHI', data, moff + method_idx * 8)
            args, ret = proto[proto_idx]
            out.append('%s %s(%s)%s' % (cls, name, ', '.join(args), ret))
    return out

def main():
    apk, selector = sys.argv[1], sys.argv[2]
    kind, value = selector.split(':', 1)
    needle_class = needle_name = needle_text = None
    if kind == 'string':
        needle_text = value
    elif kind == 'class':
        needle_class = value
    elif kind == 'method':
        needle_class, needle_name = value.rsplit(':', 1)
    seen = set()
    with zipfile.ZipFile(apk) as z:
        for name in sorted(n for n in z.namelist() if n.startswith('classes') and n.endswith('.dex')):
            for line in scan(z.read(name), kind, needle_class, needle_name, needle_text):
                if line not in seen:
                    seen.add(line)
                    print(line)

if __name__ == '__main__':
    main()
