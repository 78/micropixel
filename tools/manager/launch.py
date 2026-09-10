"""Stable launcher: selected runtimes are immutable and never overwritten in place."""
import json
import os
import subprocess
import sys
from pathlib import Path


def main():
    initial = Path(__file__).resolve().parent
    activating = len(sys.argv) in (2, 3) and sys.argv[1] == '--activate-manager'
    root = Path(sys.argv[2]) if activating and len(sys.argv) == 3 else Path(os.environ.get('MICROPIXEL_HOME', initial.parent.parent))
    os.environ['MICROPIXEL_HOME'] = str(root)
    pointer = root / 'current.json'
    if activating:
        from micropixel_manager import atomic_json, install_lock
        with install_lock(root):
            atomic_json(pointer, {'directory': str(initial)})
        return 0
    selected = initial
    if pointer.exists():
        selected = Path(json.loads(pointer.read_text(encoding='utf-8'))['directory']).resolve()
        if not selected.is_relative_to(root.resolve()):
            raise SystemExit('Invalid manager directory: outside the installation root')
    # A new process loads the selected Python DLLs; the bootstrap runtime stays intact.
    return subprocess.call([str(selected / 'python/python.exe'), '-I', '-X', 'utf8', str(selected / 'micropixel_manager.py'), *sys.argv[1:]])


if __name__ == '__main__':
    raise SystemExit(main())
