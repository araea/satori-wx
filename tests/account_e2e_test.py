#!/usr/bin/env python3
"""End-to-end adapter test: a fixture SharedPreferences directory drives the real
account adapter, and the resulting login is served over HTTP and WebSocket.
The production module has no test hook; this uses the standalone test server only."""
import importlib.util
import json
import os
import pathlib
import shutil
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

MAIN = """<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <string name="login_weixin_username">{wxid}</string>
    <string name="last_login_uin">{uin}</string>
    <boolean name="isLogin" value="{login}" />
    <string name="last_login_nick_name">{nick}</string>
</map>
"""

class AccountEndToEndTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix='satori-e2e-')
        os.makedirs(os.path.join(self.dir, 'shared_prefs'))
        self.write('wxid_e2e', '777', True, '测试昵称')
        self.proc = subprocess.Popen([wire.BINARY], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     env=dict(os.environ, SATORI_TEST_ACCOUNT_DIR=self.dir))
        self.proc.stdin.write(('token=' + TOKEN + '\n').encode())
        self.proc.stdin.close()
        wire.PORT = int(self.proc.stdout.readline())

    def tearDown(self):
        self.proc.terminate()
        self.proc.wait(timeout=3)
        self.proc.stdout.close()
        shutil.rmtree(self.dir, ignore_errors=True)

    def prefs_path(self):
        return os.path.join(self.dir, 'shared_prefs', 'com.tencent.mm_preferences.xml')

    def write(self, wxid, uin, login, nick):
        path = self.prefs_path()
        temp = path + '.tmp'
        with open(temp, 'w', encoding='utf-8') as handle:
            handle.write(MAIN.format(wxid=wxid, uin=uin, login='true' if login else 'false', nick=nick))
        os.replace(temp, path)  # Atomic so a scan never sees a half-written file.

    def meta(self):
        return wire.ServerTests.http(self, '/v1/meta')[1]

    def wait_logins(self, predicate, message):
        for _ in range(100):
            logins = self.meta().get('logins', [])
            if predicate(logins): return logins
            time.sleep(.05)
        self.fail(message)

    def login_get(self):
        headers = {'Satori-Platform': 'wechat', 'Satori-User-ID': 'wxid_e2e'}
        return wire.ServerTests.http(self, '/v1/login.get', headers=headers)

    def test_account_lifecycle(self):
        logins = self.wait_logins(lambda l: len(l) == 1, 'login did not appear')
        login = logins[0]
        self.assertEqual(login['user']['id'], 'wxid_e2e')
        self.assertEqual(login['platform'], 'wechat')
        self.assertEqual(login['status'], 1)
        self.assertEqual(login['user']['nick'], '测试昵称')
        self.assertEqual(login['features'], [])
        self.assertEqual(self.login_get(), (200, login))
        with Wire() as w:
            w.upgrade()
            _, ready = w.identify()
            self.assertEqual(json.loads(ready)['body']['logins'][0], login)
        # Logout keeps the login but marks it offline, which gates other APIs.
        self.write('wxid_e2e', '777', False, '测试昵称')
        logins = self.wait_logins(lambda l: l and l[0]['status'] == 0, 'logout not reflected')
        self.assertEqual(self.login_get()[1]['status'], 0)
        # A different account replaces the identity instead of overwriting it.
        self.write('wxid_other', '888', True, '另一个')
        logins = self.wait_logins(lambda l: l and l[0]['user']['id'] == 'wxid_other', 'switch not reflected')
        self.assertEqual(len(logins), 1)
        self.assertEqual(logins[0]['user']['nick'], '另一个')
        # Removing the identity removes the login entirely.
        with open(self.prefs_path(), 'w', encoding='utf-8') as handle:
            handle.write("<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map />\n")
        self.wait_logins(lambda l: not l, 'removal not reflected')
        self.assertEqual(self.login_get()[0], 403)

if __name__ == '__main__':
    unittest.main()
