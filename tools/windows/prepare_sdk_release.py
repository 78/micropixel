#!/usr/bin/env python3
"""Build release metadata and isolated A/B acceptance fixtures from one checkout."""
import argparse
import json
import re
import shutil
import sys
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_sdk_release as packager
from windows.release_metadata import archive as zip_archive, asset, file_digest, sdk, write, write_json


def prepare_core(out: Path, repository: str):
    metadata, archive = packager.build(ROOT)
    pin = json.loads((ROOT / 'tools/windows/release-channel.json').read_text())
    toolchain = out / 'toolchain.json'
    if not toolchain.exists() or file_digest(toolchain) != pin['sha256']:
        urllib.request.urlretrieve(pin['url'], toolchain)
    if file_digest(toolchain) != pin['sha256']:
        raise SystemExit('Published toolchain manifest checksum differs from release pin')
    write(out / metadata['archiveName'], archive)
    write_json(out / 'release.json', metadata)
    sdk(out, toolchain, out, repository)
    return metadata

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--repository', default='78/micropixel')
    args = parser.parse_args()
    out = args.directory.resolve()
    out.mkdir(parents=True, exist_ok=True)
    metadata = prepare_core(out, args.repository)
    version = metadata['version']
    policy = json.loads((ROOT / 'tools/windows/release-policy.json').read_text())
    base = f'https://github.com/{args.repository}/releases/download/sdk-v{version}'
    manifest = json.loads((out / 'sdk-manifest.json').read_text())
    entries = {version: {**asset(out / 'sdk-manifest.json', base, ''), 'release_notes_url': manifest['release_notes_url'], 'performance': []}}
    # These numeric versions are acceptance fixtures, never production channels.
    # Only the CLI identity changes; no invented performance or API claims.
    for fixture_version in ('9000.0.1', '9000.0.2'):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            for relative, content in packager.collect(ROOT).items():
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
            for relative in ('tools/micropixel', 'tools/build_app_bundle.py', 'tools/generate_localization.py', 'tools/analyze_sfx.py', 'LICENSE'):
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / relative, path)
            cli = source / 'tools/micropixel'
            cli.write_text(re.sub(r'^VERSION = "[0-9.]+"$', f'VERSION = "{fixture_version}"', cli.read_text(), flags=re.M))
            fixture_meta, fixture_bytes = packager.build(source)
        fixture_path = out / fixture_meta['archiveName']
        write(fixture_path, fixture_bytes)
        fixture_manifest = {**manifest, 'sdk_version': fixture_version, 'test_only': True,
                            'sdk': asset(fixture_path, base, 'micropixel-sdk-' + fixture_version)}
        manifest_path = out / f'test-sdk-{fixture_version}.json'
        write_json(manifest_path, fixture_manifest)
        entries[fixture_version] = {**asset(manifest_path, base, ''), 'release_notes_url': manifest['release_notes_url'], 'performance': []}
    with tempfile.TemporaryDirectory() as temporary:
        fixture_source = Path(temporary)
        for name in ('app.json', 'main.cpp'):
            shutil.copyfile(ROOT / 'tools/windows/fixtures/acceptance' / name, fixture_source / name)
        zip_archive(fixture_source, out / 'windows-acceptance-project.zip', 'windows-acceptance')
    runtime = json.loads((out / 'runtime/manager-build.json').read_text())
    manager_path = out / runtime['archive']
    shutil.copyfile(out / 'runtime' / runtime['archive'], manager_path)
    manager = {**asset(manager_path, base, 'manager'), 'version': runtime['version'], 'build_id': runtime['build_id']}
    with tempfile.TemporaryDirectory() as temporary:
        fixture_runtime = Path(temporary) / 'manager'
        shutil.copytree(out / 'runtime' / runtime['directory'], fixture_runtime)
        fixture_build = json.loads((fixture_runtime / 'build.json').read_text())
        fixture_build['build_id'] += '-acceptance'
        write_json(fixture_runtime / 'acceptance.json', {'test_only': True, 'purpose': 'Exercise version-directory switching; identical manager code'})
        (fixture_runtime / 'build.json').write_text(json.dumps(fixture_build, indent=2) + '\n')
        fixture_manager_path = out / ('micropixel-manager-' + fixture_build['build_id'] + '.zip')
        zip_archive(fixture_runtime, fixture_manager_path, 'manager')
    fixture_manager = {**asset(fixture_manager_path, base, 'manager'), 'version': runtime['version'], 'build_id': fixture_build['build_id']}
    write_json(out / 'test-sdk-index.json', {'schema_version': 1, 'test_only': True, 'stable': '9000.0.2', 'preview': None, 'versions': entries, 'manager': fixture_manager})
    write_json(out / 'channel-entry.json', {'schema_version': 1, 'version': version, 'entry': entries[version], 'manager': manager,
                                          'installer': json.loads((out / 'windows-installer.json').read_text())})
    write_json(out / 'release-notes.json', {'schema_version': 1, 'sdk_version': version, 'channel': policy['channel'],
        'performance': [], 'migration': ['Existing unmanaged projects must explicitly select an SDK before managed builds.',
        'Source migration and real-device acceptance remain separate from SDK switching.',
        'Run the new Windows installer when upgrading from 0.16.0 to replace its Ctrl-C bootstrap; manager or SDK updates alone do not replace it.'],
        'compatibility': manifest['compatibility'], 'windows_acceptance': policy['windows_acceptance'], 'code_signing': policy['code_signing'], 'release_policy': policy['policy']})
    for relative in ('docs/development/windows-sdk.zh-CN.md', 'docs/development/windows-acceptance.zh-CN.md',
                     'docs/development/windows-acceptance-2026-09-11.zh-CN.md',
                     'guest/sdk/AI.md', 'tools/windows/collect_acceptance.ps1', 'tools/windows/test_acceptance_versions.ps1', 'LICENSE', 'THIRD_PARTY_NOTICES.md'):
        shutil.copyfile(ROOT / relative, out / Path(relative).name)
    assets = sorted(path for path in out.iterdir() if path.is_file() and path.name not in ('inno-setup.exe', 'sha256sums.txt', 'release-notes.md'))
    write(out / 'sha256sums.txt', ''.join(file_digest(p) + '  ' + p.name + '\n' for p in assets).encode())
    print(version)


if __name__ == '__main__':
    main()
