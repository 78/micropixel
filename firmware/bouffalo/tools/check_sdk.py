#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check SDK APIs; --exact verifies the public revision and declared SDK patches."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = {
    'bsp/board/bl616cldk/bl616cl_common.ld.in': ['__tls_size'],
    'bsp/board/bl618dgdk/bl618dg_common.ld.in': ['__tls_size'],
    'components/os/freertos/include/portable.h': ['configSTACK_ALLOCATION_FROM_SEPARATE_HEAP'],
}
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=os.environ.get('BL_SDK_BASE'))
    parser.add_argument('--exact', action='store_true')
    args = parser.parse_args()
    if not args.sdk:
        parser.error('Set BL_SDK_BASE or pass --sdk')
    sdk = Path(args.sdk).resolve()
    failures = []
    for file, tokens in REQUIRED.items():
        path = sdk/file
        if not path.is_file() or any(token not in path.read_text() for token in tokens):
            failures.append(file)
    if args.exact:
        source = json.loads((ROOT/'sdk-source.json').read_text())
        allowed_changes = set()
        for patch in source['patches']:
            allowed_changes.update(patch['files'])
            path = ROOT/patch['path']
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != patch['sha256']:
                failures.append('SDK patch checksum: ' + patch['path'])
        try:
            top = subprocess.check_output(
                ['git', '-C', str(sdk), 'rev-parse', '--show-toplevel'], text=True,
                stderr=subprocess.PIPE).strip()
            if Path(top).resolve() != sdk:
                failures.append('SDK must be the root of its Git checkout')
            revision = subprocess.check_output(
                ['git', '-C', str(sdk), 'rev-parse', 'HEAD'], text=True,
                stderr=subprocess.PIPE).strip()
            if revision != source['commit']:
                failures.append('Git HEAD (expected ' + source['commit'] + ')')
            changed = subprocess.check_output(
                ['git', '-C', str(sdk), 'diff', '--name-only', '-z', 'HEAD', '--'],
                stderr=subprocess.PIPE).decode().split('\0')
            unexpected = set(filter(None, changed)) - allowed_changes
            if unexpected:
                failures.append('undeclared SDK changes: ' + ', '.join(sorted(unexpected)))
            untracked = subprocess.check_output(
                ['git', '-C', str(sdk), 'ls-files', '--others', '--exclude-standard', '-z'],
                stderr=subprocess.PIPE).decode().split('\0')
            if any(untracked):
                failures.append('untracked SDK files: ' + ', '.join(filter(None, untracked)))
            baseline = json.loads((ROOT/'sdk-baseline.json').read_text())['sha256']
            if not allowed_changes.issubset(baseline):
                failures.append('patched SDK files missing from fingerprint manifest')
        except (OSError, subprocess.CalledProcessError):
            failures.append('SDK Git metadata is missing or inaccessible')
        for file, expected in json.loads((ROOT/'sdk-baseline.json').read_text())['sha256'].items():
            path = sdk/file
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
                failures.append(file)
    if failures:
        raise SystemExit('SDK compatibility check failed: ' + ', '.join(sorted(set(failures))))
    print('SDK API checks passed' + ('; pinned public revision, patches and fingerprints match' if args.exact
                                    else '; use --exact to verify the reproducible SDK baseline'))
