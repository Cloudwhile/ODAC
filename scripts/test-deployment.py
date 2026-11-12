#!/usr/bin/env python3
"""Host-only deployment shell integration tests; no router or host /etc mutation."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parent.parent
LIBEXEC = PROJECT / 'device-access/files/usr/libexec'

# One stub serves every external command. "uci -c DIR set|add_list" appends
# to DIR/<package>, so staged values reach /etc/config only through the
# script's own file replacement, and a byte-for-byte restore is observable.
STUB = r'''#!/bin/sh
set -eu
name=${0##*/}
printf '%s %s\n' "$name" "$*" >> "$ROOT/events"
case "$name" in
 id) echo 0;;
 uci)
  dir=''
  while [ $# -gt 0 ]; do
   case "$1" in -c) dir=$2; shift 2;; -*) shift;; *) break;; esac
  done
  cmd=$1; shift
  arg=${1:-}
  case "$cmd" in
   get) case "$arg" in
     odac.main.interface) echo br-odac;;
     odac.main.gateway) echo 192.168.30.1;;
     odac.main.portal_port) echo 2080;;
     odac.main.portal_https_port) echo 8443;;
     odac.main.portal_hostname) echo portal.example;;
     odac.main.zone) echo odac;;
     odac.main.network) echo odac;;
     odac.main.tls_cert) echo /etc/odac/cert.pem;;
     odac.main.tls_key) echo /etc/odac/key.pem;;
     network.wan6.proto) if [ -f "$ROOT/wan6" ]; then cat "$ROOT/wan6"; else echo dhcpv6; fi;;
     firewall.odac_guard) [ "${PRESET_SECTION:-0}" = 1 ] && echo include || exit 1;;
     *) exit 1;;
    esac;;
   set|add_list) [ -z "$dir" ] || printf '%s\n' "$arg" >> "$dir/${arg%%.*}";;
   *) :;;
  esac;;
 odac) if [ "$1" = start ] && [ "${CHANGE_WAN6:-0}" = 1 ]; then echo none > "$ROOT/wan6"; fi;;
 openssl)
  case "$1" in
   s_client) [ "${FAIL_TLS:-0}" = 0 ] && echo served-certificate || exit 1;;
   x509) case " $* " in
     *" -in "*) echo fingerprint;;
     *) read -r served && [ "$served" = served-certificate ] && echo fingerprint;;
    esac;;
   *) echo random-key;;
  esac;;
 odacctl) [ "$1" = --wait-ready ]; echo '{"ok":true,"data":{}}';;
 jsonfilter) echo true;;
 curl) [ "${FAIL_TLS:-0}" = 0 ] || exit 7;;
 fw4)
  if [ "$1" = check ] && [ "${FAIL_TLS:-0}" = 0 ]; then
   [ ! -e "$ROOT/etc/nftables.d/90-odac-retired.nft" ]
  fi
  if [ "$1" = reload ] && [ "${FAIL_ROLLBACK_RELOAD:-0}" = 1 ]; then
   [ "$(grep -c '^fw4 reload' "$ROOT/events")" -lt 2 ]
  fi;;
esac
'''

RETIRED = 'retired-original\n'
OPENNDS = "config opennds\n\toption odac_backend '1'\n\toption enabled '0'\n"


class Deployment(unittest.TestCase):
    def enable(self, state='', **env_flags):
        """Run odac-setup --enable in a sandbox and return (result, root)."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        for name in ('etc/config', 'etc/odac', 'etc/init.d', 'etc/nftables.d',
                     'var/lock', 'usr/libexec', 'usr/bin', 'bin'):
            (root / name).mkdir(parents=True, exist_ok=True)
        for name in ('odac', 'firewall'):
            (root / 'etc/config' / name).write_text('original-' + name + '\n')
        (root / 'etc/config/opennds').write_text(OPENNDS)
        (root / 'etc/nftables.d/90-odac-retired.nft').write_text(RETIRED)
        if state:
            (root / 'etc/odac/ownership').write_text(state + '\n')
        for name in ('odac-setup', 'odac-ownership'):
            text = (LIBEXEC / name).read_text()
            for prefix in ('/etc/', '/var/', '/usr/'):
                text = text.replace(prefix, str(root) + prefix)
            target = root / 'usr/libexec' / name
            target.write_text(text)
            target.chmod(0o755)
        for name in ('id', 'uci', 'openssl', 'jsonfilter', 'curl', 'fw4', 'ndsctl', 'nft'):
            (root / 'bin' / name).write_text(STUB)
            (root / 'bin' / name).chmod(0o755)
        for relative in ('usr/libexec/odac-preflight', 'usr/libexec/odac-firewall',
                         'usr/libexec/odac-config', 'usr/bin/odacctl',
                         'etc/init.d/odac', 'etc/init.d/opennds'):
            (root / relative).write_text(STUB)
            (root / relative).chmod(0o755)
        env = dict(os.environ, ROOT=str(root), ODAC_OWNERSHIP_DIR=str(root / 'etc/odac'),
                   PATH=str(root / 'bin') + ':' + os.environ['PATH'],
                   **{key: str(value) for key, value in env_flags.items()})
        result = subprocess.run(['sh', str(root / 'usr/libexec/odac-setup'), '--enable'],
                                env=env, capture_output=True, text=True, timeout=10)
        events = (root / 'events').read_text() if (root / 'events').exists() else ''
        self.assertFalse((root / 'var/lock/odac-setup.lock').exists(), 'lifecycle lock leaked')
        return result, root, events

    @staticmethod
    def ownership(root):
        path = root / 'etc/odac/ownership'
        return path.read_text() if path.exists() else ''

    def assert_rolled_back(self, result, root, events):
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn('odac-firewall close br-odac', events)
        # The opennds file is ODAC-owned and restored byte for byte, and the
        # durable retired guard is put back.
        self.assertEqual((root / 'etc/config/opennds').read_text(), OPENNDS)
        self.assertEqual((root / 'etc/nftables.d/90-odac-retired.nft').read_text(), RETIRED)
        self.assertIn('uci set odac.main.enabled=0', events)
        self.assertIn('rolled back; controlled network stays closed', output)
        state = self.ownership(root)
        self.assertIn('opennds_managed 0', state)
        self.assertNotIn('enable_pending', state)
        self.assertNotIn('firewall_sections', state)
        self.assertNotIn('odac_active 1', state)
        self.assertFalse(list((root / 'etc/odac').glob('*.before-enable')))
        self.assertFalse(list((root / 'etc/odac').glob('stage-*')))

    def test_enable_records_ownership_before_mutation_and_succeeds(self):
        result, root, events = self.enable()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('odac-preflight --setup', events)
        self.assertIn('allow_preemptive_authentication=1', events)
        for option in ('fwhook_enabled=0', 'users_to_router_passthrough=1',
                       'gatewayfqdn=disable', 'dhcp_default_url_enable=0'):
            self.assertIn(option, events)
        self.assertIn('odacctl --wait-ready', events)
        self.assertLess(events.index('odac-firewall init'), events.index('odac start'))
        self.assertFalse((root / 'etc/nftables.d/90-odac-retired.nft').exists())
        # Staged values reached /etc/config only through the file swap.
        self.assertIn('opennds.@opennds[0].enabled=1', (root / 'etc/config/opennds').read_text())
        state = self.ownership(root)
        for record in ('iface br-odac', 'guard 1', 'opennds_managed 1', 'odac_active 1',
                       'firewall_sections odac_guard odac_dns odac_dhcp odac_http odac_https odac_nds'):
            self.assertIn(record, state)
        self.assertIn('firewall_sha256_odac_guard ', state)
        self.assertNotIn('enable_pending', state)
        self.assertFalse(list((root / 'etc/odac').glob('*.before-enable')))

    def test_tls_failure_restores_retired_guard_and_configuration(self):
        self.assert_rolled_back(*self.enable(FAIL_TLS=1))

    def test_wan6_change_during_activation_is_rolled_back(self):
        result, root, events = self.enable(CHANGE_WAN6=1)
        self.assertIn('network.wan6.proto changed', result.stderr)
        self.assert_rolled_back(result, root, events)

    def test_incomplete_rollback_keeps_ownership_for_disable(self):
        result, root, events = self.enable(FAIL_TLS=1, FAIL_ROLLBACK_RELOAD=1)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('rollback incomplete; ownership kept', result.stderr)
        state = self.ownership(root)
        self.assertIn('enable_pending 1', state)
        self.assertIn('opennds_managed 1', state)
        self.assertIn('firewall_sections', state)

    def test_unrecorded_reserved_section_is_refused_before_any_change(self):
        result, root, events = self.enable(PRESET_SECTION=1)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('firewall.odac_guard already exists', result.stderr)
        self.assertNotIn('odac-firewall', events)
        self.assertEqual(self.ownership(root), '')
        self.assertEqual((root / 'etc/nftables.d/90-odac-retired.nft').read_text(), RETIRED)

    def test_interrupted_activation_is_refused(self):
        for record in ('enable_pending 1', 'odac_active 1', 'opennds_managed 1',
                       'firewall_sections odac_guard'):
            with self.subTest(record=record):
                result, root, events = self.enable(state=record)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('odac-setup --disable', result.stderr)
                self.assertNotIn('odac-firewall', events)
                self.assertEqual(self.ownership(root), record + '\n')
                self.assertEqual((root / 'etc/config/opennds').read_text(), OPENNDS)

    def test_retired_guard_preflight_rejects_other_device_and_symlink(self):
        source = (LIBEXEC / 'odac-preflight').read_text()
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
        source = (LIBEXEC / 'odac-preflight').read_text()
        preemptive = source[source.index('preemptive=$('):source.index('help=$(ndsctl')]
        ports = source[source.index('[ "$http" != "$https" ]'):source.index('host=$(get portal_hostname)')]
        for value, expected in (('', 0), ('1', 0), ('0', 1), ('bogus', 1)):
            script = ('set -eu\nfail() { exit 1; }; uci() { [ -n "$VALUE" ] && printf "%s" "$VALUE"; };\n' + preemptive)
            result = subprocess.run(['sh', '-c', script], env=dict(os.environ, VALUE=value), timeout=5)
            self.assertEqual(result.returncode, expected)
        for http, https, expected in ((2080, 8443, 0), (2050, 8443, 1), (2080, 2050, 1), (2080, 2080, 1)):
            script = f'set -eu\nfail() {{ exit 1; }}; http={http}; https={https}\n' + ports
            self.assertEqual(subprocess.run(['sh', '-c', script], timeout=5).returncode, expected)


if __name__ == '__main__':
    unittest.main()
