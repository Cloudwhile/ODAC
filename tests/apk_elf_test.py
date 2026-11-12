#!/usr/bin/env python3
"""Regression tests for the APK payload and ELF header checker."""
import hashlib
import importlib.util
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

import yaml

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    'apk_elf', ROOT / 'scripts/check-apk-elf.py')
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)

MUSL = '/lib/ld-musl-x86_64.so.1'


def make_elf(wide, lsb, machine, interp=None, soname=None, needed=()):
    """Build a minimal ELF file with program headers and a dynamic section."""
    order = '<' if lsb else '>'
    ehsize, phsize = (64, 56) if wide else (52, 32)
    phnum = 3 if interp else 2
    strtab, offsets = b'\0', {}
    for text in list(needed) + ([soname] if soname else []):
        offsets[text] = len(strtab)
        strtab += text.encode() + b'\0'
    str_off = ehsize + phsize * phnum
    entry = order + ('qQ' if wide else 'iI')
    dynamic = b''.join(struct.pack(entry, 1, offsets[name]) for name in needed)
    if soname:
        dynamic += struct.pack(entry, 14, offsets[soname])
    dynamic += struct.pack(entry, 5, str_off) + struct.pack(entry, 0, 0)
    dyn_off = str_off + len(strtab)
    interp_bytes = interp.encode() + b'\0' if interp else b''
    interp_off = dyn_off + len(dynamic)
    total = interp_off + len(interp_bytes)

    def program_header(kind, offset, size):
        if wide:
            return struct.pack(order + 'IIQQQQQQ', kind, 0, offset, offset,
                               offset, size, size, 0)
        return struct.pack(order + 'IIIIIIII', kind, offset, offset, offset,
                           size, size, 0, 0)

    ident = b'\x7fELF' + bytes([2 if wide else 1, 1 if lsb else 2, 1]) + bytes(9)
    header = ident + struct.pack(
        order + ('HHIQQQIHHHHHH' if wide else 'HHIIIIIHHHHHH'),
        3, machine, 1, 0, ehsize, 0, 0, ehsize, phsize, phnum, 0, 0, 0)
    headers = program_header(1, 0, total) + program_header(2, dyn_off,
                                                           len(dynamic))
    if interp:
        headers += program_header(3, interp_off, len(interp_bytes))
    return header + headers + strtab + dynamic + interp_bytes


def block(kind, content):
    data = struct.pack('<I', kind << 30 | (4 + len(content))) + content
    return data + bytes(-len(data) % 8)


def make_package(files, corrupt=None, drop=None, extra=False):
    """Return (apk bytes, adbdump text) in the layout apk itself writes."""
    directories = {}
    for path, content in files:
        parent, name = path.rsplit('/', 1)
        directories.setdefault(parent, []).append((name, content))
    paths, raw = [{'acl': {'mode': '0755'}}], b'ADB.pckg' + block(0, bytes(16))
    for path_index, (parent, entries) in enumerate(directories.items(), 2):
        listed = []
        for file_index, (name, content) in enumerate(entries, 1):
            listed.append({'name': name, 'size': len(content),
                           'hash': hashlib.sha256(content).hexdigest()})
            stored = b'changed' if corrupt == name else content
            if drop != name:
                raw += block(2, struct.pack('<II', path_index, file_index)
                             + stored)
        paths.append({'name': parent, 'files': listed})
    if extra:
        raw += block(2, struct.pack('<II', 99, 1) + b'unlisted')
    deflate = zlib.compressobj(wbits=-15)
    apk = b'ADBd' + deflate.compress(raw) + deflate.flush()
    return apk, yaml.safe_dump({'info': {'name': 'test'}, 'paths': paths})


class ElfTest(unittest.TestCase):
    def run_check(self, files, arch, **options):
        apk, dump = make_package(files, **options)
        return checker.check(apk, dump, arch)[0]

    def test_every_supported_architecture(self):
        for arch, wide, lsb, machine, loader in [
                ('x86_64', True, True, 62, MUSL),
                ('aarch64_cortex-a53', True, True, 183,
                 '/lib/ld-musl-aarch64.so.1'),
                ('arm_cortex-a7_neon-vfpv4', False, True, 40,
                 '/lib/ld-musl-armhf.so.1'),
                ('mipsel_24kc', False, True, 8, '/lib/ld-musl-mipsel-sf.so.1'),
                ('mips_24kc', False, False, 8, '/lib/ld-musl-mips-sf.so.1')]:
            with self.subTest(arch=arch):
                files = [
                    ('usr/bin/tool', make_elf(wide, lsb, machine, loader,
                                              needed=['libx.so.1', 'libc.so'])),
                    ('usr/lib/libx.so.1', make_elf(wide, lsb, machine,
                                                   soname='libx.so.1')),
                    ('etc/config/tool', b'config tool\n')]
                self.assertEqual(self.run_check(files, arch), [])

    def test_wrong_machine_and_byte_order_rejected(self):
        arm = [('usr/bin/tool', make_elf(True, True, 183, MUSL))]
        self.assertTrue(self.run_check(arm, 'x86_64'))
        little = [('usr/bin/tool', make_elf(False, True, 8, MUSL))]
        self.assertTrue(self.run_check(little, 'mips_24kc'))
        self.assertEqual(self.run_check(little, 'mipsel_24kc'), [])

    def test_foreign_loader_rejected(self):
        files = [('usr/bin/tool',
                  make_elf(True, True, 62, '/lib64/ld-linux-x86-64.so.2'))]
        self.assertTrue(any('loader' in error
                            for error in self.run_check(files, 'x86_64')))

    def test_soname_must_match_file_name(self):
        for soname in ('argon2.so.1', None):
            with self.subTest(soname=soname):
                files = [('usr/lib/libx.so.1',
                          make_elf(True, True, 62, soname=soname))]
                self.assertTrue(any('SONAME' in error for error in
                                    self.run_check(files, 'x86_64')))

    def test_noarch_and_empty_packages(self):
        script = [('usr/share/app/view.js', b'return 1;\n')]
        self.assertEqual(self.run_check(script, 'noarch'), [])
        self.assertTrue(self.run_check(script, 'x86_64'))
        binary = [('usr/bin/tool', make_elf(True, True, 62, MUSL))]
        self.assertTrue(self.run_check(binary, 'noarch'))
        self.assertTrue(self.run_check(binary, 'riscv64'))

    def test_payload_must_match_the_listing(self):
        files = [('usr/bin/tool', make_elf(True, True, 62, MUSL))]
        for options in ({'corrupt': 'tool'}, {'drop': 'tool'},
                        {'extra': True}):
            with self.subTest(options=options), self.assertRaises(ValueError):
                self.run_check(files, 'x86_64', **options)
        apk, dump = make_package(files)
        for broken in (apk[:-8], b'APK2' + apk[4:], apk + b'trailing'):
            with self.subTest(size=len(broken)), self.assertRaises(
                    (ValueError, zlib.error)):
                checker.check(broken, dump, 'x86_64')

    @unittest.skipUnless(shutil.which('cc') and platform.machine() == 'x86_64',
                         'needs an x86_64 host C compiler')
    def test_files_from_the_host_compiler(self):
        # Files this test did not build by hand: the parser must read what a
        # real linker writes, and a glibc executable must not pass as musl.
        with tempfile.TemporaryDirectory() as work:
            source = Path(work, 'x.c')
            source.write_text('int odac_x(void) { return 1; }\n'
                              'int main(void) { return 0; }\n')
            for arguments in (['-shared', '-fPIC', '-Wl,-soname,libx.so.1',
                               '-o', 'libx.so.1'], ['-o', 'tool']):
                subprocess.run(['cc', *arguments, 'x.c'], cwd=work, check=True)
            library = Path(work, 'libx.so.1').read_bytes()
            tool = Path(work, 'tool').read_bytes()
        info = checker.elf_info(library)
        self.assertEqual((info['class'], info['order'], info['machine']),
                         (2, 1, 62))
        self.assertEqual(info['soname'], 'libx.so.1')
        self.assertIsNone(info['interp'])
        self.assertTrue(any(name.startswith('libc.so')
                            for name in checker.elf_info(tool)['needed']))
        self.assertEqual(
            self.run_check([('usr/lib/libx.so.1', library)], 'x86_64'), [])
        self.assertTrue(any('loader' in error for error in self.run_check(
            [('usr/bin/tool', tool)], 'x86_64')))


if __name__ == '__main__':
    unittest.main()
