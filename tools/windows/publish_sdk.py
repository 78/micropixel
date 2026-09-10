#!/usr/bin/env python3
"""Publish immutable Preview assets, then promote only verified releases to a channel."""
import argparse
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path


def run(*args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['draft', 'publish', 'promote'])
    parser.add_argument('--directory', type=Path, required=True)
    args = parser.parse_args()
    out = args.directory.resolve()
    entry = json.loads((out / 'channel-entry.json').read_text())
    version = entry['version']
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version):
        raise SystemExit('Invalid SDK version')
    tag = 'sdk-v' + version
    if os.environ.get('GITHUB_REF_TYPE') == 'tag' and os.environ['GITHUB_REF_NAME'] != tag:
        raise SystemExit('Tag and packaged CLI versions differ')
    if args.action == 'draft':
        if subprocess.run(['gh', 'release', 'view', tag], capture_output=True).returncode == 0:
            raise SystemExit('Release already exists; refusing to replace immutable assets')
        if not json.loads((out / 'draft-verification.json').read_text()).get('ok'):
            raise SystemExit('Installed draft verification is required')
        notes = out / 'release-notes.md'
        notes.write_text(f'''# MicroPixel SDK {version} Windows Preview

Per-user GUI and silent installer with embedded Python 3.13.12/pyserial 3.5,
verified WASI SDK 33 and fixed RISC-V/Xtensa AOT v6 compilers.

- Start with the attached Windows guide or AI.md; installer success alone does not mean ready.
- Project SDK/toolchain versions are locked; upgrading and rolling back are explicit.
- Unsigned Preview: Windows 10 GUI/security/USB acceptance and code signing remain pending.
- `test-sdk-index.json` and SDK 9000.0.1/9000.0.2 are isolated acceptance fixtures only.
- No new performance claim is made. Existing macOS toolchain usage remains supported.

Do not use the test index for production projects. See windows-acceptance.zh-CN.md.
''', encoding='utf-8')
        run('gh', 'release', 'create', tag, '--draft', '--prerelease', '--latest=false', '--target', os.environ['GITHUB_SHA'],
            '--title', f'SDK {version} Windows Preview', '--notes-file', notes)
        names = [line.split('  ', 1)[1] for line in (out / 'sha256sums.txt').read_text().splitlines()]
        run('gh', 'release', 'upload', tag, *[out / name for name in names], out / 'sha256sums.txt', out / 'draft-verification.json')
    elif args.action == 'publish':
        run('gh', 'release', 'edit', tag, '--draft=false', '--prerelease', '--latest=false')
    else:
        report = json.loads((out / 'public-verification.json').read_text())
        if not report.get('ok') or report.get('sdk_version') != version:
            raise SystemExit('Public download and installed build verification is required')
        # Preview promotion cannot set the stable SDK pointer. Stable promotion
        # needs a separate signed release and completed W01-W19 hardware review.
        remote = subprocess.check_output(['git', 'remote', 'get-url', 'origin'], text=True).strip()
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            run('git', 'init', directory)
            run('git', '-C', directory, 'remote', 'add', 'origin', remote)
            exists = subprocess.check_output(['git', 'ls-remote', '--heads', 'origin', 'sdk-channel'], text=True).strip()
            if exists:
                run('git', '-C', directory, 'fetch', '--depth=1', 'origin', 'sdk-channel')
                run('git', '-C', directory, 'checkout', '-b', 'sdk-channel', 'FETCH_HEAD')
                index = json.loads((directory / 'index.json').read_text())
            else:
                run('git', '-C', directory, 'checkout', '--orphan', 'sdk-channel')
                index = {'schema_version': 1, 'stable': None, 'preview': None, 'versions': {}}
            if version in index['versions'] and index['versions'][version] != entry['entry']:
                raise SystemExit('Channel version identity is immutable')
            index['versions'][version] = entry['entry']
            index.update(preview=version, manager=entry['manager'], windows_installer=entry['installer'])
            (directory / 'index.json').write_text(json.dumps(index, indent=2, sort_keys=True) + '\n')
            run('git', '-C', directory, 'config', 'user.name', 'MicroPixel release')
            run('git', '-C', directory, 'config', 'user.email', 'release@users.noreply.github.com')
            run('git', '-C', directory, 'add', 'index.json')
            run('git', '-C', directory, 'commit', '-m', f'Promote verified SDK {version} Preview')
            # A concurrent channel change fails normally; never force-push it.
            run('git', '-C', directory, 'push', 'origin', 'HEAD:refs/heads/sdk-channel')
        run('gh', 'release', 'upload', tag, out / 'public-verification.json')


if __name__ == '__main__':
    main()
