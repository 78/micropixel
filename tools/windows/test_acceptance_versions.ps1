# Explicit test-only A/B migration exercise. Does not publish or access devices.
[CmdletBinding()]
param(
    [string]$Launcher = "$env:LOCALAPPDATA\MicroPixel\bin\micropixel.exe",
    [Parameter(Mandatory=$true)][string]$Directory,
    [switch]$OfflineFixtures
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$Directory = [IO.Path]::GetFullPath($Directory)
if (Test-Path -LiteralPath $Directory) { throw 'Use a new empty test directory; existing projects are never overwritten' }
New-Item -ItemType Directory -Path $Directory | Out-Null
function Invoke-MicroPixel([string[]]$Command) {
    $raw = & $Launcher @Command --json
    $code = $LASTEXITCODE
    $result = $raw | ConvertFrom-Json
    if ($code -ne 0 -or -not $result.ok) { throw "Test command failed ($code): $($Command -join ' '); $($result.code)" }
    return $result
}
$a = Join-Path $Directory '中文 游戏 A'
$other = Join-Path $Directory '保持 A'
Invoke-MicroPixel @('setup', '--version', '9000.0.1', '--yes') | Out-Null
Invoke-MicroPixel @('init', $a, '--app-id', 'local.acceptance-a', '--title', 'Acceptance A') | Out-Null
Invoke-MicroPixel @('init', $other, '--app-id', 'local.acceptance-other', '--title', 'Acceptance Other') | Out-Null
$otherLock = Get-FileHash -LiteralPath (Join-Path $other 'micropixel.lock.json')
$appBefore = Get-FileHash -LiteralPath (Join-Path $a 'app.json')
$lockBefore = Get-FileHash -LiteralPath (Join-Path $a 'micropixel.lock.json')
$status = Invoke-MicroPixel @('sdk', 'status', '--project', $a, '--check')
if (-not $OfflineFixtures -and $status.result.candidate_version -ne '9000.0.2') { throw 'Test channel did not recommend fixture B' }
Invoke-MicroPixel @('package', $a, '--aot-target', 'riscv32-ilp32f') | Out-Null
if ((Get-FileHash -LiteralPath (Join-Path $a 'micropixel.lock.json')).Hash -ne $lockBefore.Hash) { throw 'Package silently changed the project lock' }
if ($OfflineFixtures) {
    Invoke-MicroPixel @('sdk', 'use', '9000.0.2', '--project', $a, '--yes') | Out-Null
} else {
    Invoke-MicroPixel @('sdk', 'upgrade', '--project', $a, '--yes') | Out-Null
}
foreach ($target in @('riscv32-ilp32f', 'xtensa')) {
    Invoke-MicroPixel @('package', $a, '--aot-target', $target) | Out-Null
}
Invoke-MicroPixel @('sdk', 'use', '9000.0.1', '--project', $a, '--yes', '--offline') | Out-Null
Invoke-MicroPixel @('build', $a, '--aot-target', 'riscv32-ilp32f', '--offline') | Out-Null
if ((Get-FileHash -LiteralPath (Join-Path $a 'app.json')).Hash -ne $appBefore.Hash) { throw 'SDK switching modified app.json' }
if ((Get-FileHash -LiteralPath (Join-Path $other 'micropixel.lock.json')).Hash -ne $otherLock.Hash) { throw 'SDK switching changed another project' }
$before = Invoke-MicroPixel @('manager-version')
$update = Invoke-MicroPixel @('update', '--yes')
$after = Invoke-MicroPixel @('manager-version')
if (-not $update.result.updated -or $before.result.build_id -eq $after.result.build_id -or $after.result.build_id -ne $update.result.candidate_build_id) { throw 'Manager version-directory switch was not exercised' }
Write-Host 'Manager update and bootstrap switch passed.'
Write-Host 'A/B update notice, explicit upgrade, dual-target rebuild, offline rollback and project isolation passed.'
