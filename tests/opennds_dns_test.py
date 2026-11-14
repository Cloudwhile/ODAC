#!/usr/bin/env python3
"""Exercise the ODAC openNDS DNS policy checker with a stub UCI library."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CHECKER = ROOT / 'odac-opennds/files/usr/libexec/odac-opennds-check'

LIB = r'''config_load() { [ "$1" = opennds ] && [ "${LOAD_ERROR:-0}" = 0 ]; }
config_foreach() {
    local callback="$1" section
    for section in $SECTIONS; do "$callback" "$section"; done
}
config_get() {
    if [ "$3" = TYPE ]; then
        case "$2" in policy) eval "$1=policy";; *) eval "$1=opennds";; esac
    else
        eval "$1=\"\${CFG_${3}:-}\""
    fi
}
'''


class OpenNdsDnsPolicyTest(unittest.TestCase):
    def run_policy(self, options, sections='opennds', load_error=False):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            library = folder / 'functions.sh'
            library.write_text(LIB)
            script = folder / 'checker'
            script.write_text(CHECKER.read_text().replace(
                '. /lib/functions.sh', f'. "{library}"'))
            script.chmod(0o755)
            env = {**os.environ, 'SECTIONS': sections,
                   'CFG_odac_backend': '1', 'CFG_gatewayfqdn': 'disable',
                   'CFG_dhcp_default_url_enable': '0',
                   'LOAD_ERROR': '1' if load_error else '0', **{
                       f'CFG_{key}': value for key, value in options.items()}}
            return subprocess.run([str(script)], env=env, text=True,
                                  capture_output=True)

    def test_odac_administrator_managed_dns_configuration_is_accepted(self):
        self.assertEqual(self.run_policy({}).returncode, 0)

    def test_shared_dns_and_dhcp_features_are_rejected(self):
        for option, value, message in [
            ('gatewayfqdn', 'portal.example.test', 'gatewayfqdn must be disabled'),
            ('dhcp_default_url_enable', '1', 'option 114 management is unsupported'),
            ('walledgarden_fqdn_list', 'example.test', 'requires shared DNS changes'),
            ('walledgarden_port_list', '80', 'requires shared DNS changes'),
            ('blocklist_fqdn_list', 'bad.example.test', 'requires shared DNS changes'),
            ('blocklist_port_list', '443', 'requires shared DNS changes'),
        ]:
            with self.subTest(option=option):
                result = self.run_policy({option: value})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)

    def test_foreign_or_ambiguous_configuration_is_rejected(self):
        result = self.run_policy({'odac_backend': '0'})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('not owned by ODAC', result.stderr)
        result = self.run_policy({}, sections='opennds opennds2')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('exactly one', result.stderr)

    def test_policy_hidden_in_other_section_type_is_rejected(self):
        result = self.run_policy({}, sections='opennds policy')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('other section types may hide DNS policy', result.stderr)

    def test_missing_or_unreadable_configuration_is_rejected(self):
        result = self.run_policy({}, sections='')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('exactly one', result.stderr)
        result = self.run_policy({}, load_error=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('cannot read', result.stderr)
        for field in ('odac_backend', 'gatewayfqdn', 'dhcp_default_url_enable'):
            with self.subTest(field=field):
                self.assertNotEqual(self.run_policy({field: ''}).returncode, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
