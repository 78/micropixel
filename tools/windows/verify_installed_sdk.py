#!/usr/bin/env python3
"""Exercise installed tools with developer tools removed from PATH; never upload apps."""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request
import urllib.error
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from windows.release_metadata import file_digest
from windows.release_assets import asset_url
from manager.micropixel_manager import extract



def download_public(url: str, path: Path):
    for attempt in range(5):
        try:
            with urllib.request.urlopen(url, timeout=30) as response, path.open('wb') as output:
                shutil.copyfileobj(response, output, 1024 * 1024)
            return
        except (urllib.error.URLError, TimeoutError) as error:
            path.unlink(missing_ok=True)
            if isinstance(error, urllib.error.HTTPError) and error.code not in (404, 408, 429, 500, 502, 503, 504):
                raise
            if attempt == 4:
                raise
            print(f'Public asset {path.name} is not ready ({error}); retrying', file=sys.stderr)
            time.sleep(2 * (attempt + 1))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--public', action='store_true')
    parser.add_argument('--quick-public', action='store_true', help='Verify published bytes against this job’s already tested installer')
    parser.add_argument('--full-validation', action='store_true', help='Also exercise A/B upgrade, rollback and manager switching')
    args = parser.parse_args()
    out = args.directory.resolve()
    entry = json.loads((out / 'channel-entry.json').read_text())
    version = entry['version']
    if args.quick_public:
        if not args.public:
            raise SystemExit('--quick-public requires --public')
        report = json.loads((out / 'draft-verification.json').read_text())
        if not report.get('ok') or report.get('sdk_version') != version or report.get('stage') != 'draft':
            raise SystemExit('Matching installed build verification is required')
        for line in (out / 'sha256sums.txt').read_text().splitlines():
            expected, name = line.split('  ', 1)
            path = out / (name + '.public-check')
            try:
                download_public(asset_url(entry, name), path)
                if file_digest(path) != expected:
                    raise SystemExit('Public asset checksum differs: ' + name)
            finally:
                path.unlink(missing_ok=True)
        report.update(stage='public', verification='published hashes plus same-job installed dual-target build')
        (out / 'public-verification.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report))
        return
    root = Path(os.environ['RUNNER_TEMP']) / ('mp-public 中文' if args.public else 'mp-draft 中文')
    if root.exists():
        raise SystemExit('Integration requires a fresh cache')
    root.mkdir()
    launcher = Path(os.environ['LOCALAPPDATA']) / 'MicroPixel/bin/micropixel.exe'
    env = dict(os.environ)
    env.update(MICROPIXEL_HOME=str(root), MICROPIXEL_SDK_INDEX_URL=asset_url(entry, 'test-sdk-index.json'))
    # Native tools must not accidentally find CI's Python/Git/CMake/LLVM/CRT.
    env['PATH'] = str(Path(env['WINDIR']) / 'System32')
    for key in ('WASI_SDK_PATH', 'WASI_CLANG', 'WASI_CLANGXX', 'WAMRC', 'XTENSA_WAMRC', 'PYTHONPATH', 'PYTHONHOME', 'PSMODULEPATH'):
        env.pop(key, None)
    # os.environ normalizes Windows keys to uppercase. Keep only the system
    # PowerShell 5.1 modules, not the parent PowerShell 7 module directory.
    powershell_root = Path(os.environ['WINDIR']) / 'System32/WindowsPowerShell/v1.0'
    env['PSMODULEPATH'] = str(powershell_root / 'Modules')
    subprocess.run([str(powershell_root / 'powershell.exe'), '-NoProfile', '-Command',
                    'Get-Command Get-FileHash -ErrorAction Stop | Select-Object -ExpandProperty Name'],
                   env=env, check=True, timeout=30)
    index = json.loads((out / 'test-sdk-index.json').read_text())
    if not args.public:
        (root / 'manifests').mkdir()
        (root / 'downloads').mkdir()
        for sdk_version, spec in index['versions'].items():
            manifest_path = out / spec['name']
            shutil.copyfile(manifest_path, root / 'manifests' / (sdk_version + '.json'))
            asset = json.loads(manifest_path.read_text())['sdk']
            shutil.copyfile(out / asset['name'], root / 'downloads' / asset['sha256'])
        shutil.copyfile(out / index['manager']['name'], root / 'downloads' / index['manager']['sha256'])
        (root / 'index-cache.json').write_text(json.dumps({'url': env['MICROPIXEL_SDK_INDEX_URL'], 'index': index,
            'checked_at': int(time.time()), 'attempted_at': int(time.time())}))
    else:
        # Independently fetch and verify every published small/SDK/installer asset.
        checksums = out / 'verification-sha256sums.txt'
        if not checksums.exists():
            checksums = out / 'sha256sums.txt'
        for line in checksums.read_text().splitlines():
            expected, name = line.split('  ', 1)
            path = root / name
            print('Checking public asset:', name, flush=True)
            download_public(asset_url(entry, name), path)
            if file_digest(path) != expected:
                raise SystemExit('Public release asset differs: ' + name)
            path.unlink()

    def invoke(*command):
        process = subprocess.run([str(launcher), *command, '--json'], env=env, cwd=root,
                                 capture_output=True, text=True, encoding='utf-8', timeout=900)
        print(process.stderr, file=sys.stderr, end='')
        result = json.loads(process.stdout)
        if process.returncode or not result.get('ok') or result.get('schema_version') != 1:
            raise RuntimeError(f'Installed command failed {command}: {result}')
        return result

    installed = invoke('setup', '--version', version, '--yes')['result']['paths']
    doctor = invoke('doctor', '--offline')
    if not doctor['result']['ready']:
        raise RuntimeError('Prepared tools did not pass doctor')
    project = root / '中文 游戏 & app'
    invoke('init', str(project), '--app-id', 'local.windows-verification', '--title', 'Windows Verification')
    lock_before = (project / 'micropixel.lock.json').read_bytes()
    source = project / 'src/main.cpp'
    source.write_text('#include \"cache_probe.hpp\"\n' + source.read_text(encoding='utf-8'), encoding='utf-8')
    header = project / 'src/cache_probe.hpp'
    header.write_text('#define CACHE_PROBE 1\n')
    for target in ('riscv32-ilp32f', 'xtensa'):
        invoke('build', str(project), '--aot-target', target, '--offline')
        invoke('package', str(project), '--aot-target', target, '--offline')
        # Bundle extensions are defined by the CLI; include all build files to
        # catch an unexpected full incremental rebuild regardless of extension.
        files = {p: p.stat().st_mtime_ns for p in (project / 'build').rglob('*') if p.is_file()}
        if not files:
            raise RuntimeError('No build artifacts were produced')
        invoke('package', str(project), '--aot-target', target, '--offline')
        if any(p.stat().st_mtime_ns != stamp for p, stamp in files.items()):
            raise RuntimeError('Incremental package rewrote build outputs')
    previous_aot = {p: p.stat().st_mtime_ns for p in (project / 'build').glob('*.aot')}
    header.write_text('#define CACHE_PROBE 2\n')
    invoke('package', str(project), '--aot-target', 'xtensa', '--offline')
    if not previous_aot or any(p.stat().st_mtime_ns == stamp for p, stamp in previous_aot.items()):
        raise RuntimeError('Changed header did not invalidate cached AOT')
    invoke('publish', str(project), '--dry-run', '--offline')
    if (project / 'micropixel.lock.json').read_bytes() != lock_before:
        raise RuntimeError('Build/package/preflight modified the project lock')
    # Exercise localization, audio generation and bundled resource files using
    # only the published SDK's copies of the build helpers.
    example = root / '素材 音效 示例'
    shutil.copytree(Path(installed['sdk']) / 'guest/apps/blocks', example)
    invoke('sdk', 'use', version, '--project', str(example), '--yes')
    for target in ('riscv32-ilp32f', 'xtensa'):
        invoke('package', str(example), '--aot-target', target, '--offline')
    if args.full_validation:
        fixture_directory = root / '真机 验收 App'
        fixture_directory.mkdir()
        extract(out / 'windows-acceptance-project.zip', fixture_directory)
        fixture = fixture_directory / 'windows-acceptance'
        invoke('sdk', 'use', version, '--project', str(fixture), '--yes')
        for target in ('riscv32-ilp32f', 'xtensa'):
            invoke('package', str(fixture), '--aot-target', target, '--offline')
        powershell = Path(os.environ['WINDIR']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
        command = [str(powershell), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                   str(out / 'test_acceptance_versions.ps1'), '-Launcher', str(launcher),
                   '-Directory', str(root / 'A B acceptance')]
        if not args.public:
            command.append('-OfflineFixtures')
        subprocess.run(command, env=env, check=True, timeout=900)
    report = {'schema_version': 1, 'ok': True, 'stage': 'public' if args.public else 'draft',
              'sdk_version': version, 'toolchain_id': doctor['result']['toolchain_id'],
              'targets': ['riscv32-ilp32f', 'xtensa'], 'windows_10_manual_acceptance': 'pending', 'validation': 'full' if args.full_validation else 'standard'}
    (out / ('public-verification.json' if args.public else 'draft-verification.json')).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
