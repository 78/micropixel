#!/usr/bin/env python3
"""Produce immutable archives and machine-readable release manifests."""
from __future__ import annotations
import argparse
import hashlib
import json
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
def file_digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open('rb') as stream:
        while block := stream.read(1024 * 1024):
            sha.update(block)
    return sha.hexdigest()


def identifier(value: str) -> str:
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', value):
        raise ValueError('Invalid SDK version')
    return value


def write(path: Path, data: bytes):
    if path.exists() and path.read_bytes() != data:
        raise ValueError(f'Refusing to overwrite immutable release asset: {path.name}')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def write_json(path: Path, value: dict):
    write(path, (json.dumps(value, indent=2, sort_keys=True) + '\n').encode())


def asset(path: Path, base: str, root: str) -> dict:
    return {'name': path.name, 'url': base + '/' + path.name, 'sha256': file_digest(path),
            'size_bytes': path.stat().st_size, 'root': root}


def archive(source: Path, destination: Path, prefix: str):
    import io
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', compression=zipfile.ZIP_DEFLATED) as output:
        for path in sorted(source.rglob('*')):
            if path.is_symlink():
                raise ValueError('Release archives cannot contain links')
            if path.is_file() and '__pycache__' not in path.parts:
                info = zipfile.ZipInfo(prefix + '/' + path.relative_to(source).as_posix(), date_time=(2020, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                output.writestr(info, path.read_bytes())
    write(destination, buffer.getvalue())


def toolchain(verified: Path, output: Path, repository: str):
    source_path = ROOT / 'tools/windows/toolchain-sources.json'
    sources = json.loads(source_path.read_text())
    source_sha = file_digest(source_path)
    crt_lock = ROOT / 'tools/windows/msvc-crt-sources.json'
    recipe_sha = file_digest(ROOT / 'tools/windows/build_toolchain.py')
    packaging_sha = hashlib.sha256((Path(__file__).read_bytes() + (ROOT / 'tools/windows/package_crt.ps1').read_bytes())).hexdigest()
    identity = hashlib.sha256((source_sha + recipe_sha + file_digest(crt_lock) + packaging_sha).encode()).hexdigest()
    toolchain_id = 'windows-x64-' + identity[:16]
    tag = 'toolchain-' + toolchain_id
    base = f'https://github.com/{repository}/releases/download/{tag}'
    packages = {}
    for target, key in [('riscv32-ilp32f', 'wamrc_riscv'), ('xtensa', 'wamrc_xtensa')]:
        directory = verified / ('wamrc-windows-x64-' + target)
        receipt = json.loads((directory / 'build-info.json').read_text())
        # The bootstrap verification predates .gitattributes; accept its CRLF
        # checkout digest only when it is exactly the same pinned source text.
        bootstrap_sha = hashlib.sha256(source_path.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')).hexdigest()
        if receipt['source_lock_sha256'] not in (source_sha, bootstrap_sha) or receipt['target'] != target or receipt['sha256'] != file_digest(directory / 'wamrc.exe'):
            raise ValueError('Compiler provenance differs from pinned source manifest')
        filename = f'wamrc-{toolchain_id}-{target}.zip'
        archive(directory, output / filename, 'wamrc')
        packages[key] = asset(output / filename, base, 'wamrc')
    crt_sources = json.loads(crt_lock.read_text())
    crt_directory = verified / 'msvc-crt'
    for name, pinned in crt_sources['files'].items():
        if file_digest(crt_directory / name) != pinned['sha256']:
            raise ValueError('MSVC runtime provenance mismatch: ' + name)
    crt_archive = output / ('msvc-crt-' + toolchain_id + '.zip')
    archive(crt_directory, crt_archive, 'crt')
    packages['msvc_crt'] = asset(crt_archive, base, 'crt')
    wasi = sources['wasi']
    # Size is obtained from the already verified official archive in the producer job.
    wasi_archive = verified / 'wasi.tar.gz'
    if file_digest(wasi_archive) != wasi['sha256']:
        raise ValueError('WASI archive checksum mismatch')
    packages['wasi'] = {'name': 'wasi-sdk-33', 'version': '33.0', 'url': wasi['url'], 'sha256': wasi['sha256'],
                        'size_bytes': wasi_archive.stat().st_size, 'root': wasi['directory']}
    write_json(output / 'toolchain.json', {'schema_version': 1, 'toolchain_id': toolchain_id,
                'source_lock_sha256': source_sha, 'build_recipe_sha256': recipe_sha, 'packaging_recipe_sha256': packaging_sha, 'msvc_crt': crt_sources, 'sources': sources, 'platforms': {'windows-x64': packages}})
    return tag


def sdk(sdk_directory: Path, toolchain_path: Path, output: Path, repository: str):
    metadata = json.loads((sdk_directory / 'release.json').read_text())
    version = identifier(metadata['version'])
    toolchain = json.loads(toolchain_path.read_text())
    if toolchain['source_lock_sha256'] != file_digest(ROOT / 'tools/windows/toolchain-sources.json'):
        raise ValueError('SDK toolchain does not match pinned source manifest')
    base = f'https://github.com/{repository}/releases/download/sdk-v{version}'
    sdk_asset = asset(sdk_directory / metadata['archiveName'], base, 'micropixel-sdk-' + version)
    if sdk_asset['sha256'] != metadata['sha256']:
        raise ValueError('SDK archive checksum mismatch')
    write_json(output / 'sdk-manifest.json', {'schema_version': 1, 'sdk_version': version, 'minimum_manager_version': '0.1.0',
        'toolchain_id': toolchain['toolchain_id'], 'sdk': sdk_asset, 'platforms': toolchain['platforms'],
        'compatibility': {'lock_schema': 1, 'aot_version': 6, 'wamr_commit': toolchain['sources']['wamr_commit']},
        'release_notes_url': f'https://github.com/{repository}/releases/tag/sdk-v{version}'})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('kind', choices=['toolchain', 'sdk'])
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--toolchain', type=Path)
    parser.add_argument('--repository', default='78/micropixel')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.kind == 'toolchain':
        print(toolchain(args.input, args.output, args.repository))
    else:
        sdk(args.input, args.toolchain, args.output, args.repository)


if __name__ == '__main__':
    main()
