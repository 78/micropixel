#!/usr/bin/env python3
"""Compile real Guest examples using only downloaded Windows tools."""
from __future__ import annotations

import argparse
import hashlib
import importlib.machinery
import importlib.util
import json
import os
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--target', required=True, choices=['riscv32-ilp32f', 'xtensa'])
    args = parser.parse_args()
    work = ROOT / 'build/windows'
    wasi = json.loads((ROOT / 'tools/windows/toolchain-sources.json').read_text())['wasi']
    archive = work / 'wasi.tar.gz'
    if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != wasi['sha256']:
        urllib.request.urlretrieve(wasi['url'], archive)
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == wasi['sha256'], 'WASI checksum mismatch'
    with tarfile.open(archive) as package:
        package.extractall(work, filter='data')
    os.environ['WASI_SDK_PATH'] = str(work / wasi['directory'])
    os.environ['WAMRC'] = str(work / args.target / 'dist/wamrc.exe')
    os.environ['XTENSA_WAMRC'] = os.environ['WAMRC']
    loader = importlib.machinery.SourceFileLoader('micropixel_verification', str(ROOT / 'tools/micropixel'))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    cli = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = cli
    loader.exec_module(cli)

    def run(*arguments: object) -> None:
        subprocess.run([sys.executable, str(ROOT / 'tools/micropixel'), *map(str, arguments)], check=True)

    project = work / '中文 projects' / 'hello world'
    run('init', project, '--app-id', 'test.windows.hello', '--title', 'Windows Hello')
    projects = [project, *(ROOT / 'guest/apps' / name for name in
                ['sdk-demo', 'snake', 'maze-evil', 'blocks', 'tilt', 'tomb-explorer'])]
    for app in projects:
        output = work / args.target / 'packages' / app.name
        run('package', app, '--aot-target', args.target, '--output-dir', output)
        bundles = list(output.glob('*.bundle.bin'))
        assert len(bundles) == 1, (app, bundles)
        data = bundles[0].read_bytes()
        cli.validate_bundle(bundles[0])
        header = cli.BUNDLE_HEADER.unpack_from(data)
        found = False
        for index in range(header[7]):
            section = cli.BUNDLE_SECTION.unpack_from(data, header[4] + index * cli.BUNDLE_SECTION.size)
            if section[0] == 1:
                cli.validate_publication_aot(data[section[2]:section[2] + section[3]], section[9], section[10])
                assert section[10] == cli.AOT_TARGET_MASKS[args.target]
                found = True
        assert found
        # Second invocation must preserve the Bundle instead of recompiling.
        before = bundles[0].stat().st_mtime_ns
        run('package', app, '--aot-target', args.target, '--output-dir', output)
        assert bundles[0].stat().st_mtime_ns == before, 'incremental build did not reuse bundle'
    for name in ['stl', 'graphics_raster', 'graphics_protocol', 'direct_surface_present']:
        run('build', ROOT / 'guest/tests/conformance' / (name + '.cpp'),
            '--aot-target', args.target, '--output-dir', work / args.target / 'conformance' / name)
    print('Windows dual-stage compilation, AOT contract and incremental examples passed:', args.target)


if __name__ == '__main__':
    main()
