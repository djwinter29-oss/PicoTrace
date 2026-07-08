param(
    [string]$FirmwareBuildDir = "build/firmware",
    [string]$TestBuildDir = "build/tests",
    [switch]$SkipFirmwareBuild
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "common.ps1")

$repoRoot = Get-RepoRoot

Push-Location $repoRoot
try {
    Initialize-BuildEnvironment

    if (-not $SkipFirmwareBuild) {
        if (-not $env:PICO_SDK_PATH) {
            throw "PICO_SDK_PATH is not set. Export it in your shell or set it in tools/windows/.env.ps1, or pass -SkipFirmwareBuild to run only the host-side tests."
        }

        Invoke-NativeCommand "Firmware configure" { cmake -S firmware -B $FirmwareBuildDir }
        Invoke-NativeCommand "Firmware build" { cmake --build $FirmwareBuildDir --config Debug }
    }

    Invoke-NativeCommand "Host test configure" { cmake -S firmware/tests -B $TestBuildDir }
    Invoke-NativeCommand "Host test build" { cmake --build $TestBuildDir --config Debug }

    Invoke-NativeCommand "Host test run" { ctest --test-dir $TestBuildDir -C Debug --output-on-failure }
}
finally {
    Pop-Location
}