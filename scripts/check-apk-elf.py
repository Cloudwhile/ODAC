#!/usr/bin/env python3
"""Check the payload of an APK v3 package: file hashes and ELF headers.

Usage: check-apk-elf.py <file.apk> <adbdump-file> <arch|noarch>

The package is parsed as data; nothing in it is executed and no signature
decision is made here. Every data block is matched by SHA256 to its entry in
the "apk adbdump" listing, which ties the inspected bytes to the metadata the
other checks use. ELF files must match the machine, word size and byte order
of <arch>, executables must use the musl loader, and the SONAME of a shared
library must equal its file name. A noarch package must not carry ELF files.

This proves what the package contains. It does not prove that the libraries
resolve on a device; that is part of the installation acceptance.
"""
import hashlib
from pathlib import Path
import re
import struct
import sys
import zlib

import yaml

# Architecture prefix: ELF class (1=32 bit, 2=64 bit), byte order (1=LSB,
# 2=MSB), e_machine.
ARCHES = (
    ('x86_64', 2, 1, 62),
    ('aarch64', 2, 1, 183),
    ('arm_', 1, 1, 40),
    ('mipsel_', 1, 1, 8),
    ('mips_', 1, 2, 8),
)
MUSL_LOADER = re.compile(r'^/lib/ld-musl-[a-z0-9_-]+\.so\.1$')


def data_blocks(apk_bytes):
    """Return {(path index, file index): content} of an ADB container."""
    if apk_bytes[:4] != b'ADBd':
        raise ValueError('unsupported container: expected a deflate ADB file')
    inflater = zlib.decompressobj(wbits=-15)
    raw = inflater.decompress(apk_bytes[4:])
    if not inflater.eof or inflater.unused_data:
        raise ValueError('truncated or trailing compressed data')
    if raw[:4] != b'ADB.':
        raise ValueError('missing ADB header')
    blocks = {}
    offset = 8
    while offset < len(raw):
        if offset + 4 > len(raw):
            raise ValueError('truncated block header')
        (type_size,) = struct.unpack_from('<I', raw, offset)
        if type_size >> 30 == 3:  # extended block: 16 byte header
            if offset + 16 > len(raw):
                raise ValueError('truncated block header')
            kind = type_size & 0x3fffffff
            (size,) = struct.unpack_from('<Q', raw, offset + 8)
            header = 16
        else:
            kind = type_size >> 30
            size = type_size & 0x3fffffff
            header = 4
        if size < header or offset + size > len(raw):
            raise ValueError('invalid block size')
        if kind == 2:
            payload = raw[offset + header:offset + size]
            if len(payload) < 8:
                raise ValueError('truncated data block')
            key = struct.unpack_from('<II', payload)
            if key in blocks:
                raise ValueError(f'duplicate data block {key}')
            blocks[key] = payload[8:]
        offset += (size + 7) & ~7
    return blocks


def listed_files(dump_text):
    """Return {(path index, file index): (full path, entry)} of a dump."""
    data = yaml.load(dump_text, Loader=yaml.BaseLoader)
    if not isinstance(data, dict) or not isinstance(data.get('paths'), list):
        raise ValueError('missing package paths')
    files = {}
    for path_index, directory in enumerate(data['paths'], 1):
        if not isinstance(directory, dict):
            raise ValueError('invalid directory entry')
        parent = directory.get('name', '')
        for file_index, entry in enumerate(directory.get('files') or [], 1):
            if not isinstance(entry, dict) or not entry.get('name'):
                raise ValueError('invalid file entry')
            path = f'{parent}/{entry["name"]}' if parent else entry['name']
            files[(path_index, file_index)] = (path, entry)
    return files


def payload(apk_bytes, dump_text):
    """Return {path: content}, after matching every block to the listing."""
    blocks = data_blocks(apk_bytes)
    files = listed_files(dump_text)
    result = {}
    for key, (path, entry) in files.items():
        if 'target' in entry or int(entry.get('size', 0)) == 0:
            continue  # symlinks and empty files carry no data block
        if key not in blocks:
            raise ValueError(f'{path}: no data block in the package')
        content = blocks.pop(key)
        if hashlib.sha256(content).hexdigest() != entry.get('hash'):
            raise ValueError(f'{path}: content does not match the listed hash')
        result[path] = content
    if blocks:
        raise ValueError(f'data blocks without a listed file: {sorted(blocks)}')
    return result


def elf_info(content):
    """Return the header facts of an ELF file, or None for other files."""
    if content[:4] != b'\x7fELF' or len(content) < 52:
        return None
    wide = content[4] == 2
    order = '<' if content[5] == 1 else '>'
    machine = struct.unpack_from(order + 'H', content, 18)[0]
    if wide:
        phoff, = struct.unpack_from(order + 'Q', content, 32)
        phentsize, phnum = struct.unpack_from(order + 'HH', content, 54)
    else:
        phoff, = struct.unpack_from(order + 'I', content, 28)
        phentsize, phnum = struct.unpack_from(order + 'HH', content, 42)
    loads, dynamic, interp = [], None, None
    for index in range(phnum):
        base = phoff + index * phentsize
        if wide:
            kind, _, offset, vaddr, _, filesz = struct.unpack_from(
                order + 'IIQQQQ', content, base)
        else:
            kind, offset, vaddr, _, filesz = struct.unpack_from(
                order + 'IIIII', content, base)
        if kind == 1:
            loads.append((vaddr, offset, filesz))
        elif kind == 2:
            dynamic = (offset, filesz)
        elif kind == 3:
            interp = content[offset:offset + filesz].split(b'\0')[0].decode()

    def file_offset(address):
        for vaddr, offset, filesz in loads:
            if vaddr <= address < vaddr + filesz:
                return offset + address - vaddr
        raise ValueError('dynamic string table is outside the loaded file')

    needed, soname = [], None
    if dynamic:
        entry = order + ('qQ' if wide else 'iI')
        step = struct.calcsize(entry)
        tags = [struct.unpack_from(entry, content, dynamic[0] + position)
                for position in range(0, dynamic[1] - step + 1, step)]
        strtab = [value for tag, value in tags if tag == 5]
        if strtab:
            table = file_offset(strtab[0])

            def text(value):
                end = content.index(b'\0', table + value)
                return content[table + value:end].decode()

            needed = [text(value) for tag, value in tags if tag == 1]
            soname = next((text(value) for tag, value in tags if tag == 14),
                          None)
    return {'class': content[4], 'order': content[5], 'machine': machine,
            'interp': interp, 'needed': needed, 'soname': soname}


def check(apk_bytes, dump_text, arch):
    """Return (problems, report lines) for one package."""
    expected = next((entry[1:] for entry in ARCHES
                     if arch == entry[0] or (arch.startswith(entry[0]) and
                                             entry[0] != 'x86_64')), None)
    if arch != 'noarch' and expected is None:
        return [f'unsupported architecture: {arch}'], []
    failures, report, count = [], [], 0
    for path, content in sorted(payload(apk_bytes, dump_text).items()):
        info = elf_info(content)
        if info is None:
            continue
        count += 1
        report.append(
            f'{path}: ELF{32 * info["class"]} '
            f'{"LSB" if info["order"] == 1 else "MSB"} '
            f'machine={info["machine"]} interp={info["interp"]} '
            f'soname={info["soname"]} needed={",".join(info["needed"])}')
        if arch == 'noarch':
            failures.append(f'{path}: ELF file in a noarch package')
            continue
        if (info['class'], info['order'], info['machine']) != expected:
            failures.append(f'{path}: ELF header does not match {arch}')
        if info['interp'] and not MUSL_LOADER.match(info['interp']):
            failures.append(f'{path}: unexpected loader {info["interp"]}')
        name = path.rsplit('/', 1)[-1]
        if '.so' in name and info['soname'] != name:
            failures.append(f'{path}: SONAME {info["soname"]} differs from '
                            'the file name')
    if arch != 'noarch' and count == 0:
        failures.append('no ELF file in an architecture specific package')
    return failures, report


def main(argv):
    if len(argv) != 4:
        print('usage: check-apk-elf.py <file.apk> <adbdump-file> <arch>',
              file=sys.stderr)
        return 2
    try:
        failures, report = check(Path(argv[1]).read_bytes(),
                                 Path(argv[2]).read_text(encoding='utf-8'),
                                 argv[3])
    except (OSError, ValueError, struct.error, zlib.error,
            yaml.YAMLError) as error:
        failures, report = [str(error)], []
    for line in report:
        print(line)
    for failure in failures:
        print(f'FAIL: {failure}', file=sys.stderr)
    if failures:
        return 1
    print(f'payload hashes and ELF headers OK ({len(report)} ELF, {argv[3]})')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
