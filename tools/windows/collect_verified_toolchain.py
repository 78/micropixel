#!/usr/bin/env python3
"""Accept artifacts only from a successful same-repository verification run."""
import argparse
import hashlib
import json
import os
import re
import subprocess
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--run', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9]+', args.run):
        raise SystemExit('Invalid verification run ID')
    repository = os.environ['GH_REPO']
    info = json.loads(subprocess.check_output(['gh', 'api', f'repos/{repository}/actions/runs/{args.run}']))
    if info['conclusion'] != 'success' or info['head_repository']['full_name'] != repository or info['path'] != '.github/workflows/windows-toolchain.yml':
        raise SystemExit('Run is not a successful same-repository Windows toolchain verification')
    sha = info['head_sha']
    if not re.fullmatch(r'[0-9a-f]{40}', sha):
        raise SystemExit('Invalid verified commit')
    subprocess.run(['git', 'fetch', '--depth=1', 'origin', sha], check=True)
    for relative in ('tools/windows/toolchain-sources.json', 'tools/windows/build_toolchain.py'):
        verified = subprocess.check_output(['git', 'show', f'{sha}:{relative}'])
        if verified.replace(b'\r\n', b'\n') != (ROOT / relative).read_bytes().replace(b'\r\n', b'\n'):
            raise SystemExit(f'Verified build recipe differs from release checkout: {relative}')
    args.output.mkdir(parents=True, exist_ok=True)
    for target in ('riscv32-ilp32f', 'xtensa'):
        name = 'wamrc-windows-x64-' + target
        subprocess.run(['gh', 'run', 'download', args.run, '--name', name, '--dir', str(args.output / name)], check=True)
    wasi = json.loads((ROOT / 'tools/windows/toolchain-sources.json').read_text())['wasi']
    archive = args.output / 'wasi.tar.gz'
    urllib.request.urlretrieve(wasi['url'], archive)
    with archive.open('rb') as stream:
        checksum = hashlib.file_digest(stream, 'sha256').hexdigest()
    if checksum != wasi['sha256']:
        raise SystemExit('Official WASI checksum mismatch')
    (args.output / 'verification.json').write_text(json.dumps({'run': args.run, 'commit': sha, 'repository': repository}) + '\n')


if __name__ == '__main__':
    main()
