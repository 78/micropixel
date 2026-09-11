#!/usr/bin/env python3
"""Wait for reusable Host jobs; never accept a partial or failed board set."""
import argparse
import json
import subprocess
import time
from firmware_artifacts import SOURCES

parser = argparse.ArgumentParser()
parser.add_argument('--run', required=True)
args = parser.parse_args()
if not args.run.isdigit():
    raise SystemExit('Expected numeric run ID')
repo = json.loads(subprocess.check_output(['gh', 'repo', 'view', '--json', 'nameWithOwner']))['nameWithOwner']
run = json.loads(subprocess.check_output(['gh', 'api', f'repos/{repo}/actions/runs/{args.run}']))
if run['path'] != '.github/workflows/firmware-build.yml' or run['head_repository']['full_name'] != repo:
    raise SystemExit('Reuse requires this repository’s firmware workflow')
expected = {'Host ' + name for name in SOURCES['profiles']}
for attempt in range(90):
    jobs = json.loads(subprocess.check_output(['gh', 'api', f'repos/{repo}/actions/runs/{args.run}/jobs?per_page=100']))['jobs']
    selected = {j['name']: j for j in jobs if j['name'] in expected}
    if any(j['conclusion'] not in (None, 'success') for j in selected.values()):
        raise SystemExit('A Host job failed; its artifacts cannot be reused')
    if set(selected) == expected and all(j['conclusion'] == 'success' for j in selected.values()):
        print('All five Host jobs succeeded; reusing their immutable artifacts')
        break
    time.sleep(10)
else:
    raise SystemExit('Timed out waiting for the complete Host artifact set')
