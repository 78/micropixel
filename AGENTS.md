# Repository instructions

Applies to the entire repository. Use source code, ABI headers, and executable tests as the authority for behavior.

## Architecture

MicroPixel runs WebAssembly apps on Espressif MCUs using ESP-IDF 6.1 and a pinned WAMR fork (AOT v6).
Guests use a restricted C++23 SDK and a single-threaded event loop; only one AppSession can exist at a time.
Supported boards are Metalio-Claw4, ESP-Mosaico, ESP32-S3-BOX-3, LCKFB SZPI, and M5Stack CoreS3.
Bundle v1 distributes apps; BundleFS v3 provides copy-on-write storage and power-loss recovery with
geometry derived from the block device (v1/v2 catalogs are imported in place).

Dependencies: `Runtime → Device contracts ← Platform`. `FirmwareApp` is the only composition root.
The Host owns hardware, app lifecycle, and system UI. Guests access capabilities through the Service ABI.

## Source map

Host paths below are relative to `firmware/espressif/main/`. Detailed design guides are currently in Chinese.

| Area | Source | Documentation |
|---|---|---|
| Build tools | `tools/` (repository root) | [Build and flashing](docs/development/flashing.zh-CN.md) |
| Host architecture | `device/`, `runtime/`, `platform/` | [Architecture](docs/design/architecture.zh-CN.md), [source guide](firmware/espressif/main/README.zh-CN.md) |
| Guest apps and SDK | `guest/apps/`, `guest/sdk/`, `guest/runtime/` (repository root) | [Guest](guest/README.zh-CN.md), [SDK](guest/sdk/README.zh-CN.md) |
| Wire protocol | `runtime/abi/`, `runtime/services/` | [ABI](guest/abi/README.zh-CN.md), [header](guest/abi/micropixel_abi.h) |
| System UI | `host/ui/`, `host/controller/` | [Source guide](firmware/espressif/main/README.zh-CN.md) |
| Graphics | `platform/graphics/`, `platform/lvgl/`, `runtime/graphics/` | [Performance](docs/development/graphics-performance.zh-CN.md) |
| Host memory | `platform/memory/` | [Architecture](docs/design/architecture.zh-CN.md) |
| App storage | `runtime/bundle/`, `runtime/bundlefs/` | [BundleFS](docs/design/bundlefs.zh-CN.md) |
| Audio | `guest/apps/<app>/audio/sfx.json` (repository root) | [Audio specification](docs/development/game-audio.zh-CN.md) |
| Boards | `platform/boards/` | [Flashing](docs/development/flashing.zh-CN.md), [S3 support](docs/development/esp32-s3-box-3-bring-up.zh-CN.md) |

## Boundaries

- Guest SDK, apps, and ABI must not depend on ESP-IDF, LVGL, or board types, or introduce threads, mutexes,
  system calls, or direct hardware access.
- Keep SDK-to-wire conversion in Guest Runtime. Extend Service methods, channels, or events before adding
  Core imports. Never repurpose published IDs or expose C++ layouts, STL types, or Host pointers through the C ABI.
- The Host validates pointers, lengths, handles, generations, ownership, and capacity independently of SDK checks.
- Real-time and cross-task Host paths use fixed-capacity queues, arrays, or pools; no implicit growth or detached tasks.
  Scene submission and Raster upload may explicitly grow PSRAM storage after validation. Allocation failure preserves
  old resources; drawing does not allocate; app termination releases storage.
- Host APIs prefer `string_view` and `span`. When Host code must own growable text or collections, use
  `PsramString`, `PsramVector`, or `PsramMap` so storage lands in PSRAM. Process-lifetime fixed-size Host
  objects use `MICROPIXEL_EXT_RAM_BSS` (empty when the target has no PSRAM BSS). Real-time paths still use
  fixed capacity. `PsramBuffer` is the fallible, explicitly sized trivially-copyable buffer.
- Host task entry points and their callees must not put large arrays, structs, or temporary copies on the stack.
  Keep fixed workspaces in task-owned PSRAM contexts (or process-lifetime PSRAM BSS with explicit ownership).
  Audit aggregate initialization, assignment, return-by-value and by-value arguments for hidden stack copies.
  Do not bypass stack-frame checks or simply increase task stacks to accommodate work buffers.
- Use move-only RAII or an explicit shutdown protocol. Destructors perform best-effort cleanup. Do not use raw
  new/delete for real-time resource ownership. Exceptions and RTTI are disabled.
- ISRs record minimal POD state and wake tasks; they never call WAMR, Guest code, or LVGL.
  Avoid large synchronous logs on Guest hot paths.
- The Host owns App Hall, status overlays, system gestures, brightness, and device master volume.
  Guests must not add app-wide master attenuation. Define game sound parameters only in `audio/sfx.json`.

## Workflow and validation

1. Run `git status --short`. Preserve unrelated and uncommitted changes.
2. Check existing contracts and tests; make the smallest correct change. Update design and regression coverage
   before changing architectural boundaries. Follow the [C/C++ style guide](docs/development/code-style.zh-CN.md).
3. Run the checks below. Compilation alone does not establish protocol, lifecycle, or hardware correctness.
4. Report behavior changes, key files, commands and results, and outstanding hardware checks. Keep docs current.

| Change | Required checks |
|---|---|
| Documentation | Relative links and `git diff --check` |
| Guest SDK / ABI | Build Guests for the selected target and run relevant conformance tests |
| Firmware / System Shell | Host tests, formatting, and the selected board’s `build-host`; no Guest or App Store rebuild for Host-only changes |
| Shared graphics / LVGL / PPA branches | Validate the selected target; add other boards only for a concrete affected platform branch or an explicit multi-board request |
| Bundle / integrated app | Relevant tests and the app's release Bundle; build Host only if changed |
| Sound effects | Analyzer unit tests, release Bundle, hardware A/B listening |
| Graphics performance | Guest and Host stage measurements plus visible output; CPU usage alone is insufficient |

Run Host tests only through `bash tools/tests/test_firmware_host.sh`, never by invoking clang++ directly.
Formatting: `bash tools/check_firmware_style.sh --format-only`. Before release or push, run checks relevant to the change and release targets.
Use the board specified by the user or established by the current hardware task. Do not default to P4 or build
unrelated boards. For S31 work, use `bash tools/s31.sh build-host` and, when requested, `flash-host`.
See [Contributing](CONTRIBUTING.md) for additional checks.

Activate ESP-IDF 6.1 via `export.sh` and configure WASI SDK and matching WAMRC before building.
A missing environment is not a Host build failure.
Host builds enforce stack-frame limits and emit GCC `.su` reports next to object files. For changes to
installation, signature verification, downloads or OTA, inspect the affected call chain and measure task
minimum free stack on the selected board after success and failure paths; a frame limit is not a total
call-stack bound. Record test results outside design docs; do not commit generated stack reports. After adding Kconfig symbols, inspect generated configuration.
Before hardware operations, read the flashing guide and identify the chip by MAC, not a re-enumerating port name.
Only one serial tool may own a device at a time.

## Repository hygiene

- Edit source JSON, assets, or generators. Do not edit or commit build outputs, artifacts, managed_components,
  generated sdkconfig, dependencies.lock.*, resource packs, AOT/Wasm/Bundle/Flash images, or generated reports.
- Update the pinned WAMR submodule only when requested. Do not copy its source into this repository or reformat
  third-party code. Check new dependency licenses and update [third-party notices](THIRD_PARTY_NOTICES.md).
- Never commit secrets, personal absolute paths, device identifiers, MAC addresses, raw serial logs, or one-off measurements.
- `guest/apps/mario/` is an optional, gitignored local benchmark; it must not become a required dependency.
- Default documentation filenames contain English; Simplified Chinese uses `.zh-CN.md`. Keep AGENTS.md in English.
  Write concise task guides and design contracts. Keep progress reports, conversations, and acceptance plans out of design docs.
