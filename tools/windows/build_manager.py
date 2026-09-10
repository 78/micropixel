#!/usr/bin/env python3
"""Assemble the self-contained Windows CLI runtime from verified upstream inputs."""
import argparse
import hashlib
import json
import shutil
import sys
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from manager.micropixel_manager import VERSION, extract, file_digest
from windows.release_metadata import archive


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    pins = json.loads((ROOT / 'tools/windows/runtime-sources.json').read_text())
    sources = {'micropixel_manager.py': ROOT / 'tools/manager/micropixel_manager.py',
               'bootstrap-cli.py': ROOT / 'tools/micropixel', 'launch.py': ROOT / 'tools/manager/launch.py',
               'THIRD_PARTY_NOTICES.md': ROOT / 'THIRD_PARTY_NOTICES.md', 'LICENSE': ROOT / 'LICENSE'}
    identity = hashlib.sha256(json.dumps(pins, sort_keys=True).encode())
    for name, path in sorted(sources.items()):
        identity.update(name.encode())
        identity.update(path.read_bytes())
    build_id = VERSION + '-' + identity.hexdigest()[:16]
    args.output.mkdir(parents=True, exist_ok=True)
    runtime = args.output / build_id
    if runtime.exists():
        raise SystemExit('Use a fresh output directory; manager payloads are immutable')
    downloads = args.output / 'downloads'
    downloads.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.manager-', dir=args.output) as temporary:
        staging = Path(temporary) / build_id
        staging.mkdir()
        for component in ('python', 'pyserial'):
            spec = pins[component]
            path = downloads / (component + '.zip')
            if not path.exists() or file_digest(path) != spec['sha256']:
                urllib.request.urlretrieve(spec['url'], path)
            if path.stat().st_size != spec['size_bytes'] or file_digest(path) != spec['sha256']:
                raise SystemExit('Runtime dependency checksum mismatch: ' + component)
            destination = staging / 'python'
            if component == 'pyserial':
                destination = Path(temporary) / 'serial-source'
            destination.mkdir(parents=True, exist_ok=True)
            extract(path, destination)
            if component == 'pyserial':
                site = staging / 'python/Lib/site-packages'
                site.mkdir(parents=True)
                shutil.copytree(destination / spec['root'] / 'serial', site / 'serial')
                shutil.copyfile(destination / spec['root'] / 'LICENSE.txt', site / 'pyserial-LICENSE.txt')
        (staging / 'python/python313._pth').write_text('python313.zip\n.\n..\nLib/site-packages\n', encoding='utf-8')
        for name, path in sources.items():
            shutil.copyfile(path, staging / name)
        (staging / 'build.json').write_text(json.dumps({'schema_version': 1, 'version': VERSION, 'build_id': build_id, 'runtime': pins}, indent=2) + '\n')
        if not (staging / 'python/LICENSE.txt').is_file() or not (staging / 'python/Lib/site-packages/pyserial-LICENSE.txt').is_file():
            raise SystemExit('Runtime license files must be retained')
        shutil.move(staging, runtime)
    archive(runtime, args.output / ('micropixel-manager-' + build_id + '.zip'), 'manager')
    (args.output / 'manager-build.json').write_text(json.dumps({'version': VERSION, 'build_id': build_id, 'directory': runtime.name, 'archive': 'micropixel-manager-' + build_id + '.zip'}, indent=2) + '\n')
    print(json.dumps({'version': VERSION, 'build_id': build_id, 'directory': str(runtime)}))


if __name__ == '__main__':
    main()
