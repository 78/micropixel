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
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from windows.release_metadata import file_digest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--public', action='store_true')
    args = parser.parse_args()
    out = args.directory.resolve()
    entry = json.loads((out / 'channel-entry.json').read_text())
    version = entry['version']
    root = Path(os.environ['RUNNER_TEMP']) / ('mp-public' if args.public else 'mp-draft')
    if root.exists():
        raise SystemExit('Integration requires a fresh cache')
    root.mkdir()
    launcher = Path(os.environ['LOCALAPPDATA']) / 'MicroPixel/bin/micropixel.exe'
    env = dict(os.environ)
    env.update(MICROPIXEL_HOME=str(root), MICROPIXEL_SDK_INDEX_URL=entry['entry']['url'].rsplit('/', 1)[0] + '/test-sdk-index.json')
    # Native tools must not accidentally find CI's Python/Git/CMake/LLVM/CRT.
    env['PATH'] = str(Path(env['WINDIR']) / 'System32')
    for key in ('WASI_SDK_PATH', 'WASI_CLANG', 'WASI_CLANGXX', 'WAMRC', 'XTENSA_WAMRC', 'PYTHONPATH', 'PYTHONHOME'):
        env.pop(key, None)
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
        for line in (out / 'sha256sums.txt').read_text().splitlines():
            expected, name = line.split('  ', 1)
            path = root / name
            urllib.request.urlretrieve(entry['entry']['url'].rsplit('/', 1)[0] + '/' + name, path)
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

    invoke('setup', '--version', version, '--yes')
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
    powershell = Path(os.environ['WINDIR']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
    command = [str(powershell), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
               str(ROOT / 'tools/windows/test_acceptance_versions.ps1'), '-Launcher', str(launcher),
               '-Directory', str(root / 'A B acceptance')]
    if not args.public:
        command.append('-OfflineFixtures')
    subprocess.run(command, env=env, check=True, timeout=900)
    report = {'schema_version': 1, 'ok': True, 'stage': 'public' if args.public else 'draft',
              'sdk_version': version, 'toolchain_id': doctor['result']['toolchain_id'],
              'targets': ['riscv32-ilp32f', 'xtensa'], 'windows_10_manual_acceptance': 'pending'}
    (out / ('public-verification.json' if args.public else 'draft-verification.json')).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
