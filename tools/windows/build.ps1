param(
    [string]$Board = "pico",
    [string]$FirmwareBuildDir = "",
    [string]$TestBuildDir = "build/tests"
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "common.ps1")

$repoRoot = Get-RepoRoot

Push-Location $repoRoot
try {
    Initialize-BuildEnvironment

    if (-not $FirmwareBuildDir) {
        $FirmwareBuildDir = "build/firmware-$Board"
    }

    Invoke-NativeCommand "Firmware configure" { cmake -S firmware -B $FirmwareBuildDir -DPICO_BOARD=$Board }
    Invoke-NativeCommand "Firmware build" { cmake --build $FirmwareBuildDir }

    Invoke-NativeCommand "Host test configure" { cmake -S firmware/tests -B $TestBuildDir }
    Invoke-NativeCommand "Host test build" { cmake --build $TestBuildDir }
}
finally {
    Pop-Location
}