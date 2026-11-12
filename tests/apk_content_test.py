#!/usr/bin/env python3
"""Regression tests for tree-shaped APK v3 listings and unsafe replacements."""
import copy
import hashlib
import importlib.util
from pathlib import Path
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    'apk_content', ROOT / 'scripts/check-apk-content.py')
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)

# Same directory/file nesting as apk adbdump; the init path is never a single
# string in the dump. Hashes are filled from source to allow deliberate edits.
NDS = '''#%SCHEMA: 676B6370
info:
  name: odac-opennds
  version: 10.3.1-r1
paths: # 4 items
  - acl:
      mode: 0755
  - name: etc/config
    files: # 1 items
      - name: opennds
        acl:
          mode: 0600
          user: root
          group: root
        hash: CONFIG_HASH
  - name: etc/init.d
    files: # 1 items
      - name: opennds
        acl:
          mode: 0755
          user: root
          group: root
        hash: INIT_HASH
  - name: usr/bin
    files: # 2 items
      - name: opennds
      - name: ndsctl
  - name: usr/libexec
    files:
      - name: odac-opennds-check
        acl:
          mode: 0755
          user: root
          group: root
        hash: DNS_CHECK_HASH
scripts:
  post-install: |
    #!/bin/sh
    default_postinst
'''


class ContentTest(unittest.TestCase):
    def setUp(self):
        text = NDS
        for token, path in [('CONFIG_HASH', 'etc/config/opennds'),
                            ('INIT_HASH', 'etc/init.d/opennds'),
                            ('DNS_CHECK_HASH', 'usr/libexec/odac-opennds-check')]:
            digest = hashlib.sha256(
                (ROOT / 'odac-opennds/files' / path).read_bytes()).hexdigest()
            text = text.replace(token, digest)
        self.text = text
        self.data = yaml.load(text, Loader=yaml.BaseLoader)

    def check_data(self, data):
        return checker.check('odac-opennds', yaml.safe_dump(data), ROOT)

    def test_actual_tree_shape_and_source_hashes(self):
        self.assertNotIn('etc/init.d/opennds', self.text)
        self.assertEqual(checker.check('odac-opennds', self.text, ROOT), [])
        missing = copy.deepcopy(self.data)
        missing['paths'] = [path for path in missing['paths']
                            if path.get('name') != 'usr/libexec']
        self.assertTrue(any('does not ship usr/libexec/odac-opennds-check' in error
                            for error in self.check_data(missing)))

    def test_backend_requires_dns_isolated_admission_version(self):
        data = {'info': {'name': 'device-access',
                         'depends': ['odac-opennds>=10.3.1-r2']},
                'paths': [{'files': [{'name': 'fixture'}]}]}
        def dependency_errors():
            return [message for message in checker.check(
                'device-access', yaml.safe_dump(data), ROOT)
                if 'must require' in message]
        self.assertEqual(dependency_errors(), [])
        for depends in ([], ['odac-opennds'], ['odac-opennds>=10.3.1-r1']):
            data['info']['depends'] = depends
            self.assertTrue(dependency_errors())

    def test_library_package_has_lib_prefix(self):
        text = '''info:
  name: libargon2
paths:
  - name: usr/lib
    files:
      - name: libargon2.so.1
'''
        self.assertEqual(checker.check('libargon2', text, ROOT), [])
        self.assertTrue(checker.check('argon2', text, ROOT))
        self.assertTrue(checker.check('libargon2', text.replace(
            'name: libargon2.so.1', 'name: argon2.so.1'), ROOT))

    def test_empty_unknown_and_mismatched_packages(self):
        for text in ['', 'info: {}', 'info: {name: odac-opennds}\npaths: []']:
            with self.subTest(text=text), self.assertRaises(ValueError):
                checker.check('odac-opennds', text, ROOT)
        self.assertTrue(checker.check('unknown', self.text, ROOT))
        self.data['info']['name'] = 'opennds'
        self.assertTrue(self.check_data(self.data))

    def test_directory_name_is_not_a_file(self):
        self.data['paths'][2] = {'name': 'etc/init.d/opennds'}
        self.assertTrue(any('does not ship etc/init.d/opennds' in error
                            for error in self.check_data(self.data)))

    def test_stock_install_and_restart_hooks_rejected(self):
        for parent, leaf in [('etc/uci-defaults', '40_opennds'),
                             ('usr/lib/opennds', 'restart.sh')]:
            with self.subTest(path=f'{parent}/{leaf}'):
                data = copy.deepcopy(self.data)
                data['paths'].append({'name': parent, 'files': [{'name': leaf}]})
                self.assertTrue(self.check_data(data))

    def test_same_path_with_changed_content_rejected(self):
        for index in (1, 2, 4):
            with self.subTest(index=index):
                data = copy.deepcopy(self.data)
                data['paths'][index]['files'][0]['hash'] = '0' * 64
                self.assertTrue(self.check_data(data))

    def test_wrong_permissions_owner_and_symlink_rejected(self):
        for field, value in [('mode', '0644'), ('user', 'nobody'),
                             ('group', 'nobody')]:
            data = copy.deepcopy(self.data)
            data['paths'][2]['files'][0]['acl'][field] = value
            self.assertTrue(self.check_data(data))
        self.data['paths'][2]['files'][0]['target'] = '/tmp/unsafe-init'
        self.assertTrue(self.check_data(self.data))

    def test_malformed_and_duplicate_entries_rejected(self):
        for directory in [None, {'name': '../etc'}, {'name': '/etc'},
                          {'name': 'etc//init.d'}, {'name': 'etc/config'},
                          {'name': 'tmp', 'files': {}},
                          {'name': 'tmp', 'files': [{'name': '../opennds'}]},
                          {'name': 'tmp', 'files': [{'name': 'a'}, {'name': 'a'}]}]:
            with self.subTest(directory=directory), self.assertRaises(ValueError):
                data = copy.deepcopy(self.data)
                data['paths'].append(directory)
                self.check_data(data)

    def test_root_files_and_script_strings(self):
        self.data['paths'][0]['files'] = [{'name': 'root-file'}]
        self.data['scripts']['post-install'] = 'paths:\n  - name: evil\n'
        _, files = checker.parse_dump(yaml.safe_dump(self.data))
        self.assertIn('root-file', files)
        self.assertNotIn('/root-file', files)
        self.assertNotIn('evil', files)
        self.assertEqual(self.check_data(self.data), [])


if __name__ == '__main__':
    unittest.main()
