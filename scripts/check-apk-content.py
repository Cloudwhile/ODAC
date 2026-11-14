#!/usr/bin/env python3
"""Validate APK v3 adbdump file trees and safety-critical source hashes.

This checks package metadata, not the payload or its signature. Callers must
obtain the dump directly from apk, check its exit status, and verify signatures
before installation. ELF/ABI and lifecycle acceptance are separate checks.
"""
import hashlib
from pathlib import Path, PurePosixPath
import sys

import yaml

REQUIRED = {
    'odac-opennds': {
        'etc/init.d/opennds', 'etc/config/opennds',
        'usr/libexec/odac-opennds-check', 'usr/bin/opennds', 'usr/bin/ndsctl',
    },
    'device-access': {
        'etc/init.d/odac', 'etc/config/odac', 'usr/sbin/odacd',
        'usr/bin/odacctl', 'usr/libexec/odac-setup',
        'usr/libexec/odac-remove', 'usr/libexec/odac-purge',
        'usr/libexec/odac-preflight', 'usr/libexec/odac-firewall',
        'usr/libexec/odac-rpc', 'etc/odac/firewall-include',
        'lib/upgrade/keep.d/device-access',
    },
    'luci-app-device-access': {
        'usr/share/luci/menu.d/luci-app-device-access.json',
        'usr/share/rpcd/acl.d/luci-app-device-access.json',
        'www/luci-static/resources/view/device-access/dashboard.js',
    },
    'libargon2': {'usr/lib/libargon2.so.1'},
}

# These files are copied verbatim by the recipes. A stock init or an enabled
# default configuration must fail even when it has the expected file name.
PINNED = {
    'odac-opennds': {'etc/init.d/opennds': 0o755,
                     'etc/config/opennds': 0o600,
                     'usr/libexec/odac-opennds-check': 0o755},
    'device-access': {'etc/init.d/odac': 0o755,
                      'etc/config/odac': 0o644},
}


def valid_path(path, root=False):
    return (isinstance(path, str) and
            ((root and path == '') or
             (path != '' and not path.startswith('/') and
              all(part not in ('', '.', '..') for part in path.split('/')))))


def parse_dump(text):
    """Read adbdump's YAML tree; directory names already contain full paths."""
    # BaseLoader accepts only strings/lists/maps, never executable YAML objects,
    # and preserves octal modes and hexadecimal digests as written by apk.
    data = yaml.load(text, Loader=yaml.BaseLoader)
    if not isinstance(data, dict) or not isinstance(data.get('info'), dict):
        raise ValueError('missing package info')
    name = data['info'].get('name')
    if not isinstance(name, str) or not name:
        raise ValueError('missing package name')
    paths = data.get('paths')
    if not isinstance(paths, list) or not paths:
        raise ValueError('missing package paths')
    files = {}
    directories = set()
    for directory in paths:
        if not isinstance(directory, dict):
            raise ValueError('invalid directory entry')
        parent = directory.get('name', '')  # apk omits the root directory name.
        if not valid_path(parent, root=True) or parent in directories:
            raise ValueError(f'invalid or duplicate directory: {parent!r}')
        directories.add(parent)
        children = directory.get('files', [])
        if not isinstance(children, list):
            raise ValueError(f'invalid files in {parent!r}')
        for child in children:
            if not isinstance(child, dict):
                raise ValueError('invalid file entry')
            leaf = child.get('name')
            if not valid_path(leaf) or '/' in leaf:
                raise ValueError(f'invalid file name: {leaf!r}')
            path = f'{parent}/{leaf}' if parent else leaf
            if path in files:
                raise ValueError(f'duplicate file: {path}')
            files[path] = child
    if not files:
        raise ValueError('empty package file list')
    return name, files


def check(package, text, source_root):
    if package not in REQUIRED:
        return [f'unsupported package: {package}']
    name, files = parse_dump(text)
    failures = []
    if package == 'device-access':
        data = yaml.load(text, Loader=yaml.BaseLoader)
        depends = data['info'].get('depends', [])
        if not isinstance(depends, list) or 'odac-opennds>=10.3.1-r2' not in depends:
            failures.append('device-access must require odac-opennds>=10.3.1-r2')
    if name != package:
        failures.append(f'package name {name!r} does not match {package!r}')
    for path in sorted(REQUIRED[package]):
        if path not in files:
            failures.append(f'{package} does not ship {path}')
    for path in files:
        if path.startswith('etc/uci-defaults/'):
            failures.append(f'{package} ships install-time uci-defaults: {path}')
        if PurePosixPath(path).name == 'restart.sh':
            failures.append(f'{package} ships an excluded restart hook: {path}')
    for path, mode in PINNED.get(package, {}).items():
        if path not in files:
            continue
        entry = files[path]
        source = source_root / package / 'files' / path
        expected_hash = hashlib.sha256(source.read_bytes()).hexdigest()
        if entry.get('hash') != expected_hash or 'target' in entry:
            failures.append(f'{package}: {path} does not match the source file')
        acl = entry.get('acl', {})
        if not isinstance(acl, dict) or any(
                acl.get(key) != value for key, value in
                {'mode': f'{mode:04o}', 'user': 'root', 'group': 'root'}.items()):
            failures.append(f'{package}: {path} has unexpected permissions/owner')
    return failures


def main(argv):
    if len(argv) != 3:
        print('usage: check-apk-content.py <apk-dump-file> <package-name>',
              file=sys.stderr)
        return 2
    dump_file, package = argv[1:]
    try:
        failures = check(package, Path(dump_file).read_text(encoding='utf-8'),
                         Path(__file__).resolve().parents[1])
    except (OSError, ValueError, yaml.YAMLError) as error:
        failures = [str(error)]
    for failure in failures:
        print(f'FAIL: {failure}', file=sys.stderr)
    if failures:
        return 1
    print(f'{package}: file layout and pinned source metadata OK')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
