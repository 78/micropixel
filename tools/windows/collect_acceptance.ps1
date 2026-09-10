# SPDX-License-Identifier: Apache-2.0
# Read-only environment collection. Never installs, upgrades, flashes or uploads.
[CmdletBinding()]
param(
    [string]$MicroPixel = "$env:LOCALAPPDATA\MicroPixel\bin\micropixel.exe",
    [string]$Output = "$env:TEMP\micropixel-acceptance.json"
)
$ErrorActionPreference = 'Stop'
$report = [ordered]@{
    schema_version = 1
    collected_at = (Get-Date).ToUniversalTime().ToString('o')
    os = [ordered]@{
        version = [Environment]::OSVersion.Version.ToString()
        is_64_bit = [Environment]::Is64BitOperatingSystem
        powershell = $PSVersionTable.PSVersion.ToString()
    }
    launcher_found = (Test-Path -LiteralPath $MicroPixel -PathType Leaf)
    checks = @()
    manual_results = @()
}
# Keep only reviewed fields. Raw stdout/stderr, paths and device data are never saved.
function Read-Check([string]$Name, [string[]]$Arguments) {
    $status = [ordered]@{ name = $Name; exit_code = $null; json_valid = $false }
    try {
        $raw = & $MicroPixel @Arguments 2>$null
        $status.exit_code = $LASTEXITCODE
        $value = ($raw -join "`n") | ConvertFrom-Json -ErrorAction Stop
        $status.json_valid = $true
        if ($null -ne $value.schema_version) { $status.schema_version = $value.schema_version }
        if ($null -ne $value.ok) { $status.ok = [bool]$value.ok }
        if ($null -ne $value.result.ready) { $status.ready = [bool]$value.result.ready }
        foreach ($key in @('sdk_version', 'toolchain_id', 'manager_version', 'manager_build_id', 'python_version', 'pyserial_version', 'wasi_sdk_version')) {
            $entry = $value.result.$key
            # Version/build identifiers only; never persist arbitrary diagnostic strings.
            if ($entry -is [string] -and $entry -match '^[a-zA-Z0-9._+-]{1,96}$') { $status[$key] = $entry }
        }
        $status.tools = @{}
        foreach ($tool in @('clang', 'riscv', 'xtensa')) {
            $entry = $value.result.tools.$tool
            if ($entry -is [string] -and $entry -match '\b[0-9]+\.[0-9]+(?:\.[0-9]+){0,2}\b') { $status.tools[$tool] = $Matches[0] }
        }
    } catch {
        $status.error = 'check_failed_or_invalid_json'
    }
    return $status
}
if ($report.launcher_found) {
    $report.checks += Read-Check 'doctor' @('doctor', '--offline', '--json')
    $report.checks += Read-Check 'sdk_status' @('sdk', 'status', '--offline', '--json')
}
1..19 | ForEach-Object {
    $report.manual_results += [ordered]@{ id = ('W{0:D2}' -f $_); status = 'not_run'; notes = '' }
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Output -Encoding UTF8
Write-Host "Local acceptance report written. No logs or device identifiers were collected or uploaded."
if (-not $report.launcher_found) { exit 4 }
if (@($report.checks | Where-Object { $_.exit_code -ne 0 -or -not $_.json_valid }).Count) { exit 4 }
