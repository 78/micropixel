#!/usr/bin/env python3
"""Build the pinned Windows AOT compiler without requiring ESP-IDF."""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def run(*args: object) -> None:
    subprocess.run([str(x) for x in args], check=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--target', required=True, choices=['riscv32-ilp32f', 'xtensa'])
    args = parser.parse_args()
    lock_path = ROOT / 'tools/windows/toolchain-sources.json'
    lock = json.loads(lock_path.read_text())
    source = lock['llvm'][args.target]
    work = ROOT / 'build/windows' / args.target
    llvm = work / 'llvm-source'
    build = work / 'llvm-build'
    work.mkdir(parents=True, exist_ok=True)
    if not (llvm / '.git').exists():
        run('git', 'init', llvm)
        run('git', '-C', llvm, 'remote', 'add', 'origin', source['repository'])
    run('git', '-C', llvm, 'config', 'core.longpaths', 'true')
    run('git', '-C', llvm, 'fetch', '--depth=1', 'origin', source['commit'])
    run('git', '-C', llvm, 'checkout', '--detach', source['commit'])
    backend = source['backend']
    run('cmake', '-S', llvm / 'llvm', '-B', build, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
        '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
        '-DLLVM_USE_CRT_RELEASE=MT', '-DLLVM_ENABLE_ASSERTIONS=OFF', '-DLLVM_INCLUDE_TESTS=OFF',
        '-DLLVM_INCLUDE_BENCHMARKS=OFF', '-DLLVM_INCLUDE_EXAMPLES=OFF',
        '-DLLVM_ENABLE_ZLIB=OFF', '-DLLVM_ENABLE_ZSTD=OFF', '-DLLVM_ENABLE_LIBXML2=OFF',
        '-DLLVM_ENABLE_TERMINFO=OFF', '-DLLVM_BUILD_TOOLS=OFF',
        '-DLLVM_TARGETS_TO_BUILD=' + ('RISCV' if backend == 'RISCV' else ''),
        '-DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=' + ('Xtensa' if backend == 'Xtensa' else ''))
    run('cmake', '--build', build, '--target', 'llvm-libraries', '--parallel', '2')
    wamr = ROOT / 'firmware/espressif/components/wasm-micro-runtime'
    actual = subprocess.check_output(['git', '-C', str(wamr), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != lock['wamr_commit']:
        raise SystemExit('WAMR checkout differs from the toolchain lock')
    aot_build = work / 'wamrc-build'
    run('cmake', '-S', wamr / 'wamr-compiler', '-B', aot_build, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
        '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
        '-DWAMR_BUILD_PLATFORM=windows', '-DWAMR_BUILD_TARGET=AMD_64',
        '-DWAMR_BUILD_WITH_CUSTOM_LLVM=1', '-DWAMR_BUILD_SIMD=0',
        '-DLLVM_DIR=' + str(build / 'lib/cmake/llvm'))
    run('cmake', '--build', aot_build, '--parallel', '2')
    output = work / 'dist'
    output.mkdir(exist_ok=True)
    executable = output / 'wamrc.exe'
    shutil.copyfile(aot_build / 'wamrc.exe', executable)
    shutil.copyfile(wamr / 'LICENSE', output / 'LICENSE-WAMR')
    shutil.copyfile(llvm / 'llvm/LICENSE.TXT', output / 'LICENSE-LLVM')
    run(executable, '--version')
    (output / 'build-info.json').write_text(json.dumps({
        'target': args.target, 'wamr_commit': actual, 'llvm': source,
        'source_lock_sha256': hashlib.sha256(lock_path.read_bytes()).hexdigest(),
        'sha256': hashlib.sha256(executable.read_bytes()).hexdigest(),
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
