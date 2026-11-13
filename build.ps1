<#
.SYNOPSIS
    Builds the wowee project (Windows equivalent of build.sh).

.DESCRIPTION
    Creates a build directory, runs CMake configure + build, and creates a
    directory junction for the Data folder so the binary can find assets.
#>

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

# Commit the superproject records for a submodule path; empty outside a git checkout.
function Get-RecordedCommit([string]$Path) {
    try {
        $sha = & git -C $ScriptDir rev-parse -q --verify "HEAD:$Path" 2>$null
        if ($LASTEXITCODE -eq 0) { return "$sha".Trim() }
    } catch {}
    return ""
}

# Shallow-fetch one ref (branch, tag or commit SHA) of a repository into a directory.
function Invoke-FetchAtRef([string]$Repo, [string]$Ref, [string]$Dir) {
    & git init -q $Dir | Out-Null
    if ($LASTEXITCODE -ne 0) { return $false }
    & git -C $Dir fetch -q --depth 1 $Repo $Ref | Out-Null
    if ($LASTEXITCODE -ne 0) { return $false }
    & git -C $Dir checkout -q --detach FETCH_HEAD | Out-Null
    return ($LASTEXITCODE -eq 0)
}

function Ensure-Fsr2Sdk {
    $sdkDir = Join-Path $ScriptDir "extern\FidelityFX-FSR2"
    $sdkHeader = Join-Path $sdkDir "src\ffx-fsr2-api\ffx_fsr2.h"
    if (Test-Path $sdkHeader) { return }

    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Write-Warning "git not found; cannot auto-fetch AMD FSR2 SDK."
        return
    }

    $sdkRef = Get-RecordedCommit "extern/FidelityFX-FSR2"
    if (-not $sdkRef) { $sdkRef = "master" }
    Write-Host "Fetching AMD FidelityFX FSR2 SDK ($sdkRef) into $sdkDir ..."
    New-Item -ItemType Directory -Path (Join-Path $ScriptDir "extern") -Force | Out-Null
    if (-not (Invoke-FetchAtRef "https://github.com/GPUOpen-Effects/FidelityFX-FSR2.git" $sdkRef $sdkDir)) {
        Write-Warning "Failed to clone AMD FSR2 SDK. Build will use internal fallback path."
    }
}

function Ensure-FidelityFxSdk {
    $sdkDir = Join-Path $ScriptDir "extern\FidelityFX-SDK"
    $sdkHeader = Join-Path $sdkDir "Kits\FidelityFX\framegeneration\fsr3\include\ffx_frameinterpolation.h"
    $sdkRepo = if ($env:WOWEE_FFX_SDK_REPO) { $env:WOWEE_FFX_SDK_REPO } else { "https://github.com/Kelsidavis/FidelityFX-SDK.git" }
    if (Test-Path $sdkHeader) { return }

    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Write-Warning "git not found; cannot auto-fetch AMD FidelityFX SDK."
        return
    }
    $sdkRef = if ($env:WOWEE_FFX_SDK_REF) { $env:WOWEE_FFX_SDK_REF } else { Get-RecordedCommit "extern/FidelityFX-SDK" }
    if (-not $sdkRef) { $sdkRef = "main" }

    Write-Host "Fetching AMD FidelityFX SDK ($sdkRef from $sdkRepo) into $sdkDir ..."
    New-Item -ItemType Directory -Path (Join-Path $ScriptDir "extern") -Force | Out-Null
    if (-not (Invoke-FetchAtRef $sdkRepo $sdkRef $sdkDir)) {
        Write-Warning "Failed to clone AMD FidelityFX SDK. FSR3 framegen extern will be unavailable."
    }
}

Write-Host "Building wowee..."
Ensure-Fsr2Sdk
Ensure-FidelityFxSdk

# Create build directory if it doesn't exist
if (-not (Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" | Out-Null
}
Set-Location "build"

# Configure with CMake
Write-Host "Configuring with CMake..."
& cmake .. -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Build with all cores
$numProcs = $env:NUMBER_OF_PROCESSORS
if (-not $numProcs) { $numProcs = 4 }
Write-Host "Building with $numProcs cores..."
& cmake --build . --parallel $numProcs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Ensure Data junction exists in bin directory
$binData = Join-Path (Get-Location) "bin\Data"
if (-not (Test-Path $binData)) {
    $target = (Resolve-Path (Join-Path (Get-Location) "..\Data")).Path
    cmd /c mklink /J "$binData" "$target"
}

Write-Host ""
Write-Host "Build complete! Binary: build\bin\wowee.exe"
Write-Host "Run with: cd build\bin && .\wowee.exe"
