#!/usr/bin/env python3
"""Build the unsigned Preview installer. Signing/stable promotion is a separate gate."""
import argparse
import json
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from windows.release_metadata import asset
from manager.micropixel_manager import file_digest


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--repository', default='78/micropixel')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    version = re.search(r'^VERSION = "([0-9.]+)"$', (ROOT / 'tools/micropixel').read_text(), re.M)[1]
    run(sys.executable, ROOT / 'tools/windows/build_manager.py', '--output', output / 'runtime')
    build = json.loads((output / 'runtime/manager-build.json').read_text())
    build_id = build['build_id']
    generated = output / 'generated'
    generated.mkdir(exist_ok=True)
    (generated / 'bootstrap_config.h').write_text('#define BOOTSTRAP_ID L"' + build_id + '"\n')
    launcher = output / 'runtime/micropixel.exe'
    run('cl', '/nologo', '/O2', '/MT', '/W4', '/WX', '/std:c11', '/DUNICODE', '/D_UNICODE',
        '/I' + str(generated), '/Fo' + str(generated / 'launcher.obj'),
        '/Fe' + str(launcher), ROOT / 'tools/windows/launcher.c')
    spec = json.loads((ROOT / 'tools/windows/installer-sources.json').read_text())['inno_setup']
    compiler_installer = output / 'inno-setup.exe'
    urllib.request.urlretrieve(spec['url'], compiler_installer)
    if compiler_installer.stat().st_size != spec['size_bytes'] or file_digest(compiler_installer) != spec['sha256']:
        raise SystemExit('Inno Setup checksum mismatch')
    compiler = output / 'inno'
    run(compiler_installer, '/VERYSILENT', '/SUPPRESSMSGBOXES', '/SP-', '/NORESTART', '/NOICONS', '/DIR=' + str(compiler))
    run(compiler / 'ISCC.exe', '/DSdkVersion=' + version, '/DManagerBuild=' + build_id,
        '/DPayloadDir=' + str(output / 'runtime'), '/DReleaseDir=' + str(output), ROOT / 'tools/windows/micropixel.iss')
    installer = output / f'micropixel-setup-{version}-windows-x64-preview.exe'
    metadata = asset(installer, f'https://github.com/{args.repository}/releases/download/sdk-v{version}', '')
    metadata.pop('root')
    metadata.update(schema_version=1, version=version, architecture='windows-x64', preview=True,
                    manager_version=build['version'], manager_build_id=build_id)
    (output / 'windows-installer.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata))


if __name__ == '__main__':
    main()
