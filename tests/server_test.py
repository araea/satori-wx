#!/usr/bin/env python3
import base64
import hashlib
import http.client
import json
import os
import socket
import struct
import subprocess
import sys
import time
import unittest

BINARY = sys.argv.pop(1)
TOKEN = 'a' * 64
PROC = None
PORT = 0

class Wire:
    def __init__(self):
        self.sock = socket.create_connection(('127.0.0.1', PORT), timeout=3)
        self.file = self.sock.makefile('rb')
    def close(self):
        self.file.close()
        self.sock.close()
    def __enter__(self): return self
    def __exit__(self, *args): self.close()
    def upgrade(self, extra=b'', key=None):
        key = key or base64.b64encode(os.urandom(16))
        self.sock.sendall(b'GET /v1/events HTTP/1.1\r\nHost: localhost\r\nUpgrade: WebSocket\r\n'
                          b'Connection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\n'
                          b'Sec-WebSocket-Key: ' + key + b'\r\n\r\n' + extra)
        status = self.file.readline()
        assert status.startswith(b'HTTP/1.1 101 '), status
        headers = {}
        while (line := self.file.readline()) != b'\r\n':
            assert line
            name, value = line.split(b':', 1)
            headers[name.lower()] = value.strip()
        expected = base64.b64encode(hashlib.sha1(key + b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
        assert headers[b'sec-websocket-accept'] == expected, headers
    @staticmethod
    def packet(payload, op=1, fin=True, masked=True):
        if isinstance(payload, str): payload = payload.encode()
        size = len(payload)
        prefix = bytes([(128 if fin else 0) | op])
        maskbit = 128 if masked else 0
        if size < 126: prefix += bytes([maskbit | size])
        elif size < 65536: prefix += bytes([maskbit | 126]) + struct.pack('!H', size)
        else: prefix += bytes([maskbit | 127]) + struct.pack('!Q', size)
        if not masked: return prefix + payload
        mask = os.urandom(4)
        return prefix + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
    def send(self, payload, **kwargs): self.sock.sendall(self.packet(payload, **kwargs))
    def receive(self):
        def read(n):
            data = self.file.read(n)
            assert len(data) == n, (len(data), n)
            return data
        first, second = read(2)
        assert first & 128 and not second & 128
        size = second & 127
        if size == 126: size = struct.unpack('!H', read(2))[0]
        if size == 127: size = struct.unpack('!Q', read(8))[0]
        return first & 15, read(size)
    def identify(self, token=TOKEN, **extra):
        self.send(json.dumps({'op': 3, 'body': {'token': token, **extra}}))
        return self.receive()

class ServerTests(unittest.TestCase):
    def http(self, path='/v1/meta', body=b'{}', auth=TOKEN, method='POST', headers=None):
        c = http.client.HTTPConnection('127.0.0.1', PORT, timeout=3)
        h = {'Content-Type': 'application/json'}
        if auth is not None: h['Authorization'] = 'Bearer ' + auth
        h.update(headers or {})
        try:
            c.request(method, path, body, h)
            response = c.getresponse()
            return response.status, json.loads(response.read())
        finally: c.close()
    @staticmethod
    def raw_http(path, method='GET', body=None, headers=None):
        c = http.client.HTTPConnection('127.0.0.1', PORT, timeout=3)
        try:
            c.request(method, path, body, headers or {})
            response = c.getresponse()
            return response.status, {k.lower(): v for k, v in response.getheaders()}, response.read()
        finally: c.close()
    def raw_status(self, data):
        with Wire() as w:
            w.sock.sendall(data)
            return int(w.file.readline().split()[1])
    def ws_close(self, payload, expected=4000, **kwargs):
        with Wire() as w:
            w.upgrade(); w.send(payload, **kwargs)
            op, data = w.receive()
            self.assertEqual(op, 8)
            self.assertEqual(struct.unpack('!H', data[:2])[0], expected)
    def test_meta_and_status(self):
        self.assertEqual(self.http(), (200, {'logins': [], 'proxy_urls': []}))
        status, body = self.http('/v1/internal/status')
        self.assertEqual(status, 200)
        self.assertTrue(body['native'])
        self.assertEqual(body['backend'], 'unavailable')
    def test_auth(self):
        self.assertEqual(self.http(auth=None)[0], 401)
        self.assertEqual(self.http(auth='b' * 64)[0], 403)
    def test_routes(self):
        self.assertEqual(self.http('/v1/login.list')[0], 404) # Not a Satori v1 API.
        self.assertEqual(self.http(method='GET')[0], 405)
        self.assertEqual(self.http('/v1/login.get')[0], 400)
        h = {'Satori-Platform': 'wechat', 'Satori-User-ID': 'test'}
        self.assertEqual(self.http('/v1/login.get', headers=h)[0], 403)
        self.assertEqual(self.http('/v1/message.create', body=b'{"channel_id":"x","content":"hello"}', headers=h)[0], 403)
    def test_pat_endpoint(self):
        # The pat route follows the account rules of the rpc methods; without a backend
        # provider a known login would answer 501, and there is none in the bare hub.
        self.assertEqual(self.http('/v1/internal/pat')[0], 400)  # missing login headers
        h = {'Satori-Platform': 'wechat', 'Satori-User-ID': 'test'}
        self.assertEqual(self.http('/v1/internal/pat', headers=h)[0], 403)  # unknown login
    def test_json_rejection(self):
        cases = [b'[]', b'null', b'{', b'{}x', b'{}\0', b'{"x":1,"x":2}',
                 b'{"x":\x011}', b'{"x":\x0b1}', b'{"x":01}', b'{"x":1.}', b'{"x":"\n"}', b'{"x":"\xff"}',
                 b'{"x":"\\u0000suffix"}', b'{"x":' * 18 + b'0' + b'}' * 18]
        for data in cases:
            with self.subTest(data=data): self.assertEqual(self.http(body=data)[0], 400)
    def test_valid_json(self):
        self.assertEqual(self.http(body=b'{"x":[-0.1e+2,"\\u4f60\\u597d",true,null]}')[0], 200)
    def test_size_limits(self):
        self.assertEqual(self.http(body=b'a' * 16385)[0], 413)
        with Wire() as w:
            w.sock.sendall(b'X' * 8192)
            self.assertIn(b'431', w.file.readline())
    def test_header_validation(self):
        cases = [b'Content-Length: 0\r\nContent-Length: 0\r\n', b'Content-Length: \r\n',
                 b'Content-Length: -1\r\n', b'Transfer-Encoding: chunked\r\n',
                 b'X-Test: hello\nworld\r\n', b'X-Test: a\0b\r\n']
        for headers in cases:
            with self.subTest(headers=headers):
                self.assertEqual(self.raw_status(b'POST /v1/meta HTTP/1.1\r\nHost: localhost\r\n' + headers + b'\r\n'), 400)
    def test_upgrade_vectors(self):
        for key in [b'dGhlIHNhbXBsZSBub25jZQ=='] + [base64.b64encode(os.urandom(16)) for _ in range(12)]:
            with Wire() as w: w.upgrade(key=key)
    def test_invalid_key(self):
        self.assertEqual(self.raw_status(b'GET /v1/events HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n'
            b'Connection: upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: invalid\r\n\r\n'), 400)
    def test_identify_ping_close(self):
        with Wire() as w:
            w.upgrade()
            op, payload = w.identify()
            self.assertEqual((op, json.loads(payload)), (1, {'op': 4, 'body': {'logins': [], 'proxy_urls': []}}))
            w.send('{"op":1}')
            self.assertEqual(w.receive(), (1, b'{"op":2}'))
            w.send(b'ping', op=9)
            self.assertEqual(w.receive(), (10, b'ping'))
            w.send(struct.pack('!H', 1000), op=8)
            self.assertEqual(w.receive(), (8, struct.pack('!H', 1000)))
    def test_bad_token(self):
        self.ws_close(json.dumps({'op': 3, 'body': {'token': 'bad'}}), 4004)
    def test_escaped_token(self):
        with Wire() as w:
            w.upgrade(); w.send('{"op":3,"body":{"token":"' + '\\u0061' * 64 + '"}}')
            self.assertEqual(json.loads(w.receive()[1])['op'], 4)
    def test_resume(self):
        with Wire() as w:
            w.upgrade(); self.assertEqual(json.loads(w.identify(sn=0)[1])['op'], 4)
        self.ws_close(json.dumps({'op': 3, 'body': {'token': TOKEN, 'sn': 1}}), 4009)
    def test_repeated_identify(self):
        with Wire() as w:
            w.upgrade(); w.identify()
            op, body = w.identify()
            self.assertEqual((op, struct.unpack('!H', body)[0]), (8, 4000))
    def test_fragmentation_and_control_interleave(self):
        with Wire() as w:
            w.upgrade()
            data = json.dumps({'op': 3, 'body': {'token': TOKEN}}).encode()
            w.send(data[:10], fin=False)
            w.send('transport', op=9)
            self.assertEqual(w.receive(), (10, b'transport'))
            w.send(data[10:], op=0)
            self.assertEqual(json.loads(w.receive()[1])['op'], 4)
    def test_upgrade_and_frame_coalesced(self):
        with Wire() as w:
            w.upgrade(extra=Wire.packet(json.dumps({'op': 3, 'body': {'token': TOKEN}})))
            self.assertEqual(json.loads(w.receive()[1])['op'], 4)
    def test_split_tcp_frame(self):
        with Wire() as w:
            w.upgrade()
            packet = Wire.packet(json.dumps({'op': 3, 'body': {'token': TOKEN}}))
            for start in range(0, len(packet), 3): w.sock.sendall(packet[start:start+3])
            self.assertEqual(json.loads(w.receive()[1])['op'], 4)
    def test_frame_rejections(self):
        self.ws_close('{"op":1}', 1002, masked=False)
        self.ws_close(b'x', 1002, op=0)
        self.ws_close(b'x', 1002, op=9, fin=False)
        self.ws_close(b'x', 1002, op=8)
        self.ws_close(b'\xff', 1007)
        self.ws_close(b'x', 1003, op=2)
        self.ws_close(b'a' * 16385, 1009)
        self.ws_close(b'a' * 65536, 1009)
    def test_signal_rejections(self):
        for data in ['{"op":1}', '{}', '[]', '{"op":3,"op":1}', '{"op":3,"body":{}}',
                     '{"op":3,"body":{"token":"' + TOKEN + '\\u0000evil"}}']:
            with self.subTest(data=data):
                self.ws_close(data, 4004 if data == '{"op":3,"body":{}}' else 4000)
    def test_coalesced_frames(self):
        with Wire() as w:
            w.upgrade(); w.identify()
            w.sock.sendall(Wire.packet('{"op":1}') * 70)
            for _ in range(70): self.assertEqual(w.receive(), (1, b'{"op":2}'))
    def test_partial_clients_do_not_block(self):
        sockets = [socket.create_connection(('127.0.0.1', PORT), timeout=3) for _ in range(3)]
        try:
            for s in sockets: s.sendall(b'POST /v1/')
            self.assertEqual(self.http()[0], 200)
        finally:
            for s in sockets: s.close()
    def test_identify_timeout(self):
        with Wire() as w:
            w.upgrade(); w.sock.settimeout(12)
            started = time.monotonic()
            op, payload = w.receive()
            self.assertEqual((op, struct.unpack('!H', payload)[0]), (8, 4004))
            self.assertGreaterEqual(time.monotonic() - started, 9)

if __name__ == '__main__':
    # Config failures must fail closed, before opening a socket.
    for invalid in ['port=5601\n', 'token=short\n', 'token=' + TOKEN + '\nport=0\n',
                    'token=' + TOKEN + '\ntoken=' + TOKEN + '\n']:
        p = subprocess.run([BINARY], input=invalid.encode(), capture_output=True, timeout=3)
        assert p.returncode == 2, p
    PROC = subprocess.Popen([BINARY], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        PROC.stdin.write(('port=5601\ntoken=' + TOKEN + '\n').encode())
        PROC.stdin.close()
        PORT = int(PROC.stdout.readline())
        unittest.main()
    finally:
        PROC.terminate()
        PROC.wait(timeout=3)
        PROC.stdout.close()
