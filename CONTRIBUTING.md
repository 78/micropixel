# Contributing

Keep Guest APIs independent of chips and boards. Follow the [C/C++ style guide](docs/development/code-style.zh-CN.md)
(Chinese) and include the relevant checks and results in your pull request.

## Checks

```sh
# Guest changes: use the selected target’s Guest build entry point.
bash tools/tests/test_firmware_host.sh
bash tools/check_firmware_style.sh --format-only
python3 -m unittest tools.tests.test_analyze_sfx -v
bash -n tools/*.sh
```

Run checks appropriate to the change and selected target. Firmware changes require that board’s
`build-host` and relevant hardware checks (for example, `bash tools/s31.sh build-host` for S31).
Do not default to P4 or build unrelated boards. Add cross-board checks only for a concrete affected
platform branch or an explicit multi-board request. Before release or push, validate the release targets.

Host tests run through the wrapper above and reuse unchanged binaries. Use `HOST_TEST_REBUILD=1` to rebuild them.
Add tests to an existing suite unless a separate target needs different compilation, fixtures, or fault injection.

## Documentation

Use English for default filenames and `.zh-CN.md` for Simplified Chinese.
Keep the README focused on what the project does and how to start. Link to detailed guides instead of repeating them.
Design documents describe mechanisms and contracts; testing and release procedures belong in development guides.
Check relative links and run `git diff --check` for documentation changes.

## Assets and dependencies

- Define game sounds in `audio/sfx.json`; follow the [audio specification](docs/development/game-audio.zh-CN.md).
- Project-authored code defaults to Apache-2.0. Use `SPDX-License-Identifier: Apache-2.0` in new source files.
- Check third-party licenses, preserve attribution, and update [third-party notices](THIRD_PARTY_NOTICES.md).
- Link to vendor hardware documents; include copies only when redistribution is permitted.
- Do not commit secrets, personal paths, device identifiers, raw logs, one-off measurements, or build outputs.

[简体中文](CONTRIBUTING.zh-CN.md)
