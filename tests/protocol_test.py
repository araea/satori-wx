#!/usr/bin/env python3
"""Only the standalone fixture has a backend and an event input pipe. Production exports neither."""
import importlib.util
import json
import os
import pathlib
import struct
import socket
import subprocess
import sys
import tempfile
import time
import unittest

spec = importlib.util.spec_from_file_location('wire', pathlib.Path(__file__).with_name('server_test.py'))
wire = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wire)
Wire = wire.Wire
TOKEN = wire.TOKEN

PARAMS = {
    'channel.get': {'channel_id': 'c'}, 'channel.list': {'guild_id': 'g'},
    'channel.create': {'guild_id': 'g', 'data': {}}, 'channel.update': {'channel_id': 'c', 'data': {}},
    'channel.delete': {'channel_id': 'c'}, 'channel.mute': {'channel_id': 'c', 'enable': True},
    'message.create': {'channel_id': 'c', 'content': '你好 &amp; <at id="u"/><img src="https://example.invalid/a"/>'},
    'message.update': {'channel_id': 'c', 'message_id': 'm', 'content': 'text'},
    'message.delete': {'channel_id': 'c', 'message_id': 'm'}, 'message.get': {'channel_id': 'c', 'message_id': 'm'},
    'message.list': {'channel_id': 'c', 'direction': 'around', 'order': 'asc', 'limit': 20},
    'reaction.create': {'channel_id': 'c', 'message_id': 'm', 'emoji_id': 'e'},
    'reaction.delete': {'channel_id': 'c', 'message_id': 'm', 'emoji_id': 'e'},
    'reaction.clear': {'channel_id': 'c', 'message_id': 'm'},
    'reaction.list': {'channel_id': 'c', 'message_id': 'm', 'emoji_id': 'e'},
    'guild.get': {'guild_id': 'g'}, 'guild.list': {},
    'guild.member.get': {'guild_id': 'g', 'user_id': 'u'}, 'guild.member.list': {'guild_id': 'g'},
    'guild.member.kick': {'guild_id': 'g', 'user_id': 'u', 'permanent': True},
    'guild.member.mute': {'guild_id': 'g', 'user_id': 'u', 'duration': 1000},
    'guild.member.role.set': {'guild_id': 'g', 'user_id': 'u', 'role_id': 'r'},
    'guild.member.role.unset': {'guild_id': 'g', 'user_id': 'u', 'role_id': 'r'},
    'guild.member.role.list': {'guild_id': 'g', 'user_id': 'u'},
    'guild.role.list': {'guild_id': 'g'}, 'guild.role.create': {'guild_id': 'g', 'data': {}},
    'guild.role.update': {'guild_id': 'g', 'role_id': 'r', 'data': {}},
    'guild.role.delete': {'guild_id': 'g', 'role_id': 'r'},
    'login.get': {}, 'user.get': {'user_id': 'u'}, 'user.channel.create': {'user_id': 'u'},
    'friend.list': {}, 'friend.delete': {'user_id': 'u'},
    'friend.approve': {'message_id': 'm', 'approve': False},
    'guild.approve': {'message_id': 'm', 'approve': True},
    'guild.member.approve': {'message_id': 'm', 'approve': True},
}

class ProtocolTests(unittest.TestCase):
    def setUp(self):
        read, self.write = os.pipe()
        self.media = tempfile.mkdtemp(prefix='satori-media-')
        env = dict(os.environ, SATORI_TEST_EVENT_FD=str(read), SATORI_TEST_MEDIA_DIR=self.media,
                   SATORI_TMPDIR=tempfile.mkdtemp(prefix='satori-upload-'))
        self.proc = subprocess.Popen([wire.BINARY, '--fixture'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, pass_fds=(read,), env=env)
        os.close(read)
        self.proc.stdin.write(('token=' + TOKEN + '\n').encode()); self.proc.stdin.close()
        wire.PORT = int(self.proc.stdout.readline())
        self.base = self.http('internal/status')[1]['sequence']
    def tearDown(self):
        os.close(self.write); self.proc.terminate(); self.proc.wait(timeout=3); self.proc.stdout.close()
        import shutil; shutil.rmtree(self.media, ignore_errors=True)
    def http(self, name, body=None, headers=None):
        h = {'Satori-Platform': 'wechat', 'Satori-User-ID': 'fixture'}
        h.update(headers or {})
        return wire.ServerTests.http(self, '/v1/' + name, json.dumps(body or {}).encode(), headers=h)
    def publish(self, body, meta=False):
        data = (('M' if meta else 'E') + json.dumps(body, ensure_ascii=False) + '\n').encode()
        os.write(self.write, data)
    def event(self, text='你好'):
        return {'type': 'message-created', 'login': {'sn': 0}, 'message': {'id': 'm', 'content': text},
                'channel': {'id': 'c', 'type': 0}, 'user': {'id': 'sender'}}
    def login(self, status=1, features=None):
        return {'sn': 0, 'status': status, 'adapter': 'test-fixture', 'platform': 'wechat', 'user': {'id': 'fixture'},
                'features': list(PARAMS) + ['upload.create'] if features is None else features}
    def wait_sequence(self, target):
        for _ in range(100):
            seq = self.http('internal/status')[1]['sequence']
            if seq >= target: return seq
            time.sleep(.01)
        self.fail(f'sequence did not reach {target}')
    def test_all_standard_routes_and_schema(self):
        status, capabilities = self.http('internal/capabilities')
        self.assertEqual(status, 200)
        self.assertEqual(set(capabilities['standard_methods']), set(PARAMS) | {'upload.create'})
        for name, params in PARAMS.items():
            with self.subTest(method=name):
                status, body = self.http(name, params)
                self.assertEqual(status, 200, body)
                if name.endswith('.list'): self.assertIsInstance(body['data'], list)
                if name == 'message.create': self.assertEqual(body[0]['content'], params['content'])
        self.assertEqual(self.http('message.create', {'channel_id': 'c'})[0], 400)
        self.assertEqual(self.http('friend.approve', {'message_id': 'm', 'approve': 'false'})[0], 400)
        self.assertEqual(self.http('message.list', {'channel_id': 'c', 'direction': 'wrong'})[0], 400)
        self.assertEqual(self.http('message.list', {'channel_id': 'c', 'limit': 0})[0], 400)
        self.assertEqual(self.http('guild.member.mute', {'guild_id': 'g', 'user_id': 'u', 'duration': -1})[0], 400)
    def test_pagination(self):
        _, first = self.http('message.list', {'channel_id': 'c'})
        _, second = self.http('message.list', {'channel_id': 'c', 'next': first['next']})
        self.assertNotIn('next', second)
    def test_upload_binary(self):
        payload = b'\0\xff\x01'
        body = (b'--boundary\r\nContent-Disposition: form-data; name="file"; filename="a.bin"\r\n'
                b'Content-Type: application/octet-stream\r\n\r\n' + payload + b'\r\n--boundary--\r\n')
        h = {'Content-Type': 'multipart/form-data; boundary="boundary"', 'Satori-Platform': 'wechat', 'Satori-User-ID': 'fixture'}
        status, result = wire.ServerTests.http(self, '/v1/upload.create', body, headers=h)
        self.assertEqual(status, 200)
        url = result['file']
        self.assertTrue(url.startswith('internal:wechat/fixture/_tmp/'), url)
        # The proxy route serves the stored bytes without the Satori login headers.
        pstatus, headers, pbody = wire.ServerTests.raw_http('/v1/proxy/' + url)
        self.assertEqual((pstatus, pbody), (200, payload))
        self.assertEqual(headers['content-type'], 'application/octet-stream')
        # Truncated multipart is rejected before anything is stored.
        self.assertEqual(wire.ServerTests.http(self, '/v1/upload.create', body[:-8], headers=h)[0], 400)
    def test_proxy_route(self):
        self.assertEqual(wire.ServerTests.raw_http('/v1/proxy/not-a-url')[0], 400)
        self.assertEqual(wire.ServerTests.raw_http('/v1/proxy/internal:broken')[0], 400)
        self.assertEqual(wire.ServerTests.raw_http('/v1/proxy/internal:wechat/nobody/_tmp/x')[0], 404)
        self.assertEqual(wire.ServerTests.raw_http('/v1/proxy/internal:wechat/fixture/_tmp/missing')[0], 404)
        self.assertEqual(wire.ServerTests.raw_http('/v1/proxy/https://example.com/a.png')[0], 403)
        self.assertEqual(wire.ServerTests.raw_http('/v1/proxy/internal:wechat/fixture/_tmp/x', method='POST')[0], 405)
    def multipart(self, payload, name='a.bin', ctype='application/octet-stream'):
        body = (b'--boundary\r\nContent-Disposition: form-data; name="file"; filename="' + name.encode() + b'"\r\n'
                b'Content-Type: ' + ctype.encode() + b'\r\n\r\n' + payload + b'\r\n--boundary--\r\n')
        return body, {'Content-Type': 'multipart/form-data; boundary="boundary"', 'Satori-Platform': 'wechat', 'Satori-User-ID': 'fixture'}
    def test_large_upload_and_streamed_proxy(self):
        # Far past the 16 KiB JSON limit: upload.create takes a real file, and the proxy streams
        # it back in chunks instead of truncating at one read.
        payload = os.urandom(3 * 1024 * 1024 + 17)
        body, h = self.multipart(payload)
        status, result = wire.ServerTests.http(self, '/v1/upload.create', body, headers=h)
        self.assertEqual(status, 200, result)
        url = result['file']
        pstatus, headers, pbody = wire.ServerTests.raw_http('/v1/proxy/' + url)
        self.assertEqual(pstatus, 200)
        self.assertEqual(int(headers['content-length']), len(payload))
        self.assertEqual(pbody, payload)
        self.assertEqual(headers['accept-ranges'], 'bytes')
        # Percent-encoded links (some SDKs encode the whole internal: URL) resolve the same.
        import urllib.parse
        self.assertEqual(wire.ServerTests.raw_http('/v1/proxy/' + urllib.parse.quote(url, safe=''))[2], payload)
        # Byte ranges, for players that seek.
        for spec, expected, code in [('bytes=10-19', payload[10:20], 206), ('bytes=-5', payload[-5:], 206),
                                     ('bytes=%d-' % (len(payload) - 3), payload[-3:], 206),
                                     ('bytes=0-99999999999', payload, 206)]:
            with self.subTest(range=spec):
                rstatus, rheaders, rbody = wire.ServerTests.raw_http('/v1/proxy/' + url, headers={'Range': spec})
                self.assertEqual((rstatus, rbody), (code, expected))
                self.assertIn('content-range', rheaders)
        rstatus, rheaders, _ = wire.ServerTests.raw_http('/v1/proxy/' + url, headers={'Range': 'bytes=%d-' % (len(payload) + 5)})
        self.assertEqual(rstatus, 416)
        self.assertEqual(rheaders['content-range'], 'bytes */%d' % len(payload))
        # HEAD announces the length and sends no body.
        hstatus, hheaders, hbody = wire.ServerTests.raw_http('/v1/proxy/' + url, method='HEAD')
        self.assertEqual((hstatus, int(hheaders['content-length']), hbody), (200, len(payload), b''))
    def test_large_upload_needs_token_before_the_body(self):
        big = b'{"pad":"' + b'a' * 20000 + b'"}'
        # An oversized JSON body is refused outright...
        self.assertEqual(self.http('message.get', json.loads(big))[0], 413)
        # ...and a big upload from a caller without the token is refused from the headers alone,
        # before any of the body is read (nothing follows the headers here).
        for auth, code in [(None, 401), ('b' * 64, 403)]:
            with Wire() as w:
                head = (b'POST /v1/upload.create HTTP/1.1\r\nHost: x\r\nContent-Type: multipart/form-data; boundary=b\r\n'
                        b'Content-Length: 5000000\r\n' + (b'Authorization: Bearer ' + auth.encode() + b'\r\n' if auth else b'') + b'\r\n')
                w.sock.sendall(head)
                self.assertEqual(int(w.file.readline().split()[1]), code)
    def test_message_create_takes_an_inline_picture(self):
        # An <img src="data:..."> carries the picture inside message.create, so that one route
        # (like upload.create) accepts a body far past the 16 KiB JSON limit.
        content = '<img src="data:image/png;base64,' + 'QUJD' * 700000 + '"/>'  # ~2.8 MB
        status, body = self.http('message.create', {'channel_id': 'c', 'content': content})
        self.assertEqual(status, 200)
        self.assertEqual(len(body[0]['content']), len(content))
        # Any other route keeps the small limit, and the big one still needs the token.
        self.assertEqual(self.http('message.update', {'channel_id': 'c', 'message_id': 'm', 'content': 'x' * 20000})[0], 413)
        with Wire() as w:
            w.sock.sendall(b'POST /v1/message.create HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: 5000000\r\n\r\n')
            self.assertEqual(int(w.file.readline().split()[1]), 401)
    def test_expect_continue(self):
        body, h = self.multipart(b'expect-me')
        with Wire() as w:
            head = (b'POST /v1/upload.create HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nAuthorization: Bearer ' + TOKEN.encode() +
                    b'\r\nSatori-Platform: wechat\r\nSatori-User-ID: fixture\r\nContent-Type: ' + h['Content-Type'].encode() +
                    b'\r\nContent-Length: ' + str(len(body)).encode() + b'\r\n\r\n')
            w.sock.sendall(head)
            self.assertEqual(w.file.readline(), b'HTTP/1.1 100 Continue\r\n')
            self.assertEqual(w.file.readline(), b'\r\n')
            w.sock.sendall(body)
            self.assertEqual(int(w.file.readline().split()[1]), 200)
        with Wire() as w:  # Any other expectation is still refused.
            w.sock.sendall(b'POST /v1/meta HTTP/1.1\r\nHost: x\r\nExpect: something\r\n\r\n')
            self.assertEqual(int(w.file.readline().split()[1]), 417)
    def test_upload_streams_past_the_json_limit(self):
        # 40 MiB: three times the old buffered ceiling. The body is written to the store as it
        # arrives, in odd-sized pieces that straddle the multipart boundary.
        payload = os.urandom(40 * 1024 * 1024 + 3)
        payload = payload[:1000] + b'\r\n--boundary' + payload[1000:]   # data that looks like the delimiter
        body, h = self.multipart(payload, name='clip.mp4', ctype='video/mp4')
        with Wire() as w:
            head = (b'POST /v1/upload.create HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer ' + TOKEN.encode() +
                    b'\r\nSatori-Platform: wechat\r\nSatori-User-ID: fixture\r\nContent-Type: ' + h['Content-Type'].encode() +
                    b'\r\nContent-Length: ' + str(len(body)).encode() + b'\r\n\r\n')
            w.sock.sendall(head)
            view = memoryview(body)
            step = 700001
            for off in range(0, len(body), step):
                w.sock.sendall(view[off:off + step])
            status = int(w.file.readline().split()[1])
            self.assertEqual(status, 200)
        # Read the answer through the ordinary helper as well, then fetch the file back.
        body2, h2 = self.multipart(b'second small one', name='b.txt', ctype='text/plain')
        s2, r2 = wire.ServerTests.http(self, '/v1/upload.create', body2, headers=h2)
        self.assertEqual(s2, 200)
        self.assertEqual(wire.ServerTests.raw_http('/v1/proxy/' + r2['file'])[2], b'second small one')
    def test_streamed_upload_round_trip(self):
        payload = os.urandom(5 * 1024 * 1024 + 11)
        body, h = self.multipart(payload, name='ある.bin')
        status, result = wire.ServerTests.http(self, '/v1/upload.create', body, headers=h)
        self.assertEqual(status, 200, result)
        pstatus, headers, pbody = wire.ServerTests.raw_http('/v1/proxy/' + result['file'])
        self.assertEqual((pstatus, pbody), (200, payload))
    def test_streamed_upload_is_checked_before_the_body(self):
        body, h = self.multipart(os.urandom(100000))
        base = (b'POST /v1/upload.create HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer ' + TOKEN.encode() + b'\r\n')
        length = b'\r\nContent-Length: ' + str(len(body)).encode() + b'\r\n\r\n'
        cases = [
            ('unknown login', b'Satori-Platform: wechat\r\nSatori-User-ID: nobody\r\nContent-Type: ' + h['Content-Type'].encode(), 403),
            ('no login headers', b'Content-Type: ' + h['Content-Type'].encode(), 400),
            ('not multipart', b'Satori-Platform: wechat\r\nSatori-User-ID: fixture\r\nContent-Type: application/json', 415),
            ('multipart without boundary', b'Satori-Platform: wechat\r\nSatori-User-ID: fixture\r\nContent-Type: multipart/form-data; x=1', 415),
        ]
        for name, extra, code in cases:
            with self.subTest(name), Wire() as w:
                w.sock.sendall(base + extra + length)   # only the headers: the answer must not wait for the body
                self.assertEqual(int(w.file.readline().split()[1]), code)
        # Announcing more than the streaming ceiling is refused outright.
        with Wire() as w:
            w.sock.sendall(base + b'Satori-Platform: wechat\r\nSatori-User-ID: fixture\r\nContent-Type: ' + h['Content-Type'].encode() +
                           b'\r\nContent-Length: 2000000000\r\n\r\n')
            self.assertEqual(int(w.file.readline().split()[1]), 413)
    def test_streamed_upload_rejects_malformed_bodies(self):
        payload = os.urandom(200000)
        good, h = self.multipart(payload)
        head = lambda n: (b'POST /v1/upload.create HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer ' + TOKEN.encode() +
                          b'\r\nSatori-Platform: wechat\r\nSatori-User-ID: fixture\r\nContent-Type: ' + h['Content-Type'].encode() +
                          b'\r\nContent-Length: ' + str(n).encode() + b'\r\n\r\n')
        for name, body in [('no closing boundary', good[:-len(b'--boundary--\r\n')]),
                           ('junk after the end', good + b'junk'),
                           ('garbage before the boundary', b'junk\r\n' + good)]:
            with self.subTest(name), Wire() as w:
                w.sock.sendall(head(len(body)) + body)
                self.assertEqual(int(w.file.readline().split()[1]), 400)
        # A client that announces a big body and then goes away leaves the server healthy.
        with Wire() as w:
            w.sock.sendall(head(len(good)) + good[:50000])
        self.assertEqual(self.http('meta')[0], 200)
        # Expect: 100-continue works on the streamed path as well.
        with Wire() as w:
            w.sock.sendall(head(len(good)).replace(b'\r\nContent-Type', b'\r\nExpect: 100-continue\r\nContent-Type', 1))
            self.assertEqual(w.file.readline(), b'HTTP/1.1 100 Continue\r\n')
            self.assertEqual(w.file.readline(), b'\r\n')
            w.sock.sendall(good)
            self.assertEqual(int(w.file.readline().split()[1]), 200)
    def test_media_resolver_route(self):
        payload = bytes(range(256)) * 4096
        with open(os.path.join(self.media, 'clip.mp4'), 'wb') as f: f.write(payload)
        with open(os.path.join(self.media, 'denied'), 'wb') as f: f.write(b'secret')
        status, headers, body = wire.ServerTests.raw_http('/v1/proxy/internal:wechat/fixture/_msg/clip.mp4')
        self.assertEqual((status, body, headers['content-type']), (200, payload, 'video/mp4'))
        # The resolver decides what is served; a refused link is indistinguishable from an unknown one.
        self.assertEqual(wire.ServerTests.raw_http('/v1/proxy/internal:wechat/fixture/_msg/denied')[0], 404)
        self.assertEqual(wire.ServerTests.raw_http('/v1/proxy/internal:wechat/fixture/_msg/missing')[0], 404)
        self.assertEqual(wire.ServerTests.raw_http('/v1/proxy/internal:wechat/fixture/_other/clip.mp4')[0], 404)
        self.assertEqual(wire.ServerTests.raw_http('/v1/proxy/internal:wechat/nobody/_msg/clip.mp4')[0], 404)
    def test_stalled_reader_does_not_block_others(self):
        with open(os.path.join(self.media, 'big.mp4'), 'wb') as f: f.write(os.urandom(8 * 1024 * 1024))
        slow = socket.create_connection(('127.0.0.1', wire.PORT), timeout=3)
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        try:
            slow.sendall(b'GET /v1/proxy/internal:wechat/fixture/_msg/big.mp4 HTTP/1.1\r\nHost: x\r\n\r\n')
            time.sleep(.3)  # never read: the server must park this stream, not spin or buffer it all
            self.assertEqual(self.http('meta')[0], 200)
            self.assertEqual(len(wire.ServerTests.raw_http('/v1/proxy/internal:wechat/fixture/_msg/big.mp4')[2]), 8 * 1024 * 1024)
        finally: slow.close()
    def test_large_response_and_large_event(self):
        status, body = self.http('message.list', {'channel_id': 'big'})
        self.assertEqual((status, len(body['data'])), (200, 40))  # ~160 KB of JSON in one reply
        text = '长' * 30000  # ~90 KB of UTF-8 in a single event, past the old 4 KiB cap
        with Wire() as w:
            w.upgrade(); w.identify()
            self.publish(self.event(text))
            got = json.loads(w.receive()[1])['body']['message']['content']
            self.assertEqual(got, text)
            sn = self.http('internal/status')[1]['sequence']
        with Wire() as w:  # ...and it replays in full to a client that resumes across it.
            w.upgrade(); w.identify(sn=sn - 1)
            self.assertEqual(json.loads(w.receive()[1])['body']['message']['content'], text)
    def test_ready_login_and_meta(self):
        with Wire() as w:
            w.upgrade(); _, ready = w.identify()
            ready = json.loads(ready)
            self.assertEqual(ready['body'], self.http('meta')[1])
            self.assertEqual(self.http('login.get')[1], ready['body']['logins'][0])
            self.publish({'proxy_urls': [], 'logins': [{'fake': True}]}, meta=True)
            op, body = w.receive()
            self.assertEqual((op, json.loads(body)), (1, {'op': 5, 'body': {'proxy_urls': []}}))
    def test_event_broadcast_and_replay(self):
        with Wire() as a, Wire() as b:
            a.upgrade(); a.identify(); b.upgrade(); b.identify()
            self.publish(self.event())
            event = json.loads(a.receive()[1])
            self.assertEqual(event, json.loads(b.receive()[1]))
            self.assertEqual(event['op'], 0)
            body = event['body']; self.assertEqual(body['message']['content'], '你好')
            self.assertEqual(set(body['login']), {'sn', 'user', 'platform'})
            self.assertGreater(body['timestamp'], 1700000000000)
        self.publish(self.event('断线消息')); self.wait_sequence(body['sn'] + 1)
        with Wire() as c:
            c.upgrade(); c.identify(sn=body['sn'])
            replay = json.loads(c.receive()[1])
            self.assertEqual(replay['body']['message']['content'], '断线消息')
    def test_login_events_order_and_no_replay(self):
        with Wire() as w:
            w.upgrade(); w.identify()
            self.publish(self.event('before'))
            self.publish({'type': 'login-updated', 'login': self.login(status=0)})
            first = json.loads(w.receive()[1])['body']
            second = json.loads(w.receive()[1])['body']
            self.assertEqual((first['type'], second['type']), ('message-created', 'login-updated'))
            self.assertGreater(second['sn'], first['sn'])
        with Wire() as w:
            w.upgrade(); _, ready = w.identify(sn=first['sn'])
            self.assertEqual(json.loads(ready)['body']['logins'][0]['status'], 0)
            w.send('{"op":1}')
            self.assertEqual(w.receive(), (1, b'{"op":2}'))
        self.assertEqual(self.http('message.create', PARAMS['message.create'])[0], 503)
    def test_unsupported_feature(self):
        self.publish({'type': 'login-updated', 'login': self.login(features=[])})
        self.wait_sequence(self.base + 1)
        self.assertEqual(self.http('message.create', PARAMS['message.create'])[0], 404)
        # Availability is decided before a method's own parameter rules: an unsupported
        # method answers 404 even when the parameters are missing or malformed, instead of
        # a misleading 400 that suggests the call would work with better arguments.
        self.assertEqual(self.http('message.create', {})[0], 404)
        self.assertEqual(self.http('message.list', {'channel_id': 'c', 'limit': 0})[0], 404)
    def test_login_removal(self):
        self.publish({'type': 'login-removed', 'login': self.login(status=0)})
        self.wait_sequence(self.base + 1)
        self.assertEqual(self.http('meta')[1]['logins'], [])
        self.assertEqual(self.http('login.get')[0], 403)
    def test_replay_floor(self):
        for i in range(80): self.publish(self.event(str(i)))
        self.wait_sequence(self.base + 80)
        with Wire() as w:
            w.upgrade(); op, body = w.identify(sn=self.base)
            self.assertEqual((op, struct.unpack('!H', body)[0]), (8, 4009))
        with Wire() as w:
            w.upgrade(); w.identify(sn=self.base + 79)
            self.assertEqual(json.loads(w.receive()[1])['body']['message']['content'], '79')
    def test_replay_larger_than_send_buffer(self):
        for i in range(20): self.publish(self.event(str(i) + ':' + '文' * 600))
        self.wait_sequence(self.base + 20)
        with Wire() as w:
            w.upgrade(); w.identify(sn=self.base)
            for i in range(20):
                self.assertTrue(json.loads(w.receive()[1])['body']['message']['content'].startswith(str(i) + ':'))

if __name__ == '__main__': unittest.main()
