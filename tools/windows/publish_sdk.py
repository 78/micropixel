#!/usr/bin/env python3
"""Publish immutable SDK assets, then promote only verified releases to a channel."""
import argparse
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path

try:
    from .release_assets import support_tag, user_assets, display_label
except ImportError:
    from release_assets import support_tag, user_assets, display_label


def run(*args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def promote_index(index, entry):
    """Advance one channel without changing immutable identities or newer defaults."""
    version = entry['version']
    channel = 'preview' if entry['installer']['preview'] else 'stable'
    if index.get(channel) and tuple(map(int, index[channel].split('.'))) > tuple(map(int, version.split('.'))):
        raise SystemExit('A newer SDK is already promoted; refusing to roll back the channel')
    if version in index['versions'] and index['versions'][version] != entry['entry']:
        raise SystemExit('Channel version identity is immutable')
    index['versions'][version] = entry['entry']
    index[channel] = version
    current = index.get('windows_installer', {}).get('version')
    if not current or tuple(map(int, current.split('.'))) <= tuple(map(int, version.split('.'))):
        index.update(manager=entry['manager'], windows_installer=entry['installer'])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['draft', 'publish', 'promote'])
    parser.add_argument('--directory', type=Path, required=True)
    args = parser.parse_args()
    out = args.directory.resolve()
    entry = json.loads((out / 'channel-entry.json').read_text())
    version = entry['version']
    preview = entry['installer']['preview']
    channel = 'preview' if preview else 'stable'
    label = ' Preview' if preview else ''
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
        notes.write_text(f"""## Get started: choose one download for your computer

| Platform | Download | Next step |
| --- | --- | --- |
| **Windows 10 / 11 x64** | **[Windows installer]({entry['installer']['url']})** | Run the installer to set up Python and the build toolchain automatically |
| **macOS / Linux** | **[SDK archive](https://github.com/78/micropixel/releases/download/{tag}/micropixel-sdk-{version}.tar.gz)** | Extract the archive and follow the toolchain setup guide |

**[Setup and AI workflow guide](https://micropixel.ai/docs/environment/)** · **[Create your first game](https://micropixel.ai/docs/quickstart/)**

After installation, start with your first game. See the [installation FAQ](https://micropixel.ai/docs/environment-faq/) for troubleshooting.
You do not need a separate Git or ESP-IDF installation. JSON files, the manager ZIP and checksums support installation and updates; manual downloads are unnecessary.
GitHub's automatically generated Source code archives contain repository sources, not the SDK distribution.

## What's new

- Gamepad buttons support customization, and default control bounds now follow the logical canvas.
- Gamepad glyph, rim and fill opacity are controlled independently, with refined defaults for overlay visibility.
- Updated examples include refined Tomb Explorer controls and improved Jump Jump sound and charge cues.
- Companion firmware 0.9.4 reports private KV usage, clears private data on explicit uninstall, and defaults to 16 KiB per AppId and 4 KiB per value. Updating an installed App preserves its data; earlier Hosts retain their configured quotas.
- Firmware 0.9.4 also updates app launch feedback, image loading, storage handling and device volume behavior. Existing installed Bundles remain installed during Host-only updates.

## Installation and compatibility

Existing projects remain version-pinned; upgrades and rollbacks are explicit. Upgrading from 0.16.0 still requires running the new installer to fix Ctrl-C handling in the launcher.
The Windows installer is unsigned. Windows 11 and some S31/S3 acceptance checks are complete; Windows 10 and remaining manual checks are still pending.
Automated installation lifecycle checks and builds for both architectures passed. Verification fixtures and evidence are maintained separately from user downloads.
""", encoding='utf-8')
        run('gh', 'release', 'create', tag, '--draft', '--prerelease=' + str(preview).lower(), '--latest=false', '--target', os.environ['GITHUB_SHA'],
            '--title', f'SDK {version}{label}', '--notes-file', notes)
        names = sorted(user_assets(entry))
        run('gh', 'release', 'upload', tag, *[str(out / name) + '#' + display_label(entry, name) for name in names], str(out / 'sha256sums.txt') + '#Checksums (optional)')
        support = support_tag(version)
        support_notes = out / 'support-notes.md'
        support_notes.write_text('Maintainer-only SDK verification fixtures and evidence. Users: download the SDK or installer from ' + tag + '.\n')
        run('gh', 'release', 'create', support, '--draft', '--prerelease', '--latest=false',
            '--target', os.environ['GITHUB_SHA'], '--title', f'Internal verification — SDK {version}', '--notes-file', support_notes)
        internal = [line.split('  ', 1)[1] for line in (out / 'verification-sha256sums.txt').read_text().splitlines()
                    if line.split('  ', 1)[1] not in user_assets(entry)]
        run('gh', 'release', 'upload', support, *[out / name for name in internal],
            out / 'verification-sha256sums.txt', out / 'draft-verification.json')
    elif args.action == 'publish':
        run('gh', 'release', 'edit', support_tag(version), '--draft=false', '--prerelease', '--latest=false')
        run('gh', 'release', 'edit', tag, '--draft=false', '--prerelease=' + str(preview).lower(), '--latest=false')
    else:
        report = json.loads((out / 'public-verification.json').read_text())
        if not report.get('ok') or report.get('sdk_version') != version:
            raise SystemExit('Public download and installed build verification is required')
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
            promote_index(index, entry)
            (directory / 'index.json').write_text(json.dumps(index, indent=2, sort_keys=True) + '\n')
            run('git', '-C', directory, 'config', 'user.name', 'MicroPixel release')
            run('git', '-C', directory, 'config', 'user.email', 'release@users.noreply.github.com')
            run('git', '-C', directory, 'add', 'index.json')
            changed = subprocess.run(['git', '-C', str(directory), 'diff', '--cached', '--quiet']).returncode
            if changed:
                run('git', '-C', directory, 'commit', '-m', f'Promote verified SDK {version} {channel}')
                # A concurrent channel change fails normally; never force-push it.
                run('git', '-C', directory, 'push', 'origin', 'HEAD:refs/heads/sdk-channel')
        evidence_tag = support_tag(version) if entry.get('verification_base') else tag
        release = json.loads(subprocess.check_output(['gh', 'release', 'view', evidence_tag, '--json', 'assets']))
        if not any(asset['name'] == 'public-verification.json' for asset in release['assets']):
            run('gh', 'release', 'upload', evidence_tag, out / 'public-verification.json')


if __name__ == '__main__':
    main()
