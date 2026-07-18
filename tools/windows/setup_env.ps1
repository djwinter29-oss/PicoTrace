param(
    [string]$SdkPath = "",
    [string]$SdkRef = "",
    [switch]$UpdateSdk,
    [switch]$SkipPrereqCheck
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "common.ps1")

$repoRoot = Get-RepoRoot

if (-not $SdkPath) {
    if ($env:PICO_SDK_PATH) {
        $SdkPath = $env:PICO_SDK_PATH
    }
    else {
        $SdkPath = "C:\src\pico-sdk"
    }
}

if (-not $SkipPrereqCheck) {
    $requiredCommands = @("git", "cmake")
    foreach ($commandName in $requiredCommands) {
        if (-not (Get-Command $commandName -ErrorAction SilentlyContinue)) {
            throw "Required command '$commandName' is not available on PATH. Install prerequisites, then rerun this script."
        }
    }
}

if (-not (Test-Path $SdkPath -PathType Container)) {
    $parentDir = Split-Path -Path $SdkPath -Parent
    if ($parentDir -and -not (Test-Path $parentDir -PathType Container)) {
        New-Item -Path $parentDir -ItemType Directory | Out-Null
    }

    Invoke-NativeCommand "Clone Pico SDK" { git clone https://github.com/raspberrypi/pico-sdk.git $SdkPath }
}
elseif ($UpdateSdk) {
    Invoke-NativeCommand "Update Pico SDK refs" { git -C $SdkPath fetch --all --tags --prune }
}

if ($SdkRef) {
    Invoke-NativeCommand "Checkout Pico SDK ref" { git -C $SdkPath checkout $SdkRef }
}

Invoke-NativeCommand "Initialize Pico SDK submodules" { git -C $SdkPath submodule update --init }

$envFile = Join-Path $PSScriptRoot ".env.ps1"
if (-not (Test-Path $envFile -PathType Leaf)) {
    @(
        "# Local Windows environment overrides for PicoTrace scripts",
        "`$env:PICO_SDK_PATH = '$SdkPath'"
    ) | Set-Content -Path $envFile -Encoding UTF8

    Write-Host "Created $envFile"
}
else {
    Write-Host "Keeping existing $envFile (not overwritten)."
}

Write-Host ""
Write-Host "Setup complete."
Write-Host "PICO_SDK_PATH=$SdkPath"
Write-Host "Next: run .\tools\windows\build.ps1"
