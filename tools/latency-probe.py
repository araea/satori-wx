#!/usr/bin/env python3
"""Measures how fast satori-wx answers on this phone: N texts to `filehelper` (your own file
transfer assistant, nobody else sees them), timing

  http   the message.create round trip (module -> WeChat send pipeline hand-off)
  event  send -> the message-created event for that same message arriving on /v1/events
         (write into WeChat's database -> change notice -> read -> WebSocket)

    python3 tools/latency-probe.py [-n 10] [--port 5601] [--token TOKEN] [--target filehelper]

Standard library only. Gaps between sends are random so the result does not phase-lock to the
poller. On v0.11.0+ (inotify-driven) `event` is tens of milliseconds; the pre-v0.11 poller
showed a flat 0.2-1.9s spread. Needs root only to read the token from the module config.
"""
import argparse
import base64
import json
import os
import random
import socket
import statistics
import struct
import subprocess
import threading
import time
import urllib.request


def read_exact(sock, n):
    data = b''
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError('connection closed')
        data += chunk
    return data


def send_frame(sock, text):
    payload = text.encode()
    header = bytes([0x81])
    if len(payload) < 126:
        header += bytes([0x80 | len(payload)])
    else:
        header += bytes([0x80 | 126]) + struct.pack('!H', len(payload))
    mask = os.urandom(4)
    sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))


def read_frame(sock):
    first, second = read_exact(sock, 2)
    size = second & 127
    if size == 126:
        size = struct.unpack('!H', read_exact(sock, 2))[0]
    elif size == 127:
        size = struct.unpack('!Q', read_exact(sock, 8))[0]
    return first & 15, read_exact(sock, size)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-n', type=int, default=10)
    parser.add_argument('--port', type=int, default=5601)
    parser.add_argument('--token')
    parser.add_argument('--target', default='filehelper')
    args = parser.parse_args()
    token = args.token or subprocess.run(['su', '-c', 'grep ^token= /data/adb/modules/satori_wx/satori-wx.conf'],
                                         capture_output=True, text=True).stdout.strip().split('=', 1)[-1]

    sock = socket.create_connection(('127.0.0.1', args.port), timeout=15)
    sock.sendall(('GET /v1/events HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                  'Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n\r\n' % base64.b64encode(os.urandom(16)).decode()).encode())
    head = b''
    while b'\r\n\r\n' not in head:
        head += sock.recv(1)
    send_frame(sock, json.dumps({'op': 3, 'body': {'token': token}}))
    login = None
    while login is None:
        opcode, payload = read_frame(sock)
        signal = json.loads(payload) if opcode == 1 else {}
        if signal.get('op') == 4:
            login = signal['body']['logins'][0]['user']['id']
    sock.settimeout(0.05)

    def create(text, box):
        request = urllib.request.Request(
            'http://127.0.0.1:%d/v1/message.create' % args.port,
            data=json.dumps({'channel_id': args.target, 'content': text}).encode(), method='POST',
            headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json',
                     'Satori-Platform': 'wechat', 'Satori-User-ID': login})
        start = time.monotonic()
        with urllib.request.urlopen(request, timeout=30) as response:
            response.read()
        box['http'] = time.monotonic() - start

    https, events = [], []
    for i in range(args.n):
        tag = 'latency-probe %d/%d' % (os.getpid(), i)
        box = {}
        start = time.monotonic()
        worker = threading.Thread(target=create, args=(tag, box))
        worker.start()
        seen = None
        while seen is None and time.monotonic() - start < 20:
            try:
                opcode, payload = read_frame(sock)
            except socket.timeout:
                continue
            if opcode == 1 and tag.encode() in payload:
                seen = time.monotonic() - start
        worker.join()
        https.append(box['http'])
        if seen is not None:
            events.append(seen)
        print('#%-2d http %5.0f ms   event %s' % (i, box['http'] * 1000, '%5.0f ms' % (seen * 1000) if seen else ' none'), flush=True)
        time.sleep(random.uniform(0.05, 2.5))

    def line(name, values):
        if not values:
            return '%s: no samples' % name
        return '%s: min %.0f  median %.0f  max %.0f ms  (%d)' % (
            name, min(values) * 1000, statistics.median(values) * 1000, max(values) * 1000, len(values))
    print(line('http ', https))
    print(line('event', events))
    if len(events) < args.n:
        print('%d of %d events never arrived within 20s' % (args.n - len(events), args.n))


if __name__ == '__main__':
    main()
