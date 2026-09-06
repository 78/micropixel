#!/usr/bin/env python3
"""Cache Host test compilation, never test execution (Clang/GCC driver arguments)."""

import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def build(command: list[str]) -> bool:
    """Return whether compilation was needed; propagate compiler failures."""
    output_index = command.index("-o") + 1
    output = Path(command[output_index]).absolute()
    output.parent.mkdir(parents=True, exist_ok=True)
    cache = output.with_name(output.name + ".build.json")
    # Concurrent invocations must not publish mismatched binaries and signatures.
    with output.with_name(output.name + ".build.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return build_locked(command, output_index, output, cache)


def build_locked(command: list[str], output_index: int, output: Path, cache: Path) -> bool:
    # Ask the compiler on EVERY run: this also detects newly added headers that
    # shadow an old include, forced includes, SDK headers and changed include paths.
    dependency_command = command[: output_index - 1] + command[output_index + 1 :]
    dependencies = subprocess.check_output(
        dependency_command + ["-M", "-MT", "host-test"], text=True
    )
    paths = set()
    for rule in dependencies.replace("\\\n", "").splitlines():
        target, separator, prerequisites = rule.partition(":")
        if separator and target == "host-test":
            paths.update(shlex.split(prerequisites.replace("$$", "$")))
    if not paths:
        raise ValueError("compiler produced no Host test dependencies")

    digest = hashlib.sha256()
    version = subprocess.check_output([command[0], "--version"])
    compiler = Path(command[0]).resolve()
    identity = {
        "command": command,
        "cwd": str(Path.cwd()),
        "compiler": str(compiler),
        "compiler_stat": [compiler.stat().st_size, compiler.stat().st_mtime_ns],
        "environment": {key: value for key, value in os.environ.items() if key not in {
            "_", "SHLVL", "OLDPWD", "PWD", "MICROPIXEL_TEST_LOCALE",
            "MICROPIXEL_EXPECT_DISPLAY_NAME", "MICROPIXEL_EXPECT_METADATA_SCHEMA",
            "MICROPIXEL_EXPECT_PACKAGE_TYPE", "HOST_TEST_REBUILD",
        }},
        "cache_version": 1,
    }
    digest.update(json.dumps(identity, sort_keys=True).encode())
    digest.update(version)
    for name in sorted(paths):
        digest.update(name.encode() + b"\0")
        digest.update(hashlib.sha256(Path(name).read_bytes()).digest())
    signature = digest.hexdigest()
    try:
        if os.environ.get("HOST_TEST_REBUILD") != "1" and output.is_file() and cache.read_text() == signature:
            return False
    except FileNotFoundError:
        pass

    print(f"Building {output.name}", flush=True)
    # Never leave a failed/partial compilation eligible for a future cache hit.
    cache.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix=output.name + ".", dir=output.parent) as staging:
        temporary = Path(staging) / output.name
        compile_command = list(command)
        compile_command[output_index] = str(temporary)
        subprocess.run(compile_command, check=True)
        temporary.replace(output)
    cache.write_text(signature)
    return True


if __name__ == "__main__":
    try:
        build(sys.argv[1:])
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
