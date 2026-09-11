#!/usr/bin/env python3
"""Wait for reusable Host jobs; never accept a partial or failed board set."""
import argparse
import json
import subprocess
import time
from firmware_artifacts import SOURCES

parser = argparse.ArgumentParser()
parser.add_argument('--run', required=True)
parser.add_argument('--exclude', default='')
args = parser.parse_args()
excluded = set(filter(None, args.exclude.split(',')))
if not excluded.issubset(SOURCES['profiles']):
    raise SystemExit('Unknown profile')
if not args.run.isdigit():
    raise SystemExit('Expected numeric run ID')
repo = json.loads(subprocess.check_output(['gh', 'repo', 'view', '--json', 'nameWithOwner']))['nameWithOwner']
run = json.loads(subprocess.check_output(['gh', 'api', f'repos/{repo}/actions/runs/{args.run}']))
if run['path'] != '.github/workflows/firmware-build.yml' or run['head_repository']['full_name'] != repo:
    raise SystemExit('Reuse requires this repository’s firmware workflow')
expected = {'Host ' + name for name in SOURCES['profiles'] if name not in excluded}
for attempt in range(90):
    jobs = json.loads(subprocess.check_output(['gh', 'api', f'repos/{repo}/actions/runs/{args.run}/jobs?per_page=100']))['jobs']
    selected = {j['name']: j for j in jobs if j['name'] in expected}
    if any(j['conclusion'] not in (None, 'success') for j in selected.values()):
        raise SystemExit('A Host job failed; its artifacts cannot be reused')
    if set(selected) == expected and all(j['conclusion'] == 'success' for j in selected.values()):
        print('All selected Host jobs succeeded; reusing their immutable artifacts')
        break
    time.sleep(10)
else:
    raise SystemExit('Timed out waiting for the complete Host artifact set')
