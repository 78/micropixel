# Windows SDK automation

[简体中文](https://github.com/78/micropixel/blob/main/guest/sdk/AI.zh-CN.md)

Use the managed Windows installer for `setup`, `doctor`, and SDK version management.
Select a version from the dedicated SDK channel, not the repository's mixed Latest Release.
The example pins SDK 0.17.0. Its installer is unsigned; Windows 10 acceptance is incomplete.

## Install

Install only when the user has requested it. Verify metadata, size, and SHA-256 before execution.

```powershell
# This explicit version uses the unsigned stable installer. Do not infer it from Latest Release.
$metadataUrl = 'https://github.com/78/micropixel/releases/download/sdk-v0.17.0/windows-installer.json'
$meta = Invoke-RestMethod -Uri $metadataUrl
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
if (([Uri]$meta.url).Scheme -ne 'https') { throw 'HTTPS download required' }
if ($meta.architecture -ne 'windows-x64') { throw 'Unsupported architecture' }
$installer = Join-Path $env:TEMP 'micropixel-setup.exe'
Invoke-WebRequest -UseBasicParsing -Uri $meta.url -OutFile $installer
if ((Get-Item $installer).Length -ne $meta.size_bytes) { throw 'Size mismatch' }
if ((Get-FileHash $installer -Algorithm SHA256).Hash.ToLowerInvariant() -ne $meta.sha256) { throw 'SHA-256 mismatch' }
$log = Join-Path $env:TEMP 'micropixel-install.log'
$p = Start-Process -FilePath $installer -ArgumentList "/VERYSILENT /SUPPRESSMSGBOXES /SP- /NORESTART /LOG=`"$log`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "Installer failed: $($p.ExitCode)" }
$mp = "$env:LOCALAPPDATA\MicroPixel\bin\micropixel.exe"
$setup = & $mp setup --version $meta.version --yes --json | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or -not $setup.ok) { throw 'Environment preparation failed; see stderr' }
$doctor = & $mp doctor --json | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or -not $doctor.ok -or -not $doctor.result.ready) { throw 'Environment is not ready' }
```

Wait for installation to exit, then check `doctor.result.ready`. The current terminal's PATH may be stale.
Do not disable SmartScreen, antivirus, or certificate validation. Report system blocks for manual resolution.

## Commands

```text
micropixel doctor --json
micropixel sdk status --check --json
micropixel build --aot-target riscv32-ilp32f --json
micropixel package --aot-target riscv32-ilp32f --json
micropixel publish --dry-run --json
```

Use `xtensa` for S3. `publish --dry-run` validates both architectures without uploading.
Select an explicit version with `sdk use <version> --yes --json`; upgrade only when requested.
Installation, project upgrades, device execution, and store publishing are separate actions; `--yes` does not authorize all of them.

## Output

Finite commands write one JSON object to stdout. Progress, diagnostics, and logs go to stderr; do not merge them.
Inspect `ok`, `code`, `result`, `error`, and `warnings`. Exit codes: 0 success, 1 execution failure,
2 invalid arguments, 3 missing input, 4 environment or compatibility failure.

`--offline` prevents network checks; missing cache does not mean the SDK is current.
Use `run --no-follow --json` for machine-readable startup results. Follow logs separately.
