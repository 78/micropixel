# Exercise silent install/uninstall without a prepared SDK; no device or network actions.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Installer)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$root = Join-Path $env:LOCALAPPDATA 'MicroPixel'
$launcher = Join-Path $root 'bin\micropixel.exe'
$project = Join-Path $env:RUNNER_TEMP '中文 游戏 & project'
New-Item -ItemType Directory -Force -Path $project | Out-Null
Set-Content -LiteralPath (Join-Path $project 'keep.txt') -Value 'project must survive uninstall'
function Execute-Installer([string]$File, [string]$Extra = '') {
    # Inno uninstall can hand off to a temporary child executable. -Wait waits
    # for the whole process tree; Wait-Process only waited for the bootstrap.
    $job = Start-Job -ArgumentList $File, $Extra -ScriptBlock {
        param($Executable, $Options)
        $p = Start-Process -FilePath $Executable -ArgumentList "/VERYSILENT /SUPPRESSMSGBOXES /SP- /NORESTART $Options" -PassThru -Wait
        $p.ExitCode
    }
    try {
        if (-not (Wait-Job $job -Timeout 120)) { throw 'Installer process tree timed out' }
        $code = Receive-Job $job -ErrorAction Stop
        if ($code -ne 0) { throw "Installer/uninstaller exit: $code" }
    } finally { Remove-Job $job -Force }
}
function Assert-PathCount([int]$Expected, [string]$Stage) {
    $bin = (Join-Path $root 'bin').ToLowerInvariant()
    $paths = @(([Environment]::GetEnvironmentVariable('Path', 'User') -split ';') | Where-Object { $_.Trim().TrimEnd('\').ToLowerInvariant() -eq $bin })
    if ($paths.Count -ne $Expected) {
        $entries = @(([Environment]::GetEnvironmentVariable('Path', 'User') -split ';') | Where-Object { $_ -like '*MicroPixel*' })
        throw "User PATH registration is incorrect at $Stage; expected $Expected, got $($paths.Count); MicroPixel entries: $($entries -join ', ')"
    }
}
Execute-Installer $Installer
$probe = & $launcher manager-version --json | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or -not $probe.ok -or $probe.result.python_version -ne '3.13.12' -or $probe.result.pyserial_version -ne '3.5') { throw 'Native launcher or embedded runtime failed' }
$doctor = & $launcher doctor --offline --json | ConvertFrom-Json
if ($LASTEXITCODE -ne 4 -or $doctor.ok) { throw 'Installer incorrectly reported an unprepared SDK as ready' }
Assert-PathCount 1 'first install'
New-Item -ItemType Directory -Force -Path (Join-Path $root 'packages') | Out-Null
$marker = Join-Path $root 'packages\cache-marker.txt'
Set-Content -LiteralPath $marker -Value 'retained cache'
$uninstaller = @(Get-ChildItem -LiteralPath $root -Filter 'unins*.exe')
if ($uninstaller.Count -ne 1) { throw 'Expected one uninstaller' }
Execute-Installer $uninstaller[0].FullName
Assert-PathCount 0 'first uninstall'
if (-not (Test-Path -LiteralPath $marker) -or -not (Test-Path -LiteralPath (Join-Path $project 'keep.txt'))) { throw 'Uninstall removed retained data' }
Execute-Installer $Installer
Assert-PathCount 1 'reinstall'
$probe = & $launcher manager-version --json | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or -not $probe.ok) { throw 'Reinstall failed' }
$uninstaller = @(Get-ChildItem -LiteralPath $root -Filter 'unins*.exe')
Execute-Installer $uninstaller[0].FullName '/PURGECACHE'
if (Test-Path -LiteralPath $marker) { throw 'Explicit cache purge was ignored' }
if (-not (Test-Path -LiteralPath (Join-Path $project 'keep.txt'))) { throw 'Cache purge touched a project' }
Assert-PathCount 0 'purge uninstall'
Write-Host 'Silent install, native launcher, truthful doctor, PATH, cache retention and reinstall passed.'
