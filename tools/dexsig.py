#!/usr/bin/env python3
"""Signature index over every classes*.dex in an APK, for matching obfuscated WeChat names across builds.

Offline research only; touches neither the device nor WeChat. The first run parses the APK (about a
minute for WeChat) and caches the index next to it as <apk>.dexsig; later runs load the cache.

usage:
  dexsig.py <apk> proto  '<descriptor>' [name]     methods with exactly this prototype, e.g. '(Ljava/lang/String;J)Lcom/tencent/mm/storage/e9;'
  dexsig.py <apk> shape  '<descriptor>' [name]     same, but L*; in the descriptor matches any class type (framework types stay literal)
  dexsig.py <apk> class  <Lpkg/Name;>              super, interfaces, fields and methods of one class
  dexsig.py <apk> sub    <Lpkg/Name;>              classes whose direct super class or interface is this one
  dexsig.py <apk> field  <type> [name]             fields of this type (exact descriptor), optionally with this name
  dexsig.py <apk> grep   <regex>                   classes whose descriptor matches
Method lines read: <class> <name><proto> [flags]. Flags: s static, f final, a abstract.
"""
import os
import pickle
import re
import struct
import sys
import zipfile


def uleb(d, p):
    r = 0
    s = 0
    while True:
        b = d[p]
        p += 1
        r |= (b & 0x7F) << s
        if not b & 0x80:
            return r, p
        s += 7


def parse_dex(d):
    ssz, soff = struct.unpack_from('<II', d, 0x38)
    tsz, toff = struct.unpack_from('<II', d, 0x40)
    psz, poff = struct.unpack_from('<II', d, 0x48)
    fsz, foff = struct.unpack_from('<II', d, 0x50)
    msz, moff = struct.unpack_from('<II', d, 0x58)
    csz, coff = struct.unpack_from('<II', d, 0x60)
    strings = []
    for i in range(ssz):
        p = struct.unpack_from('<I', d, soff + 4 * i)[0]
        _, q = uleb(d, p)
        strings.append(d[q:d.index(b'\0', q)].decode('utf-8', 'replace'))
    types = [strings[struct.unpack_from('<I', d, toff + 4 * i)[0]] for i in range(tsz)]
    protos = []
    for i in range(psz):
        _, ret, po = struct.unpack_from('<III', d, poff + 12 * i)
        args = []
        if po:
            n = struct.unpack_from('<I', d, po)[0]
            for k in range(n):
                args.append(types[struct.unpack_from('<H', d, po + 4 + 2 * k)[0]])
        protos.append('(' + ''.join(args) + ')' + types[ret])
    fields = []
    for i in range(fsz):
        c, t, n = struct.unpack_from('<HHI', d, foff + 8 * i)
        fields.append((types[c], types[t], strings[n]))
    methods = []
    for i in range(msz):
        c, pr, n = struct.unpack_from('<HHI', d, moff + 8 * i)
        methods.append((types[c], strings[n], protos[pr]))
    classes = {}
    for i in range(csz):
        base = coff + 32 * i
        ci, acc, sup, ifo, _, _, cdo, _ = struct.unpack_from('<IIIIIIII', d, base)
        name = types[ci]
        supname = types[sup] if sup != 0xFFFFFFFF else None
        ifaces = []
        if ifo:
            n = struct.unpack_from('<I', d, ifo)[0]
            for k in range(n):
                ifaces.append(types[struct.unpack_from('<H', d, ifo + 4 + 2 * k)[0]])
        info = {'acc': acc, 'super': supname, 'ifaces': ifaces, 'fields': [], 'methods': []}
        if cdo:
            p = cdo
            ns, p = uleb(d, p)
            ni, p = uleb(d, p)
            nd, p = uleb(d, p)
            nv, p = uleb(d, p)
            idx = 0
            for count, static in ((ns, True), (ni, False)):
                idx = 0
                for _ in range(count):
                    diff, p = uleb(d, p)
                    flags, p = uleb(d, p)
                    idx += diff
                    f = fields[idx]
                    info['fields'].append((f[2], f[1], flags))
            for count in (nd, nv):
                idx = 0
                for _ in range(count):
                    diff, p = uleb(d, p)
                    flags, p = uleb(d, p)
                    _code, p = uleb(d, p)
                    idx += diff
                    m = methods[idx]
                    info['methods'].append((m[1], m[2], flags))
        classes[name] = info
    return classes


def load(apk):
    cache = apk + '.dexsig'
    if os.path.exists(cache) and os.path.getmtime(cache) >= os.path.getmtime(apk):
        with open(cache, 'rb') as f:
            return pickle.load(f)
    classes = {}
    with zipfile.ZipFile(apk) as z:
        for name in sorted(n for n in z.namelist() if re.fullmatch(r'classes\d*\.dex', n)):
            sys.stderr.write('parsing %s\n' % name)
            for cname, info in parse_dex(z.read(name)).items():
                info['dex'] = name
                classes[cname] = info
    with open(cache, 'wb') as f:
        pickle.dump(classes, f, protocol=pickle.HIGHEST_PROTOCOL)
    return classes


def flags(f):
    out = ''
    if f & 0x8:
        out += 's'
    if f & 0x10:
        out += 'f'
    if f & 0x400:
        out += 'a'
    return out


def shape_regex(desc):
    # Class types become wildcards; primitives and arrays of primitives stay literal.
    out = ''
    i = 0
    while i < len(desc):
        if desc.startswith('L*;', i):
            out += r'L[^;]*;'
            i += 3
        else:
            out += re.escape(desc[i])
            i += 1
    return re.compile('^' + out + '$')


def main():
    apk, cmd = sys.argv[1], sys.argv[2]
    classes = load(apk)
    arg = sys.argv[3] if len(sys.argv) > 3 else ''
    extra = sys.argv[4] if len(sys.argv) > 4 else None
    if cmd in ('proto', 'shape'):
        match = (lambda p: p == arg) if cmd == 'proto' else (lambda p, r=shape_regex(arg): r.match(p))
        for cname, info in classes.items():
            for n, p, f in info['methods']:
                if match(p) and (extra is None or n == extra):
                    print('%s %s%s %s' % (cname, n, p, flags(f)))
    elif cmd == 'class':
        info = classes.get(arg)
        if not info:
            print('no such class')
            return
        print('%s  dex=%s super=%s ifaces=%s' % (arg, info.get('dex'), info['super'], info['ifaces']))
        for n, t, f in info['fields']:
            print('  F %s %s %s' % (n, t, flags(f)))
        for n, p, f in info['methods']:
            print('  M %s%s %s' % (n, p, flags(f)))
    elif cmd == 'sub':
        for cname, info in classes.items():
            if info['super'] == arg or arg in info['ifaces']:
                print(cname)
    elif cmd == 'field':
        for cname, info in classes.items():
            for n, t, f in info['fields']:
                if t == arg and (extra is None or n == extra):
                    print('%s %s %s %s' % (cname, n, t, flags(f)))
    elif cmd == 'grep':
        rx = re.compile(arg)
        for cname in classes:
            if rx.search(cname):
                print(cname)
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
