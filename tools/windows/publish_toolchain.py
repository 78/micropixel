#!/usr/bin/env python3
"""Create a draft, upload verified assets once, then publish the toolchain."""
import argparse
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path


def run(*args):
    subprocess.run(list(args), check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--directory', required=True, type=Path)
    parser.add_argument('--tag', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'toolchain-windows-x64-[0-9a-f]{16}', args.tag):
        raise SystemExit('Invalid immutable toolchain tag')
    manifest = json.loads((args.directory / 'toolchain.json').read_text())
    if args.tag != 'toolchain-' + manifest['toolchain_id']:
        raise SystemExit('Tag differs from manifest')
    existing = subprocess.check_output(['git', 'ls-remote', '--tags', 'origin', 'refs/tags/' + args.tag], text=True)
    if existing.strip():
        raise SystemExit('Published toolchain tags are immutable; refusing to overwrite')
    assets = sorted([args.directory / 'toolchain.json', *args.directory.glob('*.zip')])
    checksums = []
    for path in assets:
        with path.open('rb') as stream:
            checksums.append(hashlib.file_digest(stream, 'sha256').hexdigest() + '  ' + path.name)
    checksum_file = args.directory / 'sha256sums.txt'
    checksum_file.write_text('\n'.join(checksums) + '\n')
    notes = args.directory / 'release-notes.md'
    notes.write_text('Fixed Windows x64 AOT v6 toolchains and app-local MSVC runtime.\n\n'
                     'Both target compilers passed native Windows Guest example and Bundle validation. '
                     'This toolchain release is separate from SDK release channels.\n')
    run('gh', 'release', 'create', args.tag, '--draft', '--prerelease', '--latest=false',
        '--target', os.environ['GITHUB_SHA'], '--title', args.tag, '--notes-file', str(notes))
    run('gh', 'release', 'upload', args.tag, *map(str, [*assets, checksum_file]))
    run('gh', 'release', 'edit', args.tag, '--draft=false', '--prerelease', '--latest=false')


if __name__ == '__main__':
    main()
