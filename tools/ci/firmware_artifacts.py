#!/usr/bin/env python3
"""Build once per architecture, assemble per board, and validate release artifacts."""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = json.loads((ROOT / 'tools/ci/firmware-sources.json').read_text())
PROFILES = json.loads((ROOT / 'tools/firmware_profiles.json').read_text())
CHIP_IDS = {'esp32p4': 18, 'esp32s31': 32, 'esp32s3': 9}
REMOTE_KEYS = ['MICROPIXEL_REMOTE_CONTROL_HOST', 'MICROPIXEL_REMOTE_CONTROL_PORT',
               'MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS', 'MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64']


def run(*args, env=None):
    subprocess.run(list(map(str, args)), check=True, env=env)


def versions():
    return (re.search(r'set\(PROJECT_VER "([^"]+)"', (ROOT / 'firmware/espressif/CMakeLists.txt').read_text())[1],
            re.search(r'^VERSION = "([^"]+)"', (ROOT / 'tools/micropixel').read_text(), re.M)[1])


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def inventory(directory):
    return {str(p.relative_to(directory)).replace('\\', '/'): {'sha256': digest(p), 'size': p.stat().st_size}
            for p in sorted(directory.rglob('*')) if p.is_file() and p != directory / 'manifest.json'}


def check_files(directory, files):
    for name, expected in files.items():
        path = directory / name
        if path.resolve().is_relative_to(directory.resolve()) is False or path.is_symlink():
            raise ValueError('Unsafe artifact path')
        if path.stat().st_size != expected['size'] or digest(path) != expected['sha256']:
            raise ValueError('Artifact checksum mismatch: ' + name)


def check_image(path, target, version):
    data = path.read_bytes()
    if len(data) > 0x380000 or len(data) < 256 or data[0] != 0xE9:
        raise ValueError('Invalid OTA image or OTA slot overflow')
    if struct.unpack_from('<H', data, 12)[0] != CHIP_IDS[target]:
        raise ValueError('Firmware target chip mismatch')
    if struct.unpack_from('<I', data, 32)[0] != 0xABCD5432 or data[48:80].split(b'\0')[0].decode() != version:
        raise ValueError('Firmware app descriptor version mismatch')


def guest(output, launcher):
    version, sdk_version = versions()
    prepared = json.loads(subprocess.check_output(
        [str(launcher), 'setup', '--version', sdk_version, '--yes', '--json', '--offline'], text=True))
    if not prepared.get('ok') or prepared['result']['sdk_version'] != sdk_version:
        raise ValueError('Verified SDK toolchain setup failed')
    paths = prepared['result']['paths']
    environment = os.environ.copy()
    for key in ('WASI_SDK_PATH', 'WAMRC', 'XTENSA_WAMRC'):
        environment[key] = paths[key]
    environment['WASI_CLANG'] = ''
    environment['WASI_CLANGXX'] = str(Path(paths['WASI_SDK_PATH']) / 'bin/clang++.exe')
    # The workflow verifies these SDK sources against the published SDK. Build
    # with the checkout CLI so apps/... headers also come from this checkout,
    # rather than the installed SDK's older example copies.
    for target in ('riscv32-ilp32f', 'xtensa'):
        bundles = []
        for app in SOURCES['guest_apps']:
            project = ROOT / 'guest/apps' / app
            bundle = output / target / (app + '.bundle.bin')
            bundle.parent.mkdir(parents=True, exist_ok=True)
            run(sys.executable, ROOT / 'tools/micropixel', 'package', project, '--aot-target', target,
                '--output', bundle, '--json', env=environment)
            bundles.append(bundle)
        for size in ((8, 24) if target == 'riscv32-ilp32f' else (8,)):
            run(sys.executable, ROOT / 'tools/build_app_store_image.py', '--app-store-size', size * 1024 * 1024,
                '--output', output / target / f'app-store-{size}m.bin', *bundles)
    write(output / 'manifest.json', {'source_commit': os.environ['GITHUB_SHA'], 'firmware_version': version,
          'sdk_version': sdk_version, 'toolchain_id': prepared['result']['toolchain_id'], 'files': inventory(output)})


def collect(profile, output):
    spec = PROFILES[profile]
    build = ROOT / spec['build_dir']
    version, sdk = versions()
    configuration = (build / 'sdkconfig.release').read_text()
    actual = {}
    for key in REMOTE_KEYS:
        match = re.search(r'^CONFIG_' + key + r'=(.*)$', configuration, re.M)
        value = match[1] if match else 'n'
        if value.startswith('"'):
            value = json.loads(value)
        if value != os.environ.get(key):
            raise ValueError('Release configuration differs: ' + key)
        actual[key] = value
    if not actual[REMOTE_KEYS[0]]:
        raise ValueError('Release endpoint is empty')
    flash = json.loads((build / 'flasher_args.json').read_text())
    for relative in flash['flash_files'].values():
        source = (build / relative).resolve()
        if not source.is_relative_to(build.resolve()):
            raise ValueError('Flash file outside build directory')
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    shutil.copyfile(build / 'flasher_args.json', output / 'flasher_args.json')
    check_image(output / 'micropixel.bin', spec['target'], version)
    write(output / 'manifest.json', {'profile': profile, 'target': spec['target'], 'source_commit': os.environ['GITHUB_SHA'],
          'firmware_version': version, 'sdk_version': sdk, 'idf_commit': SOURCES['esp_idf_commit'],
          'remote_configuration_sha256': hashlib.sha256(json.dumps(actual, sort_keys=True).encode()).hexdigest(),
          'files': inventory(output)})


def matching_source(commit):
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('Invalid source commit')
    if commit != os.environ['GITHUB_SHA']:
        run('git', 'fetch', 'origin', commit, '--depth=1')
        run('git', 'diff', '--exit-code', commit, 'HEAD', '--', 'guest', 'firmware', 'tools/micropixel',
            'tools/build_app_bundle.py', 'tools/generate_localization.py', 'tools/analyze_sfx.py')


def assemble(inputs, guests, output):
    version, sdk = versions()
    guest_manifest = json.loads((guests / 'manifest.json').read_text())
    matching_source(guest_manifest['source_commit'])
    if guest_manifest['sdk_version'] != sdk:
        raise ValueError('Guest source commit or SDK mismatch')
    check_files(guests, guest_manifest['files'])
    configs = set()
    host_sources = {}
    for profile in SOURCES['profiles']:
        source = inputs / profile
        metadata = json.loads((source / 'manifest.json').read_text())
        matching_source(metadata['source_commit'])
        host_sources[profile] = metadata['source_commit']
        if metadata['firmware_version'] != version:
            raise ValueError('Host source commit or version mismatch')
        check_files(source, metadata['files'])
        configs.add(metadata['remote_configuration_sha256'])
        target = PROFILES[profile]['target']
        aot = 'xtensa' if target == 'esp32s3' else 'riscv32-ilp32f'
        size = 24 if target == 'esp32p4' else 8
        run(sys.executable, ROOT / 'tools/build_full_firmware_image.py', '--build-dir', source,
            '--app-store-image', guests / aot / f'app-store-{size}m.bin', '--output', source / 'micropixel-full.bin')
        destination = output / profile
        destination.mkdir(parents=True, exist_ok=True)
        for name in ('micropixel.bin', 'micropixel-full.bin'):
            shutil.copyfile(source / name, destination / name)
        check_image(destination / 'micropixel.bin', target, version)
        full = (destination / 'micropixel-full.bin').read_bytes()
        ota = (destination / 'micropixel.bin').read_bytes()
        # The flasher intentionally omits unused flash tail bytes.
        if not 0x30000 + len(ota) <= len(full) <= (32 if target == 'esp32p4' else 16) * 1024 * 1024 or full[0x30000:0x30000 + len(ota)] != ota:
            raise ValueError('Full firmware size or embedded OTA mismatch')
        metadata['files'] = inventory(destination)
        write(destination / 'manifest.json', metadata)
    if len(configs) != 1:
        raise ValueError('Boards use different release configuration')
    write(output / 'manifest.json', {'source_commit': os.environ['GITHUB_SHA'], 'firmware_version': version,
          'sdk_version': sdk, 'profiles': SOURCES['profiles'], 'guest_source_commit': guest_manifest['source_commit'],
          'host_source_commits': host_sources, 'files': inventory(output)})


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest='action', required=True)
    p = sub.add_parser('guest'); p.add_argument('--launcher', required=True); p.add_argument('--output', type=Path, required=True)
    p = sub.add_parser('collect'); p.add_argument('--profile', choices=SOURCES['profiles'], required=True); p.add_argument('--output', type=Path, required=True)
    p = sub.add_parser('assemble'); p.add_argument('--inputs', type=Path, required=True); p.add_argument('--guests', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.action == 'guest': guest(args.output.resolve(), args.launcher)
    elif args.action == 'collect': collect(args.profile, args.output.resolve())
    else: assemble(args.inputs.resolve(), args.guests.resolve(), args.output.resolve())
