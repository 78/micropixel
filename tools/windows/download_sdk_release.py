#!/usr/bin/env python3
"""Recover verification from published bytes; never rebuild or replace assets."""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from windows.release_metadata import file_digest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tag', required=True)
    parser.add_argument('--directory', type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'sdk-v[0-9]+\.[0-9]+\.[0-9]+', args.tag):
        raise SystemExit('Expected an exact SDK tag')
    release = json.loads(subprocess.check_output(['gh', 'release', 'view', args.tag, '--json', 'isDraft,isPrerelease,assets']))
    if release['isDraft']:
        raise SystemExit('Recovery accepts published releases only')
    if args.directory.exists():
        raise SystemExit('Recovery needs a fresh output directory')
    assets = release['assets']
    if len(assets) > 100 or sum(a['size'] for a in assets) > 2 * 1024 ** 3:
        raise SystemExit('Unexpected SDK release size')
    for asset in assets:
        if not re.fullmatch(r'[A-Za-z0-9_.-]+', asset['name']) or not re.fullmatch(r'sha256:[0-9a-f]{64}', asset.get('digest') or ''):
            raise SystemExit('Release asset lacks a safe name or GitHub digest')
    subprocess.run(['gh', 'release', 'download', args.tag, '--dir', str(args.directory)], check=True)
    for asset in assets:
        path = args.directory / asset['name']
        if path.stat().st_size != asset['size'] or 'sha256:' + file_digest(path) != asset['digest']:
            raise SystemExit('Published asset differs from GitHub digest: ' + path.name)
    for line in (args.directory / 'sha256sums.txt').read_text().splitlines():
        sha, name = line.split('  ', 1)
        if not re.fullmatch(r'[A-Za-z0-9_.-]+', name) or file_digest(args.directory / name) != sha:
            raise SystemExit('Published checksum list differs: ' + name)
    entry = json.loads((args.directory / 'channel-entry.json').read_text())
    if 'sdk-v' + entry['version'] != args.tag or entry['installer']['preview'] != release['isPrerelease']:
        raise SystemExit('Release identity differs from requested release')
    print('Published SDK assets verified; no release files were changed.')


if __name__ == '__main__':
    main()
