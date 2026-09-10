# Inventory redistributable files from the hosted Visual Studio installation.
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$redist = Join-Path $vs 'VC\Redist\MSVC'
$versions = @(Get-ChildItem -LiteralPath $redist -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name })
$directory = Join-Path $versions[-1].FullName 'x64\Microsoft.VC143.CRT'
$files = @{}
Get-ChildItem -LiteralPath $directory -Filter '*.dll' | ForEach-Object {
    $files[$_.Name] = @{ sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); version = $_.VersionInfo.FileVersion; size_bytes = $_.Length }
}
@{ schema_version = 1; version = $versions[-1].Name; files = $files } | ConvertTo-Json -Depth 5
