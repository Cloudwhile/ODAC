#!/usr/bin/env python3
"""Select one official SDK manifest entry and optionally verify its archive.

The caller downloads the manifest using verified HTTPS. This script reads data
only; it does not extract the archive or execute any SDK code.
"""
import argparse
import hashlib
from pathlib import Path
import re


def select(manifest, version, target, subtarget):
    prefix = re.escape(f"openwrt-sdk-{version}-{target}-{subtarget}_")
    filename = re.compile(prefix + r"[A-Za-z0-9_.+-]+\.Linux-x86_64\.tar\.zst")
    matches = []
    for line in manifest.splitlines():
        # Accept sha256sum's binary (*) and text (space) markers, exactly.
        entry = re.fullmatch(r"([0-9a-fA-F]{64}) [ *](\S+)", line)
        if entry and filename.fullmatch(entry[2]):
            matches.append((entry[2], entry[1].lower()))
    if len(matches) != 1:
        raise ValueError(f"expected exactly one SDK checksum entry, found {len(matches)}")
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("version")
    parser.add_argument("target")
    parser.add_argument("subtarget")
    parser.add_argument("--archive", type=Path)
    args = parser.parse_args()
    try:
        name, expected = select(args.manifest.read_text(encoding="utf-8"),
                                args.version, args.target, args.subtarget)
        if args.archive is not None:
            if args.archive.name != name:
                raise ValueError("archive filename differs from the manifest selection")
            digest = hashlib.sha256()
            with args.archive.open("rb") as archive:
                for block in iter(lambda: archive.read(1024 * 1024), b""):
                    digest.update(block)
            if digest.hexdigest() != expected:
                raise ValueError("SDK archive SHA256 differs from the official manifest")
        print(name)
    except (OSError, UnicodeError, ValueError) as exc:
        parser.exit(1, f"SDK verification failed: {exc}\n")


if __name__ == "__main__":
    main()
