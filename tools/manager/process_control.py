# SPDX-License-Identifier: Apache-2.0
"""Keep Windows console interrupts owned by the foreground child command."""
import signal
import subprocess
import sys


def run_child(*args, **kwargs):
    if sys.platform != 'win32':
        return subprocess.run(*args, **kwargs)
    # Windows broadcasts Ctrl-C to every process attached to the console. The
    # child must handle it (and flush its output) before wrappers return its code.
    # Do not use SIG_IGN: Windows can inherit that disposition into the child.
    previous = signal.signal(signal.SIGINT, lambda signum, frame: None)
    try:
        return subprocess.run(*args, **kwargs)
    finally:
        signal.signal(signal.SIGINT, previous)
