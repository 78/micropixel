#!/usr/bin/env python3
"""Select only explicitly requested board rebuilds from the fixed release matrix."""
import json
import os
from firmware_artifacts import SOURCES

MATRIX = [
    dict(profile='metalio-claw4', chip='esp32p4', wrapper='p4.sh build-host'),
    dict(profile='esp-mosaico', chip='esp32s31', wrapper='s31.sh build-host'),
    dict(profile='esp-box-3', chip='esp32s3', wrapper='s3.sh build-host'),
    dict(profile='szpi-esp32s3', chip='esp32s3', wrapper='s3.sh build-szpi'),
    dict(profile='m5stack-cores3', chip='esp32s3', wrapper='s3.sh build-cores3'),
]
selected = set(filter(None, os.environ.get('REBUILD_PROFILES', '').split(',')))
if not selected.issubset(SOURCES['profiles']):
    raise SystemExit('Unknown rebuild profile')
reuse = os.environ.get('REUSE_RUN', '')
if reuse and not reuse.isdigit():
    raise SystemExit('Expected numeric run ID')
matrix = [entry for entry in MATRIX if not reuse or entry['profile'] in selected]
# A skipped matrix job still needs a nonempty expression during workflow expansion.
with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
    output.write('matrix=' + json.dumps(matrix or MATRIX[:1]) + '\n')
