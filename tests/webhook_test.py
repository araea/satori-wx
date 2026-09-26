#!/usr/bin/env python3
"""WebHook delivery test (Satori optional feature).

A local recorder acts as the application side: it receives the SDK's HTTP pushes
and checks Satori-Opcode, optional Authorization and the signal body.
"""
import http.server
import importlib.util
import json
import os
import pathlib
import socketserver
import subprocess
import threading
import time
import unittest

spec = importlib.util.spec_from_file_location('wire', pathlib.Path(__file__).with_name('server_test.py'))
wire = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wire)
TOKEN = wire.TOKEN

class Recorder:
    def __init__(self):
        self.records = []
        self.lock = threading.Lock()
        outer = self
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get('Content-Length', 0))
                body = self.rfile.read(length)
                with outer.lock:
                    outer.records.append({
                        'path': self.path,
                        'opcode': self.headers.get('Satori-Opcode'),
                        'auth': self.headers.get('Authorization'),
                        'body': body,
                    })
                try:
                    self.send_response(200)
                    self.send_header('Content-Length', '2')
                    self.end_headers()
                    self.wfile.write(b'{}')
                except (BrokenPipeError, ConnectionResetError):
                    pass  # The SDK only reads the status line before closing.
            def log_message(self, *args): pass
        class Server(socketserver.ThreadingTCPServer):
            def handle_error(self, request, client_address): pass
        self.server = Server(('127.0.0.1', 0), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
    def stop(self):
        self.server.shutdown()
        self.server.server_close()
    def url(self, path='/hook'):
        return 'http://127.0.0.1:%d%s' % (self.port, path)
    def snapshot(self):
        with self.lock:
            return list(self.records)
    def wait(self, count, timeout=4):
        deadline = time.time() + timeout
        while time.time() < deadline:
            records = self.snapshot()
            if len(records) >= count: return records
            time.sleep(.02)
        return self.snapshot()

class WebHookTests(unittest.TestCase):
    def setUp(self):
        self.recorder = Recorder()
        read, self.write = os.pipe()
        env = dict(os.environ, SATORI_TEST_EVENT_FD=str(read))
        self.proc = subprocess.Popen([wire.BINARY], stdin=subprocess.PIPE, stdout=subprocess.PIPE, pass_fds=(read,), env=env)
        os.close(read)
        self.proc.stdin.write(('token=' + TOKEN + '\n').encode())
        self.proc.stdin.close()
        wire.PORT = int(self.proc.stdout.readline())

    def tearDown(self):
        os.close(self.write)
        self.proc.terminate()
        self.proc.wait(timeout=3)
        self.proc.stdout.close()
        self.recorder.stop()

    def api(self, name, body):
        return wire.ServerTests.http(self, '/v1/meta/' + name, json.dumps(body).encode())

    def publish(self, body, meta=False):
        data = (('M' if meta else 'E') + json.dumps(body, ensure_ascii=False) + '\n').encode()
        os.write(self.write, data)

    def login(self):
        return {'type': 'login-added', 'login': {'sn': 1, 'status': 1, 'adapter': 'hook-test',
                'platform': 'wechat', 'user': {'id': 'hook-user'}, 'features': []}}

    def message(self):
        return {'type': 'message-created', 'login': {'sn': 1},
                'message': {'id': 'm1', 'content': '你好'}, 'channel': {'id': 'c', 'type': 0}, 'user': {'id': 'u'}}

    def test_delivery_and_management(self):
        status, capabilities = wire.ServerTests.http(self, '/v1/internal/capabilities')
        self.assertEqual(status, 200)
        self.assertTrue(capabilities['webhook'])
        self.assertEqual(capabilities['webhooks'], 0)
        # Invalid registrations are rejected, including https (no TLS client here).
        self.assertEqual(self.api('webhook.create', {'url': 'not-a-url'})[0], 400)
        self.assertEqual(self.api('webhook.create', {'url': 'https://example.invalid/hook'})[0], 400)
        self.assertEqual(self.api('webhook.create', {'url': 42})[0], 400)
        self.assertEqual(self.api('webhook.delete', {'url': self.recorder.url()})[0], 400)
        # Authenticated registration succeeds.
        self.assertEqual(self.api('webhook.create', {'url': self.recorder.url(), 'token': 'secret'})[0], 200)
        self.assertEqual(wire.ServerTests.http(self, '/v1/internal/capabilities')[1]['webhooks'], 1)
        # Login and message events are pushed with Satori-Opcode: 0.
        self.publish(self.login())
        self.publish(self.message())
        records = self.recorder.wait(2)
        self.assertEqual(len(records), 2)
        self.assertTrue(all(r['opcode'] == '0' for r in records), records)
        self.assertTrue(all(r['auth'] == 'Bearer secret' for r in records), records)
        self.assertTrue(all(r['path'] == '/hook' for r in records), records)
        self.assertEqual(json.loads(records[0]['body'])['type'], 'login-added')
        message = json.loads(records[1]['body'])
        self.assertEqual(message['type'], 'message-created')
        self.assertEqual(message['message']['content'], '你好')
        self.assertIn('sn', message)
        # A meta signal is pushed with Satori-Opcode: 5 and the Meta body.
        self.publish({'proxy_urls': []}, meta=True)
        records = self.recorder.wait(3)
        self.assertEqual(records[2]['opcode'], '5')
        self.assertEqual(json.loads(records[2]['body']), {'proxy_urls': []})
        # Deletion stops delivery.
        self.assertEqual(self.api('webhook.delete', {'url': self.recorder.url()})[0], 200)
        self.assertEqual(wire.ServerTests.http(self, '/v1/internal/capabilities')[1]['webhooks'], 0)
        before = len(self.recorder.snapshot())
        self.publish(self.message())
        time.sleep(.4)
        self.assertEqual(len(self.recorder.snapshot()), before)
        self.assertEqual(self.api('webhook.delete', {'url': self.recorder.url()})[0], 400)

    def test_limit_and_duplicates(self):
        for i in range(4):
            self.assertEqual(self.api('webhook.create', {'url': self.recorder.url('/h%d' % i)})[0], 200)
        self.assertEqual(wire.ServerTests.http(self, '/v1/internal/capabilities')[1]['webhooks'], 4)
        self.assertEqual(self.api('webhook.create', {'url': self.recorder.url('/overflow')})[0], 400)
        # Re-registering an existing URL replaces it instead of growing the list.
        self.assertEqual(self.api('webhook.create', {'url': self.recorder.url('/h0'), 'token': 'again'})[0], 200)
        self.assertEqual(wire.ServerTests.http(self, '/v1/internal/capabilities')[1]['webhooks'], 4)
        self.assertEqual(self.api('webhook.delete', {'url': self.recorder.url('/h0')})[0], 200)
        self.assertEqual(wire.ServerTests.http(self, '/v1/internal/capabilities')[1]['webhooks'], 3)

    def test_disabled_without_registration(self):
        self.publish(self.login())
        self.publish(self.message())
        time.sleep(.3)
        self.assertEqual(self.recorder.snapshot(), [])

if __name__ == '__main__':
    unittest.main()
