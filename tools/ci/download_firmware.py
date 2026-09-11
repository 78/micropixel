#!/usr/bin/env python3
"""Download only successful same-repository builds and verify every board image."""
import argparse
import json
import subprocess
from pathlib import Path
from firmware_artifacts import SOURCES, PROFILES, check_files, check_image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--run', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit('Use a fresh artifact directory')
    repository = json.loads(subprocess.check_output(['gh', 'repo', 'view', '--json', 'nameWithOwner']))['nameWithOwner']
    run = json.loads(subprocess.check_output(['gh', 'api', f'repos/{repository}/actions/runs/{args.run}']))
    if run['conclusion'] != 'success' or run['path'] != '.github/workflows/firmware-build.yml' or run['head_repository']['full_name'] != repository:
        raise SystemExit('A successful firmware workflow in this repository is required')
    subprocess.run(['gh', 'run', 'download', args.run, '--name', 'firmware-release', '--dir', str(args.output)], check=True)
    manifest = json.loads((args.output / 'manifest.json').read_text())
    if manifest['source_commit'] != run['head_sha'] or set(manifest['profiles']) != set(SOURCES['profiles']):
        raise SystemExit('Artifact source or board set differs')
    check_files(args.output, manifest['files'])
    configs = set()
    for profile in SOURCES['profiles']:
        directory = args.output / profile
        board = json.loads((directory / 'manifest.json').read_text())
        if board['source_commit'] != manifest.get('host_source_commits', {}).get(profile, run['head_sha']) or board['firmware_version'] != manifest['firmware_version']:
            raise SystemExit('Board provenance mismatch')
        check_files(directory, board['files'])
        check_image(directory / 'micropixel.bin', PROFILES[profile]['target'], manifest['firmware_version'])
        configs.add(board['remote_configuration_sha256'])
    if len(configs) != 1:
        raise SystemExit('Board release configuration differs')
    print(json.dumps({'ok': True, 'version': manifest['firmware_version'], 'sdk': manifest['sdk_version'],
                      'source_commit': run['head_sha'], 'boards': len(manifest['profiles'])}))


if __name__ == '__main__':
    main()
