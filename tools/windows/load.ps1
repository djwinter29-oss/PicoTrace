param(
    [string]$Board = "pico",
    [string]$FirmwareBuildDir = "",
    [string]$OpenOcdExe,
    [string]$OpenOcdTarget = "",
    [int]$AdapterSpeedKhz,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "common.ps1")

$repoRoot = Get-RepoRoot

Push-Location $repoRoot
try {
    Import-RepoWindowsEnvironment

    if (-not $FirmwareBuildDir) {
        $FirmwareBuildDir = Get-DefaultFirmwareBuildDir -Board $Board
    }

    $elfPath = Join-Path $repoRoot "$FirmwareBuildDir/picotrace.elf"

    if (-not $PSBoundParameters.ContainsKey("OpenOcdExe")) {
        $OpenOcdExe = if ($env:PICO_OPENOCD_EXE) { $env:PICO_OPENOCD_EXE } else { "openocd" }
    }

    if (-not $PSBoundParameters.ContainsKey("OpenOcdTarget")) {
        if ($env:PICO_OPENOCD_TARGET) {
            $OpenOcdTarget = $env:PICO_OPENOCD_TARGET
        }
        else {
            $OpenOcdTarget = Get-DefaultOpenOcdTarget -Board $Board
        }
    }

    if (-not $PSBoundParameters.ContainsKey("AdapterSpeedKhz")) {
        $AdapterSpeedKhz = if ($env:PICO_DEBUG_PROBE_SPEED_KHZ) { [int]$env:PICO_DEBUG_PROBE_SPEED_KHZ } else { 5000 }
    }

    if (-not $SkipBuild) {
        Initialize-BuildEnvironment

        if (-not $env:PICO_SDK_PATH) {
            throw "PICO_SDK_PATH is not set. Export it in your shell or set it in tools/windows/.env.ps1, or pass -SkipBuild to program an existing ELF."
        }

        Invoke-NativeCommand "Firmware configure" { cmake -S firmware -B $FirmwareBuildDir -DPICO_BOARD=$Board }
        Invoke-NativeCommand "Firmware build" { cmake --build $FirmwareBuildDir }
    }

    if (-not (Get-Command $OpenOcdExe -ErrorAction SilentlyContinue)) {
        throw "OpenOCD executable not found: $OpenOcdExe"
    }

    if (-not (Test-Path $elfPath -PathType Leaf)) {
        throw "ELF not found at $elfPath"
    }

    Invoke-NativeCommand "Firmware program" {
        & $OpenOcdExe `
            -f interface/cmsis-dap.cfg `
            -f $OpenOcdTarget `
            -c "adapter speed $AdapterSpeedKhz" `
            -c "program $elfPath verify reset exit"
    }

    Write-Host "Programmed $elfPath over Debug Probe"
}
finally {
    Pop-Location
}