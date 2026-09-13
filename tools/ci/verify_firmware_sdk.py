#!/usr/bin/env python3
"""Require identical SDK build inputs when firmware reuses a published SDK.

Factory apps are built from the firmware checkout, not the SDK's example
copies. Markdown documentation is not a compiler or packaging input.
"""
import argparse
from pathlib import Path
import subprocess

SDK_PATHS = ('guest/sdk', 'guest/runtime', 'guest/abi', 'tools/micropixel',
             'tools/build_app_bundle.py', 'tools/generate_localization.py', 'tools/analyze_sfx.py')


def changed_inputs(root: Path, sdk_ref: str, firmware_ref: str = 'HEAD') -> list[str]:
    names = subprocess.check_output(
        ['git', '-C', str(root), 'diff', '--name-only', '--no-renames', '-z', sdk_ref, firmware_ref,
         '--', *SDK_PATHS], text=True).split('\0')
    return [name for name in names if name and not (name.startswith('guest/') and name.endswith('.md'))]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('sdk_ref')
    args = parser.parse_args()
    changes = changed_inputs(Path(__file__).resolve().parents[2], args.sdk_ref)
    if changes:
        raise SystemExit('Published SDK build inputs differ:\n' + '\n'.join(changes))
    print('Published SDK compiler/runtime/ABI and packaging inputs match; factory apps use firmware source.')


if __name__ == '__main__':
    main()
