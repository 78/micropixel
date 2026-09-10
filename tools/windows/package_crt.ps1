# Copy only hash-pinned Microsoft redistributable files; never use System32 DLLs.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference = 'Stop'
$pin = Get-Content -Raw (Join-Path $PSScriptRoot 'msvc-crt-sources.json') | ConvertFrom-Json
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$directory = Join-Path $vs "VC\Redist\MSVC\$($pin.version)\x64\Microsoft.VC143.CRT"
New-Item -ItemType Directory -Force -Path $Output | Out-Null
foreach ($file in $pin.files.PSObject.Properties) {
    $source = Join-Path $directory $file.Name
    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.Value.sha256) {
        throw "Pinned Microsoft runtime changed: $($file.Name); review the source lock before rebuilding"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $Output $file.Name)
}
@"
Microsoft Visual C++ Runtime 2015-2022
Copyright Microsoft Corporation. All rights reserved.
App-local deployment from the Visual Studio 2022 redistributable directory.
License terms: $($pin.license)
Distributable files list: $($pin.redistribution)
MicroPixel's Apache-2.0 license does not apply to these Microsoft components.
"@ | Set-Content -LiteralPath (Join-Path $Output 'LICENSE-MICROSOFT-CRT.txt') -Encoding UTF8
