# SPDX-License-Identifier: Apache-2.0
"""Isolated console fixture: broadcast two real Ctrl-C events through two wrappers."""
import ctypes
import json
import os
from pathlib import Path
import signal
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.manager.process_control import run_child

depth, exit_code = int(sys.argv[1]), int(sys.argv[2])
marker = Path(sys.argv[3])

if depth == 0:
    interrupts = 0

    def interrupted(signum, frame):
        global interrupts
        interrupts += 1
        if interrupts == 2:
            print(json.dumps({'interrupts': interrupts, 'exit_code': exit_code}), flush=True)
            raise SystemExit(exit_code)
        marker.write_text('first')

    signal.signal(signal.SIGINT, interrupted)
    marker.write_text('ready')
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        time.sleep(0.05)
    raise SystemExit(93)


def send_interrupts():
    for expected in ('ready', 'first'):
        deadline = time.monotonic() + 10
        while not marker.exists() or marker.read_text() != expected:
            if time.monotonic() >= deadline:
                os._exit(91)
            time.sleep(0.01)
        # This process has its own console; event 0 broadcasts to its complete
        # process tree, never to the test runner or the user's terminal.
        if not ctypes.WinDLL('kernel32', use_last_error=True).GenerateConsoleCtrlEvent(0, 0):
            os._exit(92)


sender = None
if depth == 2:
    sender = threading.Thread(target=send_interrupts)
    sender.start()
result = run_child([sys.executable, '-X', 'utf8', __file__, str(depth - 1), str(exit_code), str(marker)])
if sender is not None:
    sender.join()
raise SystemExit(result.returncode)
