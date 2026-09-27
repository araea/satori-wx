#!/usr/bin/env python3
"""Dump fields of one class from classes*.dex. Offline research only.
usage: dexfields.py <base.apk> <Lclass;>"""
import struct, sys, zipfile
def uleb(d,p):
    r=0;s=0
    while True:
        b=d[p];p+=1;r|=(b&0x7F)<<s
        if not b&0x80: return r,p
        s+=7
def main():
    apk,wanted=sys.argv[1],sys.argv[2]
    seen=set()
    with zipfile.ZipFile(apk) as z:
        for name in sorted(n for n in z.namelist() if n.startswith('classes') and n.endswith('.dex')):
            d=z.read(name)
            ssize,soff=struct.unpack_from('<II',d,0x38)
            strings=[]
            for i in range(ssize):
                p=struct.unpack_from('<I',d,soff+i*4)[0];_,q=uleb(d,p)
                strings.append(d[q:d.index(b'\x00',q)].decode('utf-8','replace'))
            tsize,toff=struct.unpack_from('<II',d,0x40)
            types=[strings[struct.unpack_from('<I',d,toff+i*4)[0]] for i in range(tsize)]
            fsize,foff=struct.unpack_from('<II',d,0x50)
            for i in range(fsize):
                cls,typ,nm=struct.unpack_from('<HHI',d,foff+i*8)
                if types[cls]!=wanted: continue
                line='%s %s' % (types[typ], strings[nm])
                if line not in seen: seen.add(line); print(line)
if __name__=='__main__': main()
