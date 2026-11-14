#!/bin/sh
# Execute the real shell rollback and permission logic with service boundaries
# stubbed; no root, UCI installation, or mutation of the host is required.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
helper=$root/device-access/files/usr/libexec/odac-network-config
rpc=$root/device-access/files/usr/libexec/odac-rpc
preflight=$root/device-access/files/usr/libexec/odac-preflight
sh -n "$helper" "$rpc" "$preflight"
python3 - "$helper" "$rpc" "$preflight" <<'PY'
import os
import pathlib
import subprocess
import sys
import tempfile

helper, rpc, preflight = (pathlib.Path(p).read_text() for p in sys.argv[1:])
assert 'json_get_type root_type params' in helper and 'json_select params' in helper
assert helper.index('pending=$(uci changes)') < helper.index('trap rollback EXIT')
assert helper.index("fail INTERFACE") < helper.index('trap rollback EXIT')
assert 'command uci -t "$work/delta"' in helper
assert 'network_update' in rpc and 'upload_cert' in rpc
assert '--config' in preflight
assert helper.index('uci add_list "opennds.@opennds[0].users_to_router=allow tcp port $https"') < helper.index('/usr/libexec/odac-preflight >/dev/null')
assert 'curl --cacert "$cert" --resolve' in helper and '--noproxy' in helper
assert 'ndsctl json' in helper and 'odacctl --wait-ready' in helper
assert 'mv "$upload/cert.pem"' in helper and '[ "$links" = 1 ]' in helper
assert '--upload-dir' in helper and "'drwx------:0'" in helper
assert helper.index('cert=$work/upload-cert.pem') < helper.index('openssl x509 -in "$cert" -noout')

rollback = helper[helper.index('rollback() {'):helper.index('\ntrap rollback EXIT')]
assert 'uci commit' not in rollback
# Replace only external service boundaries, preserving the transaction logic.
rollback = rollback.replace('/usr/libexec/odac-config', 'mock_config')
rollback = rollback.replace('/etc/init.d/opennds', 'mock_opennds')
rollback = rollback.replace('/etc/init.d/odac', 'mock_odac')
rollback = rollback.replace('/usr/libexec/odac-firewall', 'mock_guard')
prelude = r'''
work=/snapshot; incoming=''; lock=/lock
changed=1; active=1; new_cert=''; old_iface=br-odac
close_old() { printf 'closed\n' >> "$LOG"; return 0; }
cleanup() { printf 'cleanup\n' >> "$LOG"; }
uci() { printf 'uci %s\n' "$*" >> "$LOG"; [ "$SCENARIO" != revert ]; }
cp() { printf 'restore %s\n' "$2" >> "$LOG"; [ "$SCENARIO" != copy ]; }
fw4() { printf 'fw4 %s\n' "$*" >> "$LOG"; [ "$SCENARIO" != firewall ]; }
mock_config() { printf 'config\n' >> "$LOG"; [ "$SCENARIO" != config ]; }
mock_opennds() { printf 'opennds %s\n' "$*" >> "$LOG"; [ "$SCENARIO" != opennds ]; }
mock_odac() { printf 'odac %s\n' "$*" >> "$LOG"; [ "$SCENARIO" != reload ]; }
mock_guard() { printf 'closed\n' >> "$LOG"; }
accept_runtime() { printf 'probe\n' >> "$LOG"; [ "$SCENARIO" != probe ]; }
'''
with tempfile.TemporaryDirectory(prefix='odac-network-tests-') as tmp:
    log = pathlib.Path(tmp) / 'calls'
    for scenario in ('success', 'copy', 'revert', 'firewall', 'config', 'opennds', 'reload', 'probe'):
        log.write_text('')
        env = dict(os.environ, LOG=str(log), SCENARIO=scenario)
        result = subprocess.run(['sh'], input=prelude + rollback + '\nfalse\nrollback\n', text=True, capture_output=True, env=env)
        assert result.returncode == 1, (scenario, result)
        calls = log.read_text().splitlines()
        assert calls[0] == 'closed', (scenario, calls)
        assert calls[-1] == 'cleanup'
        assert all('commit' not in call for call in calls)
        if scenario in ('copy', 'revert', 'firewall'):
            assert 'odac reload' not in calls and 'opennds restart' not in calls, (scenario, calls)
        if scenario != 'success':
            assert calls[-2] == 'closed', (scenario, calls)
            assert 'rollback apply failed' in result.stderr, (scenario, result.stderr)
        else:
            assert calls.count('closed') == 1 and 'probe' in calls, calls
    # A rejected validation/request must not restart services or restore files.
    log.write_text('')
    result = subprocess.run(['sh'], input=prelude + '\nchanged=0\n' + rollback + '\nfalse\nrollback\n', text=True, capture_output=True, env=env)
    assert log.read_text().splitlines() == ['cleanup']

# Even a failed service stop must still attempt to install a closed guard.
close_old = helper[helper.index('close_old() {'):helper.index('\naccept_runtime() {')]
close_old = close_old.replace('/etc/init.d/odac', 'mock_odac').replace('/usr/libexec/odac-firewall', 'mock_guard')
with tempfile.TemporaryDirectory(prefix='odac-close-tests-') as tmp:
    log = pathlib.Path(tmp) / 'calls'
    for failed in ('none', 'stop', 'daemon', 'guard'):
        log.write_text('')
        script = r'''
old_iface=br-odac; old_gateway=192.168.46.1; old_http=2080; old_https=8443; old_upstream=''
mock_odac() { printf 'stop\n' >> "$LOG"; [ "$FAILED" != stop ]; }
pidof() { [ "$FAILED" = daemon ]; }
mock_guard() { printf 'init\n' >> "$LOG"; [ "$FAILED" != guard ]; }
''' + close_old + '\nclose_old\n'
        result = subprocess.run(['sh'], input=script, text=True, capture_output=True, env=dict(os.environ, LOG=str(log), FAILED=failed))
        assert log.read_text().splitlines() == ['stop', 'init'], failed
        assert (result.returncode == 0) == (failed == 'none'), failed

# Execute the actual mode case, including owner-write acceptance and rejection
# of group/other write, execute, setgid, sticky, and non-regular types.
mode_case = next(line.strip() for line in helper.splitlines() if 'case "$mode" in' in line)
for mode, allowed in {'-rw-------': True, '-r--------': True, '-rw-r--r--': True,
                      '-rw-rw----': False, '-rw-----w-': False, '-rw-r-x---': False,
                      '-rw------x': False, '-rw-r-S---': False, '-rw------T': False,
                      'lrwxrwxrwx': False}.items():
    result = subprocess.run(['sh'], input='fail() { exit 1; }; mode=' + mode + '\n' + mode_case + '\n', text=True, capture_output=True)
    assert (result.returncode == 0) == allowed, mode

# Run the actual old-pair cleanup against a temporary store, including traversal
# and outside-store cases. Only an exact generated pair can be reclaimed.
start = helper.index('if [ "$method" = upload_cert ]; then\n old_dir=')
cleanup_cert = helper[start:helper.index('\ntrap cleanup EXIT', start)]
with tempfile.TemporaryDirectory(prefix='odac-cert-tests-') as tmp:
    store = pathlib.Path(tmp) / 'certs'
    store.mkdir()
    external = pathlib.Path(tmp) / 'external'
    external.mkdir()
    good = store / ('a' * 32)
    for relative, remove in [('a' * 32, True), ('../external', False), ('custom', False)]:
        directory = store / relative
        directory.mkdir(exist_ok=True)
        cert = directory / 'cert.pem'
        key = directory / 'key.pem'
        cert.write_text('certificate'); key.write_text('key')
        env = dict(os.environ, method='upload_cert', old_cert=str(cert), old_key=str(key), new_cert=str(store / 'new'))
        script = cleanup_cert.replace('/etc/odac/certs/', str(store) + '/')
        result = subprocess.run(['sh'], input=script, text=True, capture_output=True, env=env)
        assert result.returncode == 0, result.stderr
        assert directory.exists() != remove, relative
print('network configuration contracts and rollback/permission/cleanup behavior passed')
PY
