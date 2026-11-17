#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fetch the public SDK revision pinned by this port into a new directory."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def prepare(directory):
    pin = json.loads((ROOT/'sdk-source.json').read_text())
    for patch in pin['patches']:
        if hashlib.sha256((ROOT/patch['path']).read_bytes()).hexdigest() != patch['sha256']:
            raise ValueError('SDK patch checksum mismatch: ' + patch['path'])
    if directory.exists():
        subprocess.run([sys.executable, str(ROOT/'tools/check_sdk.py'),
                        '--sdk', str(directory), '--exact'], check=True)
        return
    directory.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.micropixel-sdk-', dir=directory.parent) as scratch:
        sdk = Path(scratch)/'sdk'
        subprocess.run(['git', 'init', str(sdk)], check=True)
        subprocess.run(['git', '-C', str(sdk), 'config', 'core.autocrlf', 'false'], check=True)
        subprocess.run(['git', '-C', str(sdk), 'remote', 'add', 'origin', pin['repository']], check=True)
        subprocess.run(['git', '-C', str(sdk), 'fetch', '--depth=1', 'origin', pin['commit']], check=True)
        subprocess.run(['git', '-C', str(sdk), 'checkout', '--detach', pin['commit']], check=True)
        for patch in pin['patches']:
            path = str(ROOT/patch['path'])
            subprocess.run(['git', '-C', str(sdk), 'apply', '--check', path], check=True)
            subprocess.run(['git', '-C', str(sdk), 'apply', path], check=True)
        subprocess.run([sys.executable, str(ROOT/'tools/check_sdk.py'),
                        '--sdk', str(sdk), '--exact'], check=True)
        sdk.rename(directory)
    print(f'SDK ready: {directory}\nSet BL_SDK_BASE to this directory.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True,
                        help='new SDK directory, or an existing checkout to verify without modifying')
    args = parser.parse_args()
    prepare(args.directory.resolve())
