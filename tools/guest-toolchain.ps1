#!/usr/bin/env pwsh
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
    Prepare the Windows Guest (game) build toolchain in the current shell.

.DESCRIPTION
    POSIX shells activate the Guest toolchain with `export WASI_SDK_PATH=... WAMRC=...`.
    Windows has no equivalent, so this script reads the repository-root .env, validates
    the three toolchain paths, and publishes them into the current process environment
    where tools/micropixel picks them up.

    Dot-source it to keep the variables in your shell:

        . .\tools\guest-toolchain.ps1
        python tools/micropixel package guest/apps/snake --aot-target xtensa --output-dir build/package/snake

    Run it without dot-sourcing to only report status. An environment variable that is
    already set in the shell wins over the .env value, matching tools/firmware.ps1.

    This script performs no download and no build; it only validates what is installed.

.PARAMETER EnvFile
    Path to the environment file. Defaults to <repository root>/.env.
#>
[CmdletBinding()]
param(
    [string] $EnvFile
)

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$envPath = if ($EnvFile) { $EnvFile } else { Join-Path $repoRoot '.env' }

function Read-EnvironmentFile {
    param([string] $Path)

    $values = [ordered]@{}
    if (-not (Test-Path -LiteralPath $Path)) {
        return $values
    }
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith('#')) {
            continue
        }
        $separator = $trimmed.IndexOf('=')
        if ($separator -lt 1) {
            continue
        }
        $key = $trimmed.Substring(0, $separator).Trim()
        $value = $trimmed.Substring($separator + 1).Trim()
        if ($value.Length -ge 2 -and (
                ($value.StartsWith('"') -and $value.EndsWith('"')) -or
                ($value.StartsWith("'") -and $value.EndsWith("'")))) {
            $value = $value.Substring(1, $value.Length - 2)
        }
        $values[$key] = $value
    }
    return $values
}

function Resolve-ToolchainVariable {
    param([string] $Key, [string] $Description, [System.Collections.IDictionary] $File)

    $current = [System.Environment]::GetEnvironmentVariable($Key)
    if (-not [string]::IsNullOrEmpty($current)) {
        return @{ Key = $Key; Value = $current; Origin = 'environment' }
    }
    if ($File.Contains($Key)) {
        return @{ Key = $Key; Value = [string] $File[$Key]; Origin = $envPath }
    }
    throw "$Key is not set. Add it to $envPath or export it before building $Description."
}

$fileValues = Read-EnvironmentFile -Path $envPath

$required = @(
    @{ Key = 'WASI_SDK_PATH'; Kind = 'directory'; Probe = 'bin\clang++.exe'; Description = 'Guest WebAssembly' }
    @{ Key = 'WAMRC'; Kind = 'file'; Probe = $null; Description = 'RISC-V AOT (P4, S31)' }
    @{ Key = 'XTENSA_WAMRC'; Kind = 'file'; Probe = $null; Description = 'Xtensa AOT (box3, szpi, cores3)' }
)

$resolved = @{}
foreach ($entry in $required) {
    $result = Resolve-ToolchainVariable -Key $entry.Key -Description $entry.Description -File $fileValues
    $path = $result.Value

    if ($entry.Kind -eq 'directory') {
        if (-not (Test-Path -LiteralPath $path -PathType Container)) {
            throw "$($entry.Key) points at a missing directory: $path"
        }
        $probe = Join-Path $path $entry.Probe
        if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) {
            throw "$($entry.Key) does not contain $($entry.Probe): $path"
        }
    }
    else {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "$($entry.Key) points at a missing file: $path"
        }
    }

    $resolved[$entry.Key] = @{ Path = $path; Origin = $result.Origin; Description = $entry.Description }
}

# UTF-8 mode keeps Python's filesystem encoding consistent when the workspace, the user
# profile or a device identifier contains non-ASCII characters.
if ([string]::IsNullOrEmpty([System.Environment]::GetEnvironmentVariable('PYTHONUTF8'))) {
    [System.Environment]::SetEnvironmentVariable('PYTHONUTF8', '1')
}

foreach ($key in $resolved.Keys) {
    [System.Environment]::SetEnvironmentVariable($key, $resolved[$key].Path)
}

Write-Host 'Guest toolchain ready:'
foreach ($key in $resolved.Keys) {
    Write-Host ("  {0,-14} {1}" -f $key, $resolved[$key].Path)
    Write-Host ("  {0,-14}   {1}  [from {2}]" -f '', $resolved[$key].Description, $resolved[$key].Origin)
}

$wasiClang = Join-Path $resolved['WASI_SDK_PATH'].Path 'bin\clang++.exe'
foreach ($probe in @($wasiClang, $resolved['WAMRC'].Path, $resolved['XTENSA_WAMRC'].Path)) {
    # Capture the whole stream rather than stopping the pipeline early: truncating a
    # native command's output leaves $LASTEXITCODE unreliable when this script is
    # dot-sourced into a pipeline such as ". .\tools\guest-toolchain.ps1 | Out-Null".
    $lines = @(& $probe --version 2>&1)
    if ($lines.Count -eq 0) {
        throw "$probe produced no output and could not be executed"
    }
    Write-Host ("  verified {0}: {1}" -f (Split-Path $probe -Leaf), ([string] $lines[0]).Trim())
}

Write-Host ''
Write-Host 'Target selection: --aot-target xtensa for ESP32-S3 boards, riscv32-ilp32f for ESP32-P4 and ESP32-S31.'
