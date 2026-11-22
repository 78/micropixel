# Reproducible SDK setup

Run the commands from the MicroPixel repository root on Linux x86_64 or WSL.
Use Python 3.12+, Git, Make, CMake and the toolchains below. Serial control also
requires `pyserial` 3.5 in the Python environment that owns the port. The pinned
SDK includes its Kconfig and TOML Python modules. Its packaged Linux firmware
postprocessor requires a compatible glibc host; Ubuntu 24.04 is the tested host.

## Source and patches

The authoritative source lock is [sdk-source.json](../sdk-source.json):

- Repository: <https://github.com/bouffalolab/bouffalo_sdk.git>
- Commit: `eba87edced5a03ce8bcad8cf6002494982f83a5f`
- Public SDK contents are in one Git tree; no private submodule checkout is needed.
```sh
python3 firmware/bouffalo/tools/prepare_sdk.py --directory ../micropixel-bouffalo-sdk
export BL_SDK_BASE="$(cd ../micropixel-bouffalo-sdk && pwd)"
python3 firmware/bouffalo/tools/check_sdk.py --exact
```

Preparation uses a temporary sibling directory and installs it only after verification.
Repeating the command verifies an existing checkout without modifying it. `--exact`
rejects a different HEAD, undeclared tracked changes, untracked files, damaged
patches, or modified critical inputs. The SD adapter pins a patch that bounds SDH polling and card programming waits.
Use a fresh preparation directory when switching from the unpatched SDK checkout.

## Toolchains

The Linux x86_64 download URLs and SHA256 digests are also in `sdk-source.json`.
Download each archive, compare the complete SHA256 before extracting, and add
the directory containing its compiler executable to `PATH`.

| Purpose | Public release and asset | SHA256 |
| --- | --- | --- |
| Compile/link Host and SDK | [Zephyr SDK v1.0.0-rc1](https://github.com/zephyrproject-rtos/sdk-ng/releases/tag/v1.0.0-rc1), `toolchain_gnu_linux-x86_64_riscv64-zephyr-elf.tar.xz` | `9ed3eb07a7c47a49b5a6b87c8fa34c2fc81f71a112c3eca78ca7e88502d18677` |
| SDK DWARF `nm` and `objdump` | [Bouffalo toolchain](https://github.com/bouffalolab/toolchain/releases/tag/default), `gcc-riscv-14.3.0-linux-x86_64.tar.xz` | `93128c0da22cbbcf8473fae8c38b63232ebcd8d51969a14dc8491e77721b63a5` |

The Bouffalo release tag is mutable; the asset digest is the fixed input. If
its contents change, reject the download rather than accepting the new bytes.
Both compilers report GCC 14.3.0; the Host compiler identifies itself as
`Zephyr SDK 1.0.0-rc1`. This port requires the Zephyr C++23 standard library and
Picolibc; putting the Bouffalo toolchain on PATH does not change `CROSS_COMPILE`.

For example, after downloading the Zephyr archive:

```sh
echo '9ed3eb07a7c47a49b5a6b87c8fa34c2fc81f71a112c3eca78ca7e88502d18677  toolchain_gnu_linux-x86_64_riscv64-zephyr-elf.tar.xz' | sha256sum -c -
# Extract each verified archive into its own directory, then set actual bin paths.
export PATH=/path/to/zephyr/bin:/path/to/bouffalo/bin:$PATH
riscv64-zephyr-elf-g++ --version
riscv64-unknown-elf-nm --version
riscv64-unknown-elf-objdump --version
```
