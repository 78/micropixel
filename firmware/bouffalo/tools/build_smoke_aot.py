# SPDX-License-Identifier: Apache-2.0
"""Build the smoke Guest with clang and the compiler from the pinned WAMR tree."""
import argparse
from pathlib import Path
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clang', required=True)
    parser.add_argument('--wamrc', required=True)
    parser.add_argument('--wasm-ld', required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    wasm = args.output.with_suffix('.wasm')
    aot = args.output.with_suffix('.aot')
    subprocess.run([args.clang, '--target=wasm32', '--ld-path='+args.wasm_ld, '-O2', '-nostdlib',
                    '-Wl,--no-entry', '-Wl,--export=f64_add', '-Wl,--export=f64_div',
                    '-Wl,--export=grow', '-Wl,--initial-memory=65536',
                    '-Wl,--max-memory=196608', '-Wl,-z,stack-size=4096',
                    str(args.source), '-o', str(wasm)], check=True)
    subprocess.run([args.wamrc, '--target=riscv32', '--target-abi=ilp32f', '--cpu=generic-rv32',
                    '--cpu-features=+m,+a,+f,+c,+zicsr,+zifencei', '--bounds-checks=1',
                    '--disable-simd', '-o', str(aot), str(wasm)], check=True)
    data = aot.read_bytes()
    if data[:8] != b'\0aot' + struct.pack('<I', 6):
        raise ValueError('The smoke Guest requires the pinned AOT v6 compiler')
    for helper in (b'__adddf3', b'__divdf3'):
        if helper not in data:
            raise ValueError('RV32F AOT must reference ' + helper.decode())
    rows = [', '.join(f'0x{value:02x}' for value in data[start:start+16])
            for start in range(0, len(data), 16)]
    args.output.write_text('// Generated smoke AOT; do not commit.\n'
                           '_Alignas(4) static unsigned char smoke_aot[] = {\n'
                           + ',\n'.join(rows) + '\n};\n')
    print('Smoke AOT v6 references __adddf3 and __divdf3')


if __name__ == '__main__':
    main()
