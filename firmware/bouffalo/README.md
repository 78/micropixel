# Bouffalo build foundation

This directory introduces external SDK build profiles for BL616CL and BL618DG B0 AP.
Each profile currently builds a minimal SDK smoke executable that initializes the
vendor board and prints a UART banner. Full MicroPixel firmware composition and
board display, input, audio and storage adapters are separate follow-ups.

## Build

Use Linux x86_64 or WSL, Python 3.12+, Git, Make and CMake 3.20+.
Follow [SDK setup](docs/sdk.md) for pinned source and verified toolchains, then:

```sh
python3 firmware/bouffalo/tools/prepare_sdk.py --directory ../micropixel-bouffalo-sdk
export BL_SDK_BASE="$(cd ../micropixel-bouffalo-sdk && pwd)"
# Add the verified Zephyr and Bouffalo toolchain bin directories to PATH first.
make -C firmware/bouffalo/boards/bl616cl
make -C firmware/bouffalo/boards/bl618dg
```

`cmake/sdk.cmake` verifies SDK provenance before configuring either target. Both
use RV32 ILP32F, C++23, disabled exceptions and RTTI. Unused prebuilt multimedia
and PKA libraries are excluded because the SDK's DG variants use ILP32D. The
linker continues to enforce ABI compatibility. Outputs stay inside each board's
ignored `build/`; `CONFIG_SKIP_COMBINE` produces raw images without installation
partition layouts. These foundation targets are compile/link checks, not desktop
release images. Use `BUILD_DIR=build-other` for a fresh output directory.

## Source dependencies

`dependencies.json` pins the same WAMR revision/AOT v6 as the Espressif submodule.
The separate ignored source directory lets the Bouffalo build work without an
ESP-IDF component checkout. The submodule revision is unchanged.

```sh
python3 firmware/bouffalo/tools/prepare_deps.py
python3 firmware/bouffalo/tools/prepare_deps.py --check
```

Downloads are checksum-verified before extraction and installation. Existing
source trees are checked against the manifest's file fingerprints and are never
overwritten. That check covers the listed critical files, not every local file;
use a freshly prepared tree for release builds. No dependencies or toolchain
binaries are committed.

## Checks

```sh
MICROPIXEL_HOST_TEST_PLATFORM=bouffalo bash tools/tests/test_firmware_host.sh
MICROPIXEL_HOST_TEST_PLATFORM=bouffalo bash tools/check_firmware_style.sh --format-only
git diff --check
```

The tests use temporary local Git repositories and source archives without
network access. They cover repeat preparation, wrong revision, source changes,
untracked files, damaged patches and failed dependency checksum rejection. The platform selector leaves the existing Espressif
checks as the default.
