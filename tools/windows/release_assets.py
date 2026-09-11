"""Keep user downloads separate from immutable verification support assets."""
import hashlib


def support_tag(version):
    return 'sdk-support-v' + version


def user_assets(entry):
    version = entry['version']
    return {f'micropixel-sdk-{version}.tar.gz', entry['installer']['name'], entry['manager']['name'],
            'release.json', 'sdk-manifest.json', 'windows-installer.json', 'release-notes.json'}


def split_checksums(out, entry):
    public = user_assets(entry)
    excluded = {'inno-setup.exe', 'sha256sums.txt', 'verification-sha256sums.txt', 'release-notes.md', 'support-notes.md'}
    paths = sorted(p for p in out.iterdir() if p.is_file() and p.name not in excluded)
    def lines(selected):
        return ''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.name + '\n' for p in selected)
    (out / 'sha256sums.txt').write_bytes(lines(p for p in paths if p.name in public).encode())
    (out / 'verification-sha256sums.txt').write_bytes(lines(paths).encode())


def asset_url(entry, name):
    base = entry['entry']['url'].rsplit('/', 1)[0]
    return (base if name in user_assets(entry) or name == 'sha256sums.txt' else entry.get('verification_base', base)) + '/' + name


def display_label(entry, name):
    if name == entry['installer']['name']:
        return 'Windows 10 / 11 x64 安装包（Windows 用户下载这个）'
    if name == f"micropixel-sdk-{entry['version']}.tar.gz":
        return 'macOS / Linux SDK（macOS 和 Linux 用户下载这个）'
    if name == entry['manager']['name']:
        return '自动更新组件（无需手动下载）'
    return '机器清单（无需手动下载）'
