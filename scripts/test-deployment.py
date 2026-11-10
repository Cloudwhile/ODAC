#!/usr/bin/env python3
"""Host-only deployment shell integration tests; no router or host /etc mutation."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parent.parent


class Deployment(unittest.TestCase):
    def exercise(self, fail_tls):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('etc/config', 'etc/odac', 'etc/init.d', 'etc/nftables.d',
                         'var/lock', 'usr/libexec', 'usr/bin', 'bin'):
                (root / name).mkdir(parents=True, exist_ok=True)
            for name in ('odac', 'opennds', 'uhttpd', 'firewall'):
                (root / 'etc/config' / name).write_text('original-' + name)
            retired = root / 'etc/nftables.d/90-odac-retired.nft'
            retired.write_text('retired-original\n')
            for name in ('odac-setup', 'odac-remove'):
                text = (PROJECT / 'device-access/files/usr/libexec' / name).read_text()
                for prefix in ('/etc/', '/var/', '/usr/'):
                    text = text.replace(prefix, str(root) + prefix)
                target = root / 'usr/libexec' / name
                target.write_text(text)
                target.chmod(0o755)
            stub = r'''#!/bin/sh
set -eu
name=${0##*/}
printf '%s %s\n' "$name" "$*" >> "$ROOT/events"
case "$name" in
 id) echo 0;;
 uci)
  case "$*" in
   *get\ odac.main.interface) echo br-odac;;
   *get\ odac.main.gateway) echo 192.168.30.1;;
   *get\ odac.main.portal_port) echo 2080;;
   *get\ odac.main.portal_https_port) echo 8443;;
   *get\ odac.main.portal_hostname) echo portal.example;;
   *get\ odac.main.zone) echo odac;;
   *get\ odac.main.enabled) echo 1;;
  esac;;
 openssl)
  case "$1" in
   s_client) [ "$FAIL_TLS" = 0 ] && echo served-certificate || exit 1;;
   x509) case " $* " in
     *" -in "*) echo fingerprint;;
     *) read -r served && [ "$served" = served-certificate ] && echo fingerprint;;
    esac;;
   *) echo random-key;;
  esac;;
 odacctl) [ "$1" = --wait-ready ]; echo '{"ok":true,"data":{}}';;
 jsonfilter) echo true;;
 curl) [ "$FAIL_TLS" = 0 ] || exit 7;;
 fw4) if [ "$1" = check ] && [ "$FAIL_TLS" = 0 ]; then
   [ ! -e "$ROOT/etc/nftables.d/90-odac-retired.nft" ]; fi;;
esac
'''
            for name in ('id', 'uci', 'openssl', 'jsonfilter', 'curl', 'fw4', 'ndsctl', 'nft'):
                target = root / 'bin' / name
                target.write_text(stub)
                target.chmod(0o755)
            for relative in ('usr/libexec/odac-preflight', 'usr/libexec/odac-firewall',
                             'usr/libexec/odac-config', 'usr/bin/odacctl',
                             'etc/init.d/odac', 'etc/init.d/opennds'):
                target = root / relative
                target.write_text(stub)
                target.chmod(0o755)
            env = dict(os.environ, ROOT=str(root), FAIL_TLS=str(int(fail_tls)),
                       PATH=str(root / 'bin') + ':' + os.environ['PATH'])
            result = subprocess.run(['sh', str(root / 'usr/libexec/odac-setup'), '--enable'],
                                    env=env, capture_output=True, text=True, timeout=10)
            self.assertTrue((root / 'events').exists(), result.stdout + result.stderr)
            events = (root / 'events').read_text()
            self.assertIn('odac-preflight --setup', events)
            self.assertIn('allow_preemptive_authentication=1', events)
            self.assertIn('odacctl --wait-ready', events)
            self.assertLess(events.index('odac-firewall init'), events.index('odac start'))
            self.assertFalse((root / 'var/lock/odac-setup.lock').exists())
            if fail_tls:
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(retired.read_text(), 'retired-original\n')
                self.assertEqual((root / 'etc/config/opennds').read_text(), 'original-opennds')
                self.assertIn('odac-firewall close', events)
                self.assertTrue(list((root / 'etc/odac').glob('removal-*')))
            else:
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(retired.exists())
                backup = Path((root / 'etc/odac/last-setup').read_text().strip())
                self.assertEqual((backup / 'retired.nft').read_text(), 'retired-original\n')
                self.assertEqual((backup / 'opennds').read_text(), 'original-opennds')

    def test_retired_guard_preflight_rejects_other_device_and_symlink(self):
        source = (PROJECT / 'device-access/files/usr/libexec/odac-preflight').read_text()
        block = source[source.index('retired=/etc/'):source.index('# Arithmetic IPv4')]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            retired = root / 'retired.nft'
            valid = ('chain odac_retired_forward { type filter hook forward priority -10; policy accept; iifname "br-odac" drop; oifname "br-odac" drop; }\n'
                     'chain odac_retired_input { type filter hook input priority -10; policy accept; iifname "br-odac" drop; }\n')
            script = ('set -eu\nfail() { exit 1; }; owner_is_root() { :; }; iface=br-odac\n' +
                      block.replace('/etc/nftables.d/90-odac-retired.nft', str(retired)))
            for contents, expected in ((valid, 0), (valid.replace('br-odac', 'br-other'), 1)):
                retired.write_text(contents)
                result = subprocess.run(['sh', '-c', script], capture_output=True, timeout=5)
                self.assertEqual(result.returncode, expected, result.stderr)
            target = root / 'target'
            target.write_text(valid)
            retired.unlink()
            retired.symlink_to(target)
            self.assertNotEqual(subprocess.run(['sh', '-c', script], timeout=5).returncode, 0)

    def test_preemptive_default_and_gateway_port_contract(self):
        source = (PROJECT / 'device-access/files/usr/libexec/odac-preflight').read_text()
        preemptive = source[source.index('preemptive=$('):source.index('help=$(ndsctl')]
        ports = source[source.index('[ "$http" != "$https" ]'):source.index('host=$(get portal_hostname)')]
        for value, expected in (('', 0), ('1', 0), ('0', 1), ('bogus', 1)):
            script = ('set -eu\nfail() { exit 1; }; uci() { [ -n "$VALUE" ] && printf "%s" "$VALUE"; };\n' + preemptive)
            result = subprocess.run(['sh', '-c', script], env=dict(os.environ, VALUE=value), timeout=5)
            self.assertEqual(result.returncode, expected)
        for http, https, expected in ((2080, 8443, 0), (2050, 8443, 1), (2080, 2050, 1), (2080, 2080, 1)):
            script = f'set -eu\nfail() {{ exit 1; }}; http={http}; https={https}\n' + ports
            self.assertEqual(subprocess.run(['sh', '-c', script], timeout=5).returncode, expected)

    def test_reenable_replaces_retired_drop_after_guard(self):
        self.exercise(False)

    def test_tls_failure_restores_retired_drop_and_configuration(self):
        self.exercise(True)


if __name__ == '__main__':
    unittest.main()
