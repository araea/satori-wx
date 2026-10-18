#!/usr/bin/env python3
"""Index of a WeChat APK's dex files for the name-discovery rules (wxnames.py).

Offline research tool, same job as native/wx_discover.cpp does on the phone: classes with their super
class, interfaces, fields and methods, plus the string constants that methods load ("anchors": WeChat's
obfuscation shuffles names but not the log tags and cgi paths in its code).

The first run parses the APK (a few minutes) and caches the result next to it as <apk>.wxidx.
"""
import os
import pickle
import re
import struct
import sys
import zipfile

# Instruction width in 16-bit code units (same table as dexlib.py, indexed by opcode).
W = [1] * 256
for _op, _w in {
    0x02: 2, 0x03: 3, 0x05: 2, 0x06: 3, 0x08: 2, 0x09: 3,
    0x13: 2, 0x14: 3, 0x15: 2, 0x16: 2, 0x17: 3, 0x18: 5, 0x19: 2,
    0x1a: 2, 0x1b: 3, 0x1c: 2, 0x1f: 2, 0x20: 2, 0x22: 2, 0x23: 2,
    0x24: 3, 0x25: 3, 0x26: 3, 0x29: 2, 0x2a: 3, 0x2b: 3, 0x2c: 3,
}.items():
    W[_op] = _w
for _op in range(0x2D, 0x3E):
    W[_op] = 2
for _op in range(0x44, 0x6E):
    W[_op] = 2
for _op in range(0x6E, 0x73):
    W[_op] = 3
for _op in range(0x74, 0x79):
    W[_op] = 3
for _op in range(0x90, 0xB0):
    W[_op] = 2
for _op in range(0xD0, 0xE3):
    W[_op] = 2
W[0xFA] = W[0xFB] = 4
W[0xFC] = W[0xFD] = 3
W[0xFE] = W[0xFF] = 2

# Framework methods whose callers matter: (class descriptor, method name).
WATCH = (('Landroid/os/Looper;', 'myLooper'), ('Landroid/os/Looper;', 'getMainLooper'),
         ('Landroid/os/Looper;', 'prepare'))

# A string loaded by more classes than this is not an anchor (format strings, "", "null", ...).
MAX_CLASSES = 80


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


def payload_units(d, p):
    kind = d[p + 1]
    if kind == 1:
        return 4 + (d[p + 2] | d[p + 3] << 8) * 2
    if kind == 2:
        return 2 + (d[p + 2] | d[p + 3] << 8) * 4
    if kind == 3:
        element = d[p + 2] | d[p + 3] << 8
        size = struct.unpack_from('<I', d, p + 4)[0]
        return 4 + (size * element + 1) // 2
    return 1


def string_loads(d, code):
    """(string index, field index or None) for each string constant a code item loads.

    The field is the one an iput-object stores the constant into a few instructions later, when there is
    such a store: the way to tell which field holds "msg_raw_img_send" without trusting its name."""
    size = struct.unpack_from('<I', d, code + 12)[0]
    p = code + 16
    end = p + size * 2
    out = []
    pending = {}  # register -> [position in out, instructions left]
    while p < end:
        op = d[p]
        if pending:
            for reg in list(pending):
                pending[reg][1] -= 1
                if pending[reg][1] < 0:
                    del pending[reg]
        if op == 0x1A or op == 0x1B:
            sidx = d[p + 2] | d[p + 3] << 8 if op == 0x1A else struct.unpack_from('<I', d, p + 2)[0]
            out.append([sidx, None])
            pending[d[p + 1]] = [len(out) - 1, 6]
        elif op == 0x5B and pending:  # iput-object vA, vB, field@CCCC
            reg = d[p + 1] & 0x0F
            if reg in pending:
                out[pending[reg][0]][1] = d[p + 2] | d[p + 3] << 8
                del pending[reg]
        elif op == 0 and d[p + 1]:
            p += payload_units(d, p) * 2
            continue
        p += W[op] * 2
    return out


def invokes(d, code, watched):
    """Method indexes (from `watched`) that a code item invokes."""
    size = struct.unpack_from('<I', d, code + 12)[0]
    p = code + 16
    end = p + size * 2
    out = []
    while p < end:
        op = d[p]
        if (0x6E <= op <= 0x72 or 0x74 <= op <= 0x78) and (d[p + 2] | d[p + 3] << 8) in watched:
            out.append(d[p + 2] | d[p + 3] << 8)
        elif op == 0 and d[p + 1]:
            p += payload_units(d, p) * 2
            continue
        p += W[op] * 2
    return out


def parse_dex(d, users, calls):
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
    watched = {i: m for i, m in enumerate(methods) if (m[0], m[1]) in WATCH}
    classes = {}
    for i in range(csz):
        base = coff + 32 * i
        ci, acc, sup, ifo, _, _, cdo, _ = struct.unpack_from('<IIIIIIII', d, base)
        name = types[ci]
        ifaces = []
        if ifo:
            n = struct.unpack_from('<I', d, ifo)[0]
            for k in range(n):
                ifaces.append(types[struct.unpack_from('<H', d, ifo + 4 + 2 * k)[0]])
        info = {'acc': acc, 'super': types[sup] if sup != 0xFFFFFFFF else None, 'ifaces': ifaces,
                'fields': [], 'methods': []}
        if cdo:
            p = cdo
            ns, p = uleb(d, p)
            ni, p = uleb(d, p)
            nd, p = uleb(d, p)
            nv, p = uleb(d, p)
            for count in (ns, ni):
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
                    code, p = uleb(d, p)
                    idx += diff
                    m = methods[idx]
                    info['methods'].append((m[1], m[2], flags))
                    if code:
                        if watched:
                            for target in invokes(d, code, watched):
                                calls.setdefault(watched[target][1], {}).setdefault(name, []).append((m[1], m[2]))
                        for s, fidx in string_loads(d, code):
                            users.setdefault(strings[s], {}).setdefault(name, []).append(
                                (m[1], m[2], fields[fidx][2] if fidx is not None else None))
        classes[name] = info
    return classes


def build(apk):
    classes = {}
    users = {}
    calls = {}
    with zipfile.ZipFile(apk) as z:
        for name in sorted(n for n in z.namelist() if re.fullmatch(r'classes\d*\.dex', n)):
            sys.stderr.write('parsing %s\n' % name)
            part = {}
            for cname, info in parse_dex(z.read(name), part, calls).items():
                classes[cname] = info
            for s, who in part.items():
                merged = users.setdefault(s, {})
                for c, ms in who.items():
                    merged.setdefault(c, []).extend(ms)
    # Keep the strings that identify something: loaded by few classes and long enough to mean it.
    # anchors[string] = {class descriptor: [(method name, method proto), ...]}
    anchors = {s: who for s, who in users.items() if len(who) <= MAX_CLASSES and len(s) >= 5}
    return {'classes': classes, 'anchors': anchors, 'calls': calls}


def load(apk):
    cache = apk + '.wxidx'
    if os.path.exists(cache) and os.path.getmtime(cache) >= os.path.getmtime(apk):
        with open(cache, 'rb') as f:
            return pickle.load(f)
    index = build(apk)
    with open(cache, 'wb') as f:
        pickle.dump(index, f, protocol=pickle.HIGHEST_PROTOCOL)
    return index


if __name__ == '__main__':
    idx = load(sys.argv[1])
    print(len(idx['classes']), 'classes,', len(idx['anchors']), 'anchor strings')
