#!/usr/bin/env python3
"""Minimal DEX helpers: accurate instruction walking and reference extraction.
Offline research only; used by the other dex* tools."""

import struct

# Instruction width in 16-bit code units, indexed by opcode (0x00-0xff).
_W = [1] * 256
for op, width in {
    0x02: 2, 0x03: 3, 0x05: 2, 0x06: 3, 0x08: 2, 0x09: 3,
    0x13: 2, 0x14: 3, 0x15: 2, 0x16: 2, 0x17: 3, 0x18: 5, 0x19: 2,
    0x1a: 2, 0x1b: 3, 0x1c: 2, 0x1f: 2, 0x20: 2, 0x22: 2, 0x23: 2,
    0x24: 3, 0x25: 3, 0x26: 3, 0x29: 2, 0x2a: 3, 0x2b: 3, 0x2c: 3,
}.items():
    _W[op] = width
for op in range(0x2D, 0x32): _W[op] = 2
for op in range(0x32, 0x3E): _W[op] = 2
for op in range(0x44, 0x52): _W[op] = 2
for op in range(0x52, 0x6E): _W[op] = 2
for op in range(0x6E, 0x73): _W[op] = 3
for op in range(0x74, 0x79): _W[op] = 3
for op in range(0x90, 0xB0): _W[op] = 2
for op in range(0xD0, 0xD8): _W[op] = 2
for op in range(0xD8, 0xE3): _W[op] = 2
_W[0xFA] = 4
_W[0xFB] = 4
_W[0xFC] = 3
_W[0xFD] = 3
_W[0xFE] = 2
_W[0xFF] = 2

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

def header(data):
    ssize, soff = struct.unpack_from('<II', data, 0x38)
    strings = []
    for i in range(ssize):
        p = struct.unpack_from('<I', data, soff + i * 4)[0]
        _, q = uleb(data, p)
        strings.append(data[q:data.index(b'\x00', q)].decode('utf-8', 'replace'))
    tsize, toff = struct.unpack_from('<II', data, 0x40)
    types = [strings[struct.unpack_from('<I', data, toff + i * 4)[0]] for i in range(tsize)]
    fsize, foff = struct.unpack_from('<II', data, 0x50)
    fields = []
    for i in range(fsize):
        cls, typ, nm = struct.unpack_from('<HHI', data, foff + i * 8)
        fields.append((types[cls], strings[nm]))
    msize, moff = struct.unpack_from('<II', data, 0x58)
    methods = []
    for i in range(msize):
        cls, _, nm = struct.unpack_from('<HHI', data, moff + i * 8)
        methods.append((types[cls], strings[nm]))
    return strings, types, fields, methods

def payload_width(data, base, i, unit):
    kind = (unit >> 8) & 0xFF
    if kind == 0x01:  # packed-switch
        size = struct.unpack_from('<H', data, base + (i + 1) * 2)[0]
        return 4 + size * 2
    if kind == 0x02:  # sparse-switch
        size = struct.unpack_from('<H', data, base + (i + 1) * 2)[0]
        return 2 + size * 4
    if kind == 0x03:  # fill-array-data
        element = struct.unpack_from('<H', data, base + (i + 1) * 2)[0]
        size = struct.unpack_from('<I', data, base + (i + 2) * 2)[0]
        return 4 + ((size * element + 1) // 2) * 2
    return 1

def iter_instructions(data, code_off):
    """Yield (index, opcode_or_0xff_for_payload, unit) at real instruction boundaries."""
    insns_size = struct.unpack_from('<I', data, code_off + 12)[0]
    base = code_off + 16
    i = 0
    while i < insns_size:
        unit = struct.unpack_from('<H', data, base + i * 2)[0]
        op = unit & 0xFF
        if op == 0 and unit != 0:
            yield i, 0x100, unit
            i += payload_width(data, base, i, unit)
            continue
        yield i, op, unit
        i += _W[op]

def method_code(data, class_size, class_off):
    """Yield (class_idx, method_idx, code_off) for every method with code."""
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
            if code_off:
                yield class_idx, method_idx, code_off
