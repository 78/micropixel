#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fetch pinned source dependencies. Existing trees are verified, never overwritten."""
import argparse
import base64
import hashlib
import http.client
import time
import urllib.error
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]

def verify(directory, pin):
    for name, expected in pin['verify_files'].items():
        path = directory/name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f'Dependency source mismatch: {path}; preserve it and prepare a fresh tree')

def prepare(name, pin, root, check):
    target = root/name
    if target.exists():
        verify(target, pin)
        print(f'{name}: verified')
        return
    if check:
        raise ValueError(f'Missing {target}; run prepare_deps.py first')
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.'+name+'-', dir=root) as scratch:
        scratch = Path(scratch)
        archive = scratch/'download'
        request = urllib.request.Request(pin['url'], headers={'User-Agent': 'MicroPixel-Bouffalo'})
        expected = pin['archive_sha256']
        for attempt in range(3):
            try:
                with urllib.request.urlopen(request, timeout=60) as response, archive.open('wb') as output:
                    shutil.copyfileobj(response, output)
                if name == 'stb':
                    payload = json.loads(archive.read_text())
                    archive.write_bytes(base64.b64decode(payload['content']))
                actual = hashlib.sha256(archive.read_bytes()).hexdigest()
                if actual != expected:
                    raise ValueError(f'{name}: archive SHA256 mismatch ({actual})')
                break
            except (OSError, http.client.HTTPException, urllib.error.URLError, ValueError):
                if attempt == 2:
                    raise
                print(f'{name}: incomplete or unverified download; retrying', flush=True)
                time.sleep(1)
        print(f'{name}: archive SHA256 {actual}')
        if 'file' in pin:
            source = scratch/'source'
            source.mkdir()
            shutil.copy2(archive, source/pin['file'])
        else:
            extracted = scratch/'unpack'
            with tarfile.open(archive) as compressed:
                compressed.extractall(extracted, filter='data')
            children = list(extracted.iterdir())
            if len(children) != 1 or not children[0].is_dir():
                raise ValueError('Expected one archive source directory')
            source = children[0]
        if name == 'libpng':
            shutil.copy2(source/'scripts/pnglibconf.h.prebuilt', source/'pnglibconf.h')
        verify(source, pin)
        source.rename(target)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--directory', type=Path, default=ROOT/'third_party')
    parser.add_argument('--only', nargs='+')
    args = parser.parse_args()
    pins = json.loads((ROOT/'dependencies.json').read_text())
    for name in args.only or pins:
        prepare(name, pins[name], args.directory.resolve(), args.check)
