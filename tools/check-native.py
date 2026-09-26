#!/usr/bin/env python3
"""Check ABI, exported entry, dynamic dependencies and forbidden embedded payloads."""
import pathlib
import re
import subprocess
import sys
p = pathlib.Path(sys.argv[1])
def readelf(*args):
    return subprocess.check_output(['readelf', *args, str(p)], text=True)
assert 'AArch64' in readelf('-h'), 'expected arm64-v8a'
assert not re.search(r'\((?:RUNPATH|RPATH)\)', readelf('-d')), 'unexpected library search path'
needed = re.findall(r'\(NEEDED\).*\[(.*?)\]', readelf('-d'))
assert set(needed) <= {'libc.so', 'libm.so', 'libdl.so', 'liblog.so'}, needed
assert 'zygisk_module_entry' in readelf('--dyn-syms', '--wide'), 'missing Zygisk entry'
blob = p.read_bytes()
for forbidden in (b'dex\n0', b'cdex001', b'satori_dex_start', b'InMemoryDexClassLoader', b'ArtMethod', b'setHiddenApiExemptions'):
    assert forbidden not in blob, f'forbidden payload: {forbidden!r}'
print(f'native check passed: {p.stat().st_size} bytes, NEEDED={needed}')
