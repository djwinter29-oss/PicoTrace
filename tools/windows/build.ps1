param(
    [string]$Board = "pico",
    [string]$FirmwareBuildDir = ""
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "common.ps1")

$repoRoot = Get-RepoRoot

Push-Location $repoRoot
try {
    Initialize-BuildEnvironment

    if (-not $FirmwareBuildDir) {
        $FirmwareBuildDir = Get-DefaultFirmwareBuildDir -Board $Board
    }

    Invoke-NativeCommand "Firmware configure" { cmake -S firmware -B $FirmwareBuildDir -DPICO_BOARD=$Board }
    Invoke-NativeCommand "Firmware build" { cmake --build $FirmwareBuildDir }
}
finally {
    Pop-Location
}