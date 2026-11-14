#!/usr/bin/env python3
"""Run the job wrapper in a sandbox with mocked jshn/procd/helper boundaries."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / 'device-access/files/usr/libexec/odac-config-job').read_text()


class Jobs(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='odac-job-test-')
        self.base = Path(self.temp.name)
        self.root = self.base / 'jobs'
        self.bin = self.base / 'bin'
        self.bin.mkdir()
        self.env = dict(os.environ, SANDBOX=str(self.base), PYTHON=os.sys.executable)
        parser = self.base / 'json.py'
        parser.write_text('''import json,sys
op,doc,*args=sys.argv[1:]
x=json.loads(doc)
if op=='load': print(doc)
else:
 key=args[0] if args else ''
 value=x.get(key) if isinstance(x,dict) else None
 if op=='select':
  if not isinstance(value,(dict,list)): sys.exit(1)
  print(json.dumps(value))
 elif op=='type':
  t={dict:'object',list:'array',str:'string',bool:'boolean',int:'int',float:'double'}.get(type(value),'')
  if not t: sys.exit(1)
  print(t)
 elif op=='keys': print(' '.join(x.keys()))
 elif op=='var': print(int(value) if isinstance(value,bool) else value)
''')
        library = self.base / 'jshn.sh'
        library.write_text('''json_load() { json_document=$1; "$PYTHON" -I "$SANDBOX/json.py" load "$1" >/dev/null 2>&1; }
json_select() { json_document=$("$PYTHON" -I "$SANDBOX/json.py" select "$json_document" "$1"); }
json_get_type() { value=$("$PYTHON" -I "$SANDBOX/json.py" type "$json_document" "$2") || return 1; eval "$1=\\\"\\$value\\\""; }
json_get_keys() { value=$("$PYTHON" -I "$SANDBOX/json.py" keys "$json_document"); eval "$1=\\\"\\$value\\\""; }
json_get_var() { value=$("$PYTHON" -I "$SANDBOX/json.py" var "$json_document" "$2"); eval "$1=\\\"\\$value\\\""; }
json_init() { :; }
json_add_string() { :; }
json_add_object() { :; }
json_add_array() { :; }
json_add_int() { :; }
json_add_boolean() { :; }
json_close_object() { :; }
json_close_array() { :; }
json_dump() { printf '{}'; }
''')
        self.executable('ubus', '''#!/bin/sh
case "$3" in
 set) printf 'set\\n' >> "$SANDBOX/submissions"; [ "${SET_FAIL:-0}" = 0 ];;
 list) [ "${LIST_FAIL:-0}" = 0 ] || exit 1; printf '%s\\n' "${SERVICE_STATE:-{}}";;
esac
''')
        self.executable('openssl', '''#!/bin/sh
printf '%s\\n' "$JOB_ID"
''')
        helper = self.base / 'helper'
        helper.write_text('''#!/bin/sh
printf '%s\\n' "$*" > "$SANDBOX/helper-args"
if [ "${HOLD:-0}" = 1 ]; then
 trap 'printf rollback > "$SANDBOX/rollback"; sleep 1; printf done > "$SANDBOX/rollback"; exit 1' TERM
 while :; do sleep 0.1; done
fi
if [ -n "$RESPONSE" ]; then printf '%s\\n' "$RESPONSE"; else printf '%s\\n' '{"ok":true}'; fi
exit "${HELPER_RC:-0}"
''')
        helper.chmod(0o700)
        source = SOURCE.replace('PATH=/usr/sbin:/usr/bin:/sbin:/bin', f'PATH="{self.bin}:$PATH"')
        source = source.replace('root=/var/run/odac-config-job', f'root="{self.root}"')
        source = source.replace('. /usr/share/libubox/jshn.sh', f'. "{library}"')
        source = source.replace('[ "$(id -u)" = 0 ]', '[ 0 = 0 ]')
        source = source.replace('[ "$(ls -ldn "$root" | awk \'{print $3}\')" = 0 ]', '[ 0 = 0 ]')
        source = source.replace('/usr/libexec/odac-network-config', str(helper))
        source = source.replace('/var/lock/odac-setup.lock', str(self.base / 'setup.lock'))
        source = source.replace('/tmp/odac-upload-', str(self.base / 'odac-upload-'))
        self.script = self.base / 'job'
        self.script.write_text(source)
        self.processes = []

    def tearDown(self):
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
        self.temp.cleanup()

    def executable(self, name, text):
        path = self.bin / name
        path.write_text(text)
        path.chmod(0o700)

    def run_job(self, method, payload=None, **env):
        result = subprocess.run(['sh', str(self.script), method], input=json.dumps(payload or {}),
                                text=True, capture_output=True, env=dict(self.env, **env), timeout=10)
        return result, json.loads(result.stdout)

    def submit(self, number=1, method='network_update', **env):
        ident = f'{number:032x}'
        result, data = self.run_job(method, JOB_ID=ident, **env)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(data['data']['job'], ident)
        return ident

    def worker(self, ident, method='network_update', **env):
        return subprocess.run(['sh', str(self.script), '--run', ident, method], text=True,
                              capture_output=True, env=dict(self.env, **env), timeout=10)

    def test_request_validation_and_concurrent_lock(self):
        raw = subprocess.run(['sh', str(self.script), 'network_update'], input='{},"escape":{}',
                             text=True, capture_output=True, env=self.env)
        self.assertFalse(json.loads(raw.stdout)['ok'])
        ident = self.submit()
        result, data = self.run_job('network_update', JOB_ID='2' * 32)
        self.assertEqual(data['error']['code'], 'BUSY')
        self.assertEqual((self.root / 'busy/id').read_text().strip(), ident)
        self.assertFalse((self.base / 'helper-args').exists())  # RPC never forks helper.

    def test_worker_result_and_retention(self):
        for number in range(1, 11):
            ident = self.submit(number)
            result = self.worker(ident)
            self.assertEqual(result.returncode, 0, result.stderr)
            _, status = self.run_job('status', {'job': ident})
            self.assertTrue(status['data']['done'])
            self.assertTrue(status['data']['result']['ok'])
        self.assertEqual(len(list(self.root.glob('*/result'))), 8)
        _, status = self.run_job('status', {'job': ident})
        self.assertTrue(status['data']['done'])

    def test_worker_failures_and_duplicate_start(self):
        ident = self.submit()
        result = self.worker(ident, RESPONSE='not JSON', HELPER_RC='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        _, status = self.run_job('status', {'job': ident})
        self.assertEqual(status['data']['result']['error']['code'], 'BAD_RESPONSE')
        self.assertNotEqual(self.worker(ident).returncode, 0)
        ident = self.submit(2)
        self.worker(ident, RESPONSE='{"ok":true}', HELPER_RC='1')
        _, status = self.run_job('status', {'job': ident})
        self.assertEqual(status['data']['result']['error']['code'], 'APPLY')

    def test_stale_lock_and_uncertain_procd(self):
        ident = self.submit()
        (self.root / ident / 'started').write_text(str(int(time.time()) - 60))
        _, status = self.run_job('status', {'job': ident}, LIST_FAIL='1')
        self.assertFalse(status['data']['done'])
        _, status = self.run_job('status', {'job': ident}, SERVICE_STATE='null')
        self.assertFalse(status['data']['done'])
        _, status = self.run_job('status', {'job': ident}, SERVICE_STATE=json.dumps({'odac-config-job': {'instances': {'worker': {'running': True}}}}))
        self.assertFalse(status['data']['done'])
        (self.base / 'setup.lock').mkdir()
        _, status = self.run_job('status', {'job': ident})
        self.assertFalse(status['data']['done'])
        (self.base / 'setup.lock').rmdir()
        _, status = self.run_job('status', {'job': ident})
        self.assertEqual(status['data']['result']['error']['code'], 'INTERRUPTED')
        self.assertFalse((self.root / 'busy').exists())

    def test_failed_start_cleanup_and_uncertain_reply(self):
        _, data = self.run_job('network_update', JOB_ID='a' * 32, SET_FAIL='1')
        self.assertEqual(data['error']['code'], 'UNAVAILABLE')
        self.assertFalse((self.root / 'busy').exists())
        self.assertFalse((self.root / ('a' * 32)).exists())
        ident = self.submit(2, SET_FAIL='1', LIST_FAIL='1')
        self.assertTrue((self.root / ident).exists())
        self.assertTrue((self.root / 'busy').exists())

    def test_capture_upload_pair_before_rpc_returns(self):
        for name in ('cert', 'key'):
            (self.base / f'odac-upload-{name}.pem').write_text('original-' + name)
        ident = self.submit(method='upload_cert')
        for name in ('cert', 'key'):
            self.assertEqual((self.root / ident / 'upload' / f'{name}.pem').read_text(), 'original-' + name)
            (self.base / f'odac-upload-{name}.pem').write_text('replacement-' + name)
        self.worker(ident, 'upload_cert')
        self.assertEqual((self.base / 'helper-args').read_text().strip(), f'upload_cert --upload-dir {self.root / ident / "upload"}')
        self.assertEqual((self.base / 'odac-upload-key.pem').read_text(), 'replacement-key')

    def test_term_waits_for_helper_rollback(self):
        ident = self.submit()
        process = subprocess.Popen(['sh', str(self.script), '--run', ident, 'network_update'],
                                   env=dict(self.env, HOLD='1'), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.processes.append(process)
        deadline = time.monotonic() + 5
        while not (self.base / 'helper-args').exists():
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.02)
        process.terminate()
        time.sleep(0.2)
        self.assertTrue((self.root / 'busy').exists())
        self.assertFalse((self.root / ident / 'result').exists())
        process.communicate(timeout=5)
        self.assertEqual((self.base / 'rollback').read_text(), 'done')
        self.assertFalse((self.root / 'busy').exists())
        _, status = self.run_job('status', {'job': ident})
        self.assertEqual(status['data']['result']['error']['code'], 'BAD_RESPONSE')


if __name__ == '__main__':
    unittest.main()
