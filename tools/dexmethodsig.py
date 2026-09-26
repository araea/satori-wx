#!/usr/bin/env python3
"""Dump method signatures for one class from classes*.dex. Offline research only.
usage: dexmethodsig.py <base.apk> <Lclass/descriptor;>
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
    psize, poff = struct.unpack_from('<II', data, 0x48)
    protos = []
    for i in range(psize):
        shorty, ret, params = struct.unpack_from('<III', data, poff + i * 12)
        args = []
        if params:
            n = struct.unpack_from('<I', data, params)[0]
            for k in range(n):
                args.append(types[struct.unpack_from('<H', data, params + 4 + k * 2)[0]])
        protos.append((args, types[ret]))
    msize, moff = struct.unpack_from('<II', data, 0x58)
    return strings, types, protos, msize, moff

def main():
    apk, wanted = sys.argv[1], sys.argv[2]
    seen = set()
    with zipfile.ZipFile(apk) as z:
        for name in sorted(n for n in z.namelist() if n.startswith('classes') and n.endswith('.dex')):
            data = z.read(name)
            strings, types, protos, msize, moff = setup(data)
            for i in range(msize):
                cls, proto, name_idx = struct.unpack_from('<HHI', data, moff + i * 8)
                if types[cls] != wanted:
                    continue
                args, ret = protos[proto]
                sig = '%s(%s)%s' % (strings[name_idx], ', '.join(args), ret)
                if sig in seen:
                    continue
                seen.add(sig)
                print(sig)

if __name__ == '__main__':
    main()
