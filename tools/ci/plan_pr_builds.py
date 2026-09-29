#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Select compile checks from the complete PR diff, including deleted paths."""
import argparse
import json
import os
import subprocess

from plan_hosts import MATRIX
from firmware_artifacts import PROFILES, ROOT

BOARD_ROOT = 'firmware/espressif/main/platform/boards/'
BOARDS = {entry['profile']: entry['profile'] for entry in MATRIX}
BOARDS['esp32-s3-box-3'] = BOARDS.pop('esp-box-3')
S3 = {entry['profile'] for entry in MATRIX if entry['chip'] == 'esp32s3'}
WRAPPERS = {'tools/p4.sh': {'metalio-claw4'}, 'tools/s31.sh': {'esp-mosaico'}, 'tools/s3.sh': S3}


def select(paths):
    hosts = set()
    all_hosts = {entry['profile'] for entry in MATRIX}
    for path in paths:
        if path.endswith('.md') or path.startswith(('docs/', 'tools/tests/')):
            continue
        if path.startswith(BOARD_ROOT):
            board = path.removeprefix(BOARD_ROOT).split('/')[0]
            hosts.update({BOARDS[board]} if board in BOARDS else S3 if board == 'esp32-s3-common' else all_hosts)
        elif path.startswith('firmware/espressif/sdkconfig.'):
            hosts.update(profile for profile in all_hosts if path in PROFILES[profile]['sdkconfig_defaults'])
        elif path.startswith('firmware/'):
            hosts.update(all_hosts)
        elif path.startswith('guest/abi/') or path == 'guest/sdk/symbols.hpp':
            hosts.update(all_hosts)
        elif path in WRAPPERS:
            hosts.update(WRAPPERS[path])
        elif path in ('tools/firmware.py', 'tools/firmware_profiles.json',
                      'tools/generate_localization.py', 'tools/generate_builtin_fonts.py',
                      'tools/ci/firmware_artifacts.py', 'tools/ci/firmware-sources.json',
                      'tools/ci/plan_hosts.py', 'tools/ci/plan_pr_builds.py',
                      '.gitmodules', '.github/workflows/pr-build.yml') or path.startswith('tools/fonts/'):
            hosts.update(all_hosts)
    return [entry for entry in MATRIX if entry['profile'] in hosts]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--base', required=True)
    args = parser.parse_args()
    # The checkout is GitHub's merge commit: compare against its base, not only
    # the most recent contributor commit. --no-renames also checks removed paths.
    paths = subprocess.check_output(['git', 'diff', '--name-only', '--no-renames', '-z', args.base, 'HEAD'], cwd=ROOT).decode().split('\0')
    hosts = select(paths)
    values = {'hosts': hosts or MATRIX[:1], 'has_hosts': bool(hosts)}
    with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
        for key, value in values.items():
            output.write(f'{key}={json.dumps(value)}\n')
    summary = f"Host profiles: {', '.join(entry['profile'] for entry in hosts) or 'none (no Host build inputs changed)'}\n"
    print(summary)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as output:
            output.write('```text\n' + summary + '```\n')


if __name__ == '__main__':
    main()
