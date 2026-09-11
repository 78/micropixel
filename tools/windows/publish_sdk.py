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
        notes.write_text(f"""## 开始开发：只下载与你电脑对应的一个文件

| 你的电脑 | 下载 | 下一步 |
| --- | --- | --- |
| **Windows 10 / 11 x64** | **[Windows 安装包]({entry['installer']['url']})** | 双击安装，自动准备 Python 和编译工具链 |
| **macOS / Linux** | **[SDK 归档](https://github.com/78/micropixel/releases/download/{tag}/micropixel-sdk-{version}.tar.gz)** | 解压后按指南配置工具链 |

**[安装与 AI 使用指南](https://micropixel.ai/docs/environment/)** · **[创建第一个游戏](https://micropixel.ai/docs/quickstart/)**

安装完成后直接创建第一个游戏。遇到问题再查看[安装 FAQ](https://micropixel.ai/docs/environment-faq/)。
无需另装 Git 或 ESP-IDF。其余 JSON、manager ZIP 和校验文件供安装与更新流程使用，无需手动下载。
GitHub 自动生成的 Source code 是仓库源码，不是 SDK。

已有项目保持版本锁，升级和回退需显式执行。从 0.16.0 升级请运行新安装包，以修复 Ctrl-C 启动层。
Windows 安装包未签名；Windows 11 与 S31/S3 部分验收已完成，Windows 10 和剩余人工项目继续跟踪。
自动安装、双架构编译及升级回退验证通过。验收材料单独保存，不属于普通用户安装内容。
""", encoding='utf-8')
        run('gh', 'release', 'create', tag, '--draft', '--prerelease=' + str(preview).lower(), '--latest=false', '--target', os.environ['GITHUB_SHA'],
            '--title', f'SDK {version}{label}', '--notes-file', notes)
        names = sorted(user_assets(entry))
        run('gh', 'release', 'upload', tag, *[str(out / name) + '#' + display_label(entry, name) for name in names], str(out / 'sha256sums.txt') + '#下载校验（可选）')
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
