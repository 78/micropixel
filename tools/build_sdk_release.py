#!/usr/bin/env python3
"""Create the SDK once for GitHub and the documentation site.

Inputs must be a clean source export. This command has no network side effects.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import re
import subprocess
import tarfile
from pathlib import Path

DIRECTORIES = ('guest/sdk', 'guest/runtime', 'guest/abi', *(f'guest/apps/{name}' for name in
               ('sdk-demo', 'snake', 'maze-evil', 'blocks', 'tilt', 'tomb-explorer')))
WAMR_COMMIT = 'af07c787ac6f7d1d20555f97ddc184f5fc13731a'
MARKER = 'EMBEDDED_BUNDLE_BUILDER: str | None = None'


def collect(root: Path) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    tracked = None
    try:
        git_root = subprocess.check_output(['git', '-C', str(root), 'rev-parse', '--show-toplevel'], stderr=subprocess.DEVNULL, text=True).strip()
        if Path(git_root).resolve() == root.resolve():
            tracked = set(subprocess.check_output(['git', '-C', str(root), 'ls-files', '-z']).decode('utf-8').split('\0'))
    except (OSError, subprocess.CalledProcessError):
        pass  # Clean source archives do not contain a .git directory.
    for directory in DIRECTORIES:
        base = root / directory
        if not base.is_dir():
            raise ValueError(f'Missing SDK source directory: {directory}')
        for path in sorted(base.rglob('*')):
            relative = path.relative_to(root)
            if path.is_symlink():
                raise ValueError(f'Symlink is not allowed in SDK sources: {relative}')
            if any(p in ('build', '__pycache__', '.git') or p.startswith('.') for p in relative.parts):
                continue
            if tracked is not None and relative.as_posix() not in tracked:
                continue
            if path.is_file() and path.name != 'README.md' and path.suffix != '.pyc':
                files[relative.as_posix()] = path.read_bytes()
    for name in ('generate_localization.py', 'analyze_sfx.py'):
        files['libexec/' + name] = (root / 'tools' / name).read_bytes()
    return files


def build(root: Path) -> tuple[dict, bytes]:
    root = root.resolve()
    cli = (root / 'tools/micropixel').read_text(encoding='utf-8')
    match = re.search(r'^VERSION = "([0-9]+\.[0-9]+\.[0-9]+)"$', cli, re.M)
    if not match or cli.count(MARKER) != 1:
        raise ValueError('SDK CLI must declare its version and one embedded-builder marker')
    version = match[1]
    files = collect(root)
    builder = (root / 'tools/build_app_bundle.py').read_text(encoding='utf-8')
    cli = cli.replace(MARKER, 'EMBEDDED_BUNDLE_BUILDER: str | None = ' + repr(builder))
    files['micropixel'] = cli.encode('utf-8')
    files['LICENSE'] = (root / 'LICENSE').read_bytes()
    files['README.md'] = f'''# MicroPixel SDK {version}

Restricted C++23 Guest SDK, Runtime, ABI and six example games.

Start with [Quickstart](guest/sdk/QUICKSTART.md), then read
[Publishing](guest/sdk/PUBLISHING.md) and [Graphics](guest/sdk/GRAPHICS.md).

## Toolchain

Use WASI SDK 33 and MicroPixel WAMR fork commit `{WAMR_COMMIT}`:
https://github.com/78/wasm-micro-runtime (branch `wamr-host/esp-idf-psram`).
The required AOT format is **v6**. `wamrc --version` reports `wamrc 2.4.3`,
but that string alone does not establish compatibility.
Upstream WAMR 2.4.3, 2.4.4, and 2.4.5 releases emit
AOT format v5 and must not be substituted.

Set WASI_SDK_PATH, WAMRC (RISC-V) and XTENSA_WAMRC (ESP32-S3).
USB development also needs pyserial>=3.5,<4. Run the CLI using Python:

```sh
python micropixel init my-game --app-id local.my-game --title "My Game"
python micropixel --transport usb run my-game
```

`publish --dry-run` validates **both** architectures without uploading.
Real publishing requires separate developer authentication and authorization.

Full environment instructions: https://micropixel.ai/docs/environment/

Project sources are Apache-2.0; see LICENSE. WASI/LLVM and WAMR toolchains
are distributed separately and carry their own third-party licenses.
'''.encode('utf-8')
    prefix = f'micropixel-sdk-{version}'
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.USTAR_FORMAT) as archive:
        for name, content in sorted(files.items()):
            info = tarfile.TarInfo(prefix + '/' + name)
            info.size = len(content)
            info.mode = 0o755 if name == 'micropixel' else 0o644
            info.mtime = 0
            info.uname = info.gname = 'micropixel'
            archive.addfile(info, io.BytesIO(content))
    # GzipFile gives a stable header (including OS byte) on every supported host.
    compressed = io.BytesIO()
    with gzip.GzipFile(fileobj=compressed, mode='wb', filename='', mtime=0, compresslevel=9) as output:
        output.write(raw.getvalue())
    data = compressed.getvalue()
    metadata = {
        'version': version, 'archiveName': prefix + '.tar.gz',
        'sha256': hashlib.sha256(data).hexdigest(), 'sizeBytes': len(data),
        'fileCount': len(files), 'toolchain': {
            'wamrRepository': 'https://github.com/78/wasm-micro-runtime',
            'wamrBranch': 'wamr-host/esp-idf-psram', 'wamrCommit': WAMR_COMMIT,
            'wamrcReportedVersion': '2.4.3', 'aotFormatVersion': 6,
        },
    }
    return metadata, data


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--workspace-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    metadata, data = build(args.workspace_root)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    target = args.output_dir / metadata['archiveName']
    if target.exists() and target.read_bytes() != data:
        raise SystemExit('Refusing to replace an existing SDK version with different content')
    target.write_bytes(data)
    (args.output_dir / 'release.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    (args.output_dir / 'sha256sums.txt').write_text(f"{metadata['sha256']}  {metadata['archiveName']}\n")
    print(json.dumps(metadata))


if __name__ == '__main__':
    main()
