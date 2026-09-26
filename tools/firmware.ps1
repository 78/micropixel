#!/usr/bin/env pwsh
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
    Windows entry point for the MicroPixel Host firmware workflow.

.DESCRIPTION
    Builds, flashes, and monitors Host firmware with a native Windows ESP-IDF
    installation. This is the Windows counterpart of tools/p4.sh, tools/s3.sh,
    and tools/s31.sh, and it mirrors their behaviour:

      * loads the repository-root .env without overriding the caller's variables,
      * activates the ESP-IDF selected by the ESP-IDF Installation Manager (EIM),
      * writes the generated Remote Control / LVGL sdkconfig defaults for the board,
      * delegates build, flash, monitor, and fullclean to tools/firmware.py.

    Guest application images (build-release) are not produced here, because
    build-release is a POSIX shell entry point that builds the Host, the release
    Apps, the app_store image, and the combined image in one pass. The same image
    can be built on Windows from the official toolchain; see
    docs/development/flashing.zh-CN.md section 10.4.

    Unlike the POSIX wrappers this script does not rewrite an existing generated
    sdkconfig in place. When the Remote Control configuration changes it warns
    instead, and fullclean-host re-applies the new values.

.PARAMETER Command
    build-host      Compile the selected board's Host.
    build-null      Compile the hardware-independent Null gate; never flash it.
    flash-host      Flash the already-built Host; app_store is not touched.
    monitor         Follow the Host console without building, flashing, or erasing.
    fullclean-host  Delete the Host build cache.
    port            Print the serial port resolved for the board.
    list            List every firmware profile.
    build-release   Not available on Windows; prints the WSL instructions.

.PARAMETER Board
    p4 (default), box3, szpi, cores3, or s31.

.PARAMETER Port
    Serial port such as COM7. Probed when omitted.

.PARAMETER Baud
    Override the profile's flash baud rate.

.PARAMETER Reset
    Reset the application when monitor starts.

.PARAMETER IdfPath
    Use this ESP-IDF tree instead of the EIM selection, skipping the 6.1 check.

.PARAMETER IdfActivationScript
    Dot-source this activation script instead of the EIM selection.

.EXAMPLE
    pwsh tools/firmware.ps1 build-host

.EXAMPLE
    pwsh tools/firmware.ps1 flash-host -Board p4 -Port COM7

.EXAMPLE
    pwsh tools/firmware.ps1 monitor -Board box3 -Port COM7 -Reset
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet(
        'build-host',
        'build-null',
        'flash-host',
        'monitor',
        'fullclean-host',
        'port',
        'list',
        'build-release'
    )]
    [string] $Command,

    [Parameter(Position = 1)]
    [ValidateSet('p4', 'box3', 'szpi', 'cores3', 's31')]
    [string] $Board = 'p4',

    [Parameter(Position = 2)]
    [string] $Port,

    [string] $Baud,

    [switch] $Reset,

    [string] $IdfPath,

    [string] $IdfActivationScript
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$profilesPath = Join-Path $PSScriptRoot 'firmware_profiles.json'
$firmwareScript = Join-Path $PSScriptRoot 'firmware.py'

# Board name -> product profile, plus the compile-gate profile build-null uses.
# tools/p4.sh and tools/s31.sh apply the generated Remote Control defaults to
# their Null gate while tools/s3.sh does not; mirror that per board.
$boardProfiles = @{
    'p4'     = @{ Product = 'metalio-claw4';  Null = 'p4-null';  NullUsesEnvDefaults = $true }
    's31'    = @{ Product = 'esp-mosaico';    Null = 's31-null'; NullUsesEnvDefaults = $true }
    'box3'   = @{ Product = 'esp-box-3';      Null = 's3-null';  NullUsesEnvDefaults = $false }
    'szpi'   = @{ Product = 'szpi-esp32s3';   Null = 's3-null';  NullUsesEnvDefaults = $false }
    'cores3' = @{ Product = 'm5stack-cores3'; Null = 's3-null';  NullUsesEnvDefaults = $false }
}

function Write-Step {
    param([Parameter(Mandatory = $true)][string] $Message)
    Write-Host "==> $Message"
}

function Fail {
    param([Parameter(Mandatory = $true)][string] $Message)
    [Console]::Error.WriteLine("firmware.ps1: $Message")
    exit 2
}

function Resolve-RepoPath {
    param([Parameter(Mandatory = $true)][string] $Path)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Get-FirmwareProfiles {
    $json = [System.IO.File]::ReadAllText($profilesPath, [System.Text.Encoding]::UTF8)
    return $json | ConvertFrom-Json
}

function Get-ProfileField {
    param(
        [Parameter(Mandatory = $true)] $Profiles,
        [Parameter(Mandatory = $true)][string] $Name,
        [Parameter(Mandatory = $true)][string] $Field
    )
    $profile = $Profiles.PSObject.Properties[$Name]
    if (-not $profile) {
        Fail "unknown firmware profile '$Name'; check $profilesPath"
    }
    $property = $profile.Value.PSObject.Properties[$Field]
    if (-not $property) {
        return $null
    }
    return $property.Value
}

function Get-SdkconfigValue {
    param(
        [Parameter(Mandatory = $true)][string] $Path,
        [Parameter(Mandatory = $true)][string] $Key
    )
    if (-not (Test-Path -LiteralPath $Path)) {
        return $null
    }
    # Last definition wins, matching the board wrappers that scan several files.
    $value = $null
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        if ($line.StartsWith("$Key=", [System.StringComparison]::Ordinal)) {
            $value = $line.Substring($Key.Length + 1).Trim()
        }
    }
    return $value
}

function Import-RepositoryEnv {
    $envPath = Join-Path $repoRoot '.env'
    if (-not (Test-Path -LiteralPath $envPath)) {
        return
    }
    Write-Step "Loading environment defaults from $envPath"
    foreach ($line in [System.IO.File]::ReadAllLines($envPath)) {
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
        # Explicit environment variables take precedence over the .env file.
        if (-not [string]::IsNullOrEmpty([System.Environment]::GetEnvironmentVariable($key))) {
            continue
        }
        [System.Environment]::SetEnvironmentVariable($key, $value)
    }
}

function Find-EimActivationScript {
    $candidates = @()
    if ($env:MICROPIXEL_IDF_TOOLS_PATH) {
        $candidates += $env:MICROPIXEL_IDF_TOOLS_PATH
    }
    if ($env:IDF_TOOLS_PATH) {
        $candidates += $env:IDF_TOOLS_PATH
    }
    # The ESP-IDF Installation Manager default on Windows.
    $candidates += 'C:\Espressif\tools'
    $candidates += (Join-Path $env:USERPROFILE '.espressif')

    foreach ($directory in $candidates) {
        $manifestPath = Join-Path $directory 'eim_idf.json'
        if (-not (Test-Path -LiteralPath $manifestPath)) {
            continue
        }
        $manifest = [System.IO.File]::ReadAllText(
            $manifestPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
        $installed = @($manifest.idfInstalled)
        if ($installed.Count -eq 0) {
            continue
        }
        $selectedId = $null
        $selectedProperty = $manifest.PSObject.Properties['idfSelectedId']
        if ($selectedProperty) {
            $selectedId = $selectedProperty.Value
        }
        $selected = $installed | Where-Object { $_.id -eq $selectedId } | Select-Object -First 1
        if (-not $selected) {
            $selected = $installed | Select-Object -Last 1
        }
        $script = $selected.activationScript
        if ($script -and (Test-Path -LiteralPath $script)) {
            Write-Step "Selected ESP-IDF $($selected.name) from $manifestPath"
            return $script
        }
    }
    return $null
}

function Assert-EspIdfVersion {
    param([Parameter(Mandatory = $true)][bool] $AllowOverride)

    $header = Join-Path $env:IDF_PATH 'components/esp_common/include/esp_idf_version.h'
    if (-not (Test-Path -LiteralPath $header)) {
        Write-Warning "cannot read the ESP-IDF version from $header"
        return
    }
    $text = [System.IO.File]::ReadAllText($header)
    $major = [regex]::Match($text, 'ESP_IDF_VERSION_MAJOR\s+(\d+)').Groups[1].Value
    $minor = [regex]::Match($text, 'ESP_IDF_VERSION_MINOR\s+(\d+)').Groups[1].Value
    $version = "$major.$minor"
    Write-Step "ESP-IDF v$version at $env:IDF_PATH"
    if ($version -eq '6.1') {
        return
    }
    if ($AllowOverride) {
        Write-Warning "ESP-IDF v$version is not the pinned 6.1 line; continuing because you selected it explicitly."
        return
    }
    Fail ("ESP-IDF v$version is unsupported; this repository requires ESP-IDF 6.1. " +
        'Select v6.1 with the ESP-IDF Installation Manager, or pass -IdfPath / ' +
        '-IdfActivationScript to override deliberately.')
}

function Resolve-EspIdf {
    if ($IdfActivationScript) {
        if (-not (Test-Path -LiteralPath $IdfActivationScript)) {
            Fail "-IdfActivationScript not found: $IdfActivationScript"
        }
        Write-Step "Activating ESP-IDF from $IdfActivationScript"
        . $IdfActivationScript
        Assert-EspIdfVersion -AllowOverride:$true
        return
    }
    if ($IdfPath) {
        if (-not (Test-Path -LiteralPath (Join-Path $IdfPath 'tools/idf.py'))) {
            Fail "-IdfPath is not an ESP-IDF tree: $IdfPath"
        }
        $env:IDF_PATH = (Resolve-Path -LiteralPath $IdfPath).Path
        Assert-EspIdfVersion -AllowOverride:$true
        return
    }
    if ($env:IDF_PATH -and (Test-Path -LiteralPath (Join-Path $env:IDF_PATH 'tools/idf.py'))) {
        Write-Step "Reusing the active ESP-IDF at $env:IDF_PATH"
        Assert-EspIdfVersion -AllowOverride:$false
        return
    }

    $script = Find-EimActivationScript
    if (-not $script) {
        Fail ('no active ESP-IDF and no ESP-IDF Installation Manager manifest was found; ' +
            'install ESP-IDF 6.1 with the EIM, or set IDF_PATH, or pass -IdfPath.')
    }
    . $script
    if (-not $env:IDF_PATH -or -not (Test-Path -LiteralPath (Join-Path $env:IDF_PATH 'tools/idf.py'))) {
        Fail "activation did not produce a usable IDF_PATH (got '$env:IDF_PATH')"
    }
    Assert-EspIdfVersion -AllowOverride:$false
}

function Resolve-Python {
    if ($env:IDF_PYTHON_ENV_PATH) {
        $candidate = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts/python.exe'
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    $command = Get-Command python -ErrorAction SilentlyContinue
    if (-not $command) {
        Fail 'no Python interpreter found; activate ESP-IDF first, or set IDF_PYTHON_ENV_PATH'
    }
    return $command.Source
}

function Write-SdkconfigEnvDefaults {
    param(
        [Parameter(Mandatory = $true)][string] $BuildDirectory,
        [Parameter(Mandatory = $true)][string[]] $DefaultFiles,
        [Parameter(Mandatory = $true)][string] $Target
    )

    $remoteHost = [string] $env:MICROPIXEL_REMOTE_CONTROL_HOST
    $remotePort = '8443'
    if ($env:MICROPIXEL_REMOTE_CONTROL_PORT) {
        $remotePort = [string] $env:MICROPIXEL_REMOTE_CONTROL_PORT
    }
    $allowUnverified = 'y'
    if ($env:MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS) {
        $allowUnverified = [string] $env:MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS
    }
    $trustedCa = [string] $env:MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64

    if ($remoteHost -and $remoteHost -notmatch '^[A-Za-z0-9._:-]+$') {
        Fail "MICROPIXEL_REMOTE_CONTROL_HOST contains unsupported characters: $remoteHost"
    }
    if ($remotePort -notmatch '^[0-9]+$' -or [int] $remotePort -lt 1 -or [int] $remotePort -gt 65535) {
        Fail "MICROPIXEL_REMOTE_CONTROL_PORT must be between 1 and 65535: $remotePort"
    }
    switch -Regex ($allowUnverified) {
        '^(?i)(y|yes|true|1)$' { $allowUnverified = 'y' }
        '^(?i)(n|no|false|0)$' { $allowUnverified = 'n' }
        default { Fail "MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS must be y or n" }
    }
    if ($trustedCa -and $trustedCa -notmatch '^[A-Za-z0-9+/=]+$') {
        Fail 'MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64 is not valid base64 text'
    }
    if ($allowUnverified -eq 'n' -and -not $trustedCa) {
        Fail 'strict Remote Control TLS requires MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64'
    }

    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add('CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST="' + $remoteHost + '"')
    $lines.Add("CONFIG_MICROPIXEL_REMOTE_CONTROL_PORT=$remotePort")
    if ($allowUnverified -eq 'y') {
        $lines.Add('CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS=y')
    } else {
        $lines.Add('# CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS is not set')
    }
    $lines.Add('CONFIG_MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64="' + $trustedCa + '"')

    $fileName = 'sdkconfig.env.defaults'
    if ($Target -eq 'esp32s3') {
        # tools/s3.sh keeps its generated defaults to the Remote Control keys.
        $fileName = 'sdkconfig.remote.defaults'
    } else {
        $lvMemSize = $null
        foreach ($file in $DefaultFiles) {
            $candidate = Get-SdkconfigValue -Path $file -Key 'CONFIG_LV_MEM_SIZE'
            if ($candidate) {
                $lvMemSize = $candidate
            }
        }
        if (-not $lvMemSize -or $lvMemSize -notmatch '^[1-9][0-9]*$') {
            Fail 'the target sdkconfig defaults must define a positive CONFIG_LV_MEM_SIZE in bytes'
        }
        $lines.Add('# CONFIG_MBEDTLS_HAVE_TIME_DATE is not set')
        $lines.Add("CONFIG_LV_MEM_SIZE=$lvMemSize")
        $lines.Add('# CONFIG_LV_BUILD_EXAMPLES is not set')
        $lines.Add('# CONFIG_LV_BUILD_DEMOS is not set')
    }

    $path = Join-Path $BuildDirectory $fileName
    # Kconfig reads these files literally, so keep LF endings and no BOM.
    $content = ($lines -join "`n") + "`n"
    $previous = $null
    if (Test-Path -LiteralPath $path) {
        $previous = [System.IO.File]::ReadAllText($path)
    }
    if ($previous -ne $content) {
        New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null
        [System.IO.File]::WriteAllText($path, $content, [System.Text.UTF8Encoding]::new($false))
        if ($previous -and (Test-Path -LiteralPath (Join-Path $BuildDirectory 'sdkconfig.release'))) {
            Write-Warning ('the Remote Control configuration changed; run fullclean-host ' +
                '(or delete the build directory) so the generated sdkconfig picks it up')
        }
    }
    return $path
}

if (-not (Test-Path -LiteralPath $profilesPath)) {
    Fail "firmware profile file is missing: $profilesPath"
}

if ($Reset -and $Command -ne 'monitor') {
    Fail '-Reset is only valid with the monitor command'
}

Import-RepositoryEnv
$profiles = Get-FirmwareProfiles

# list needs neither ESP-IDF nor a board.
if ($Command -eq 'list') {
    $python = Resolve-Python
    & $python $firmwareScript 'list'
    exit $LASTEXITCODE
}

if ($Command -eq 'build-release') {
    [Console]::Error.WriteLine(@'
firmware.ps1: build-release is not available on Windows.

build-release is a POSIX shell entry point (tools/p4.sh, tools/s31.sh, tools/s3.sh)
that builds the Host, seven release Apps, the app_store image, and the combined
browser image in one pass. Windows has no equivalent wrapper, but the same image can
be produced natively from the official Windows toolchain:

    . .\tools\guest-toolchain.ps1
    python tools/micropixel package guest/apps/<app> --profile release --aot-target xtensa
    python tools/build_app_store_image.py --app-store-size 0x0800000 --output app-store.bin <bundles...>
    python tools/build_full_firmware_image.py --build-dir <host build dir> ^
        --app-store-image app-store.bin --output micropixel-full.bin

See docs/development/flashing.zh-CN.md section 10.4. To use the POSIX entry point
instead, run it from WSL:

    bash tools/p4.sh   build-release
    bash tools/s31.sh  build-release
    bash tools/s3.sh   build-release box3
'@)
    exit 2
}

$mapping = $boardProfiles[$Board]
$profileName = $mapping.Product
$action = 'build'
$useEnvDefaults = $true

switch ($Command) {
    'build-host' { $profileName = $mapping.Product }
    'build-null' {
        $profileName = $mapping.Null
        $useEnvDefaults = $mapping.NullUsesEnvDefaults
    }
    'flash-host' { $action = 'flash-built' }
    'monitor' { $action = 'monitor' }
    'fullclean-host' {
        $action = 'fullclean'
        $useEnvDefaults = $false
    }
    'port' {
        $action = 'port'
        $useEnvDefaults = $false
    }
}

if ($env:CONDA_PREFIX) {
    Write-Warning "an active Conda environment may shadow the ESP-IDF tools: $env:CONDA_PREFIX"
}

Resolve-EspIdf
$python = Resolve-Python
# The workspace, the user profile, or a device identifier can contain non-ASCII
# characters, which breaks ESP-IDF's default console encoding.
$env:PYTHONUTF8 = '1'

$target = Get-ProfileField -Profiles $profiles -Name $profileName -Field 'target'
$sdkconfigName = Get-ProfileField -Profiles $profiles -Name $profileName -Field 'sdkconfig_name'

$buildDirEnv = Get-ProfileField -Profiles $profiles -Name $profileName -Field 'build_dir_env'
$buildDirValue = $null
if ($buildDirEnv) {
    $buildDirValue = [System.Environment]::GetEnvironmentVariable($buildDirEnv)
}
if (-not $buildDirValue) {
    $buildDirValue = Get-ProfileField -Profiles $profiles -Name $profileName -Field 'build_dir'
}
$buildDir = Resolve-RepoPath -Path $buildDirValue

$sdkconfigEnv = Get-ProfileField -Profiles $profiles -Name $profileName -Field 'sdkconfig_env'
$sdkconfigValue = $null
if ($sdkconfigEnv) {
    $sdkconfigValue = [System.Environment]::GetEnvironmentVariable($sdkconfigEnv)
}
if ($sdkconfigValue) {
    $sdkconfigPath = Resolve-RepoPath -Path $sdkconfigValue
} else {
    $sdkconfigPath = Join-Path $buildDir $sdkconfigName
}

$defaultsEnv = Get-ProfileField -Profiles $profiles -Name $profileName -Field 'sdkconfig_defaults_env'
$defaultsOverride = $null
if ($defaultsEnv) {
    $defaultsOverride = [System.Environment]::GetEnvironmentVariable($defaultsEnv)
}
if ($defaultsOverride) {
    $defaultFiles = @($defaultsOverride.Split(';') | Where-Object { $_ })
} else {
    $defaultFiles = @(
        Get-ProfileField -Profiles $profiles -Name $profileName -Field 'sdkconfig_defaults'
    )
}
$defaultFiles = @($defaultFiles | ForEach-Object { Resolve-RepoPath -Path $_ })

$arguments = @($firmwareScript, $profileName, $action)
if ($useEnvDefaults) {
    $envDefaultsPath = Write-SdkconfigEnvDefaults -BuildDirectory $buildDir `
        -DefaultFiles $defaultFiles -Target $target
    $arguments += @('--sdkconfig-defaults', ((@($defaultFiles) + $envDefaultsPath) -join ';'))
}
$arguments += @('--build-dir', $buildDir, '--sdkconfig', $sdkconfigPath)
if ($Port) {
    $arguments += @('--port', $Port)
}
if ($Baud) {
    $arguments += @('--baud', $Baud)
}
if ($Reset) {
    $arguments += '--reset'
}

Write-Step "$Command ($profileName) using $python"
Write-Step "build directory: $buildDir"
if ($action -eq 'build' -and -not (Test-Path -LiteralPath (Join-Path $buildDir 'CMakeCache.txt'))) {
    Write-Step ('first build for this profile: ESP-IDF runs CMake configure and may fetch ' +
        'managed components before compiling begins, so expect several minutes of quiet ' +
        'output before the first compile line')
}
& $python @arguments
exit $LASTEXITCODE
