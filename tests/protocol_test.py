#!/usr/bin/env python3
"""Only the standalone fixture has a backend and an event input pipe. Production exports neither."""
import importlib.util
import json
import os
import pathlib
import struct
import subprocess
import sys
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
        env = dict(os.environ, SATORI_TEST_EVENT_FD=str(read))
        self.proc = subprocess.Popen([wire.BINARY, '--fixture'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, pass_fds=(read,), env=env)
        os.close(read)
        self.proc.stdin.write(('token=' + TOKEN + '\n').encode()); self.proc.stdin.close()
        wire.PORT = int(self.proc.stdout.readline())
        self.base = self.http('internal/status')[1]['sequence']
    def tearDown(self):
        os.close(self.write); self.proc.terminate(); self.proc.wait(timeout=3); self.proc.stdout.close()
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
        body = (b'--boundary\r\nContent-Disposition: form-data; name="file"; filename="a.bin"\r\n'
                b'Content-Type: application/octet-stream\r\n\r\n\0\xff\x01\r\n--boundary--\r\n')
        h = {'Content-Type': 'multipart/form-data; boundary="boundary"', 'Satori-Platform': 'wechat', 'Satori-User-ID': 'fixture'}
        status, result = wire.ServerTests.http(self, '/v1/upload.create', body, headers=h)
        self.assertEqual((status, result), (200, {'file': 'https://example.invalid/3'}))
        self.assertEqual(wire.ServerTests.http(self, '/v1/upload.create', body[:-8], headers=h)[0], 400)
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
