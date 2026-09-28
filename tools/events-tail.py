#!/usr/bin/env python3
"""Prints the events a satori-wx server sends over /v1/events, one JSON line each.

Standard library only (a minimal RFC 6455 client), so it runs on the phone as is:

    python3 tools/events-tail.py [--port 5601] [--token TOKEN] [--types message-created,message-deleted]

The token defaults to the one in the module's config (needs root: `su -c cat ...`); pass --token
to avoid that. Ctrl-C to stop. Useful for checking a new build by hand: send yourself a message,
recall it, add someone to a test group, and watch the events arrive.
"""
import argparse
import base64
import json
import os
import socket
import struct
import subprocess
import sys
import time


def read_exact(sock, n):
    data = b''
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError('connection closed')
        data += chunk
    return data


def send_frame(sock, payload, opcode=1):
    payload = payload.encode() if isinstance(payload, str) else payload
    header = bytes([0x80 | opcode])
    size = len(payload)
    if size < 126:
        header += bytes([0x80 | size])
    elif size < 65536:
        header += bytes([0x80 | 126]) + struct.pack('!H', size)
    else:
        header += bytes([0x80 | 127]) + struct.pack('!Q', size)
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


def default_token():
    out = subprocess.run(['su', '-c', 'grep ^token= /data/adb/modules/satori_wx/satori-wx.conf'],
                         capture_output=True, text=True)
    return out.stdout.strip().split('=', 1)[-1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=5601)
    parser.add_argument('--token')
    parser.add_argument('--types', help='comma-separated event types to print (default: all)')
    args = parser.parse_args()
    token = args.token or default_token()
    wanted = set(args.types.split(',')) if args.types else None

    sock = socket.create_connection((args.host, args.port), timeout=15)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(('GET /v1/events HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                  'Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n\r\n' % (args.host, args.port, key)).encode())
    response = b''
    while b'\r\n\r\n' not in response:
        response += sock.recv(1)
    if b' 101 ' not in response.split(b'\r\n')[0]:
        sys.exit('upgrade refused: ' + response.split(b'\r\n')[0].decode())
    send_frame(sock, json.dumps({'op': 3, 'body': {'token': token}}))
    last_ping = time.monotonic()
    sock.settimeout(1)
    while True:
        if time.monotonic() - last_ping > 10:
            send_frame(sock, '{"op":1}')
            last_ping = time.monotonic()
        try:
            opcode, payload = read_frame(sock)
        except socket.timeout:
            continue
        if opcode == 8:
            code = struct.unpack('!H', payload[:2])[0] if len(payload) >= 2 else 0
            sys.exit('closed by server (%d)' % code)
        if opcode == 9:
            send_frame(sock, payload, 10)
            continue
        if opcode != 1:
            continue
        signal = json.loads(payload)
        if signal.get('op') == 4:
            logins = signal['body'].get('logins', [])
            print(json.dumps({'ready': [login.get('user', {}).get('id') for login in logins]}, ensure_ascii=False), flush=True)
        elif signal.get('op') == 0:
            body = signal['body']
            if wanted is None or body.get('type') in wanted:
                print(json.dumps(body, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
