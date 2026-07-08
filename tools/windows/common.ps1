$script:RepoWindowsEnvLoaded = $false

function Invoke-NativeCommand {
    param(
        [string]$Description,
        [scriptblock]$Command
    )

    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

function Get-RepoRoot {
    Resolve-Path (Join-Path $PSScriptRoot "../..")
}

function Import-RepoWindowsEnvironment {
    if ($script:RepoWindowsEnvLoaded) {
        return
    }

    $envFile = Join-Path $PSScriptRoot ".env.ps1"
    if (Test-Path $envFile -PathType Leaf) {
        . $envFile
    }

    if (-not $env:PICO_SDK_PATH) {
        $defaultPicoSdkPath = "C:\src\pico-sdk"
        if (Test-Path $defaultPicoSdkPath -PathType Container) {
            $env:PICO_SDK_PATH = $defaultPicoSdkPath
        }
    }

    $script:RepoWindowsEnvLoaded = $true
}

function Get-VsDevCmdPath {
    Import-RepoWindowsEnvironment

    if ($env:PICO_VSDEVCMD -and (Test-Path $env:PICO_VSDEVCMD -PathType Leaf)) {
        return $env:PICO_VSDEVCMD
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere -PathType Leaf)) {
        throw "The Visual Studio developer environment is required, but Visual Studio Installer metadata was not found. Set PICO_VSDEVCMD in tools/windows/.env.ps1 to override auto-detection."
    }

    $vsInstallPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsInstallPath) {
        throw "No Visual Studio installation with C++ build tools was found. Set PICO_VSDEVCMD in tools/windows/.env.ps1 to point at VsDevCmd.bat if it is installed elsewhere."
    }

    $vsDevCmd = Join-Path $vsInstallPath "Common7\Tools\VsDevCmd.bat"
    if (-not (Test-Path $vsDevCmd -PathType Leaf)) {
        throw "VsDevCmd.bat was not found under '$vsInstallPath'."
    }

    return $vsDevCmd
}

function Initialize-BuildEnvironment {
    Import-RepoWindowsEnvironment

    $needsVsDevEnvironment = (-not (Get-Command cmake -ErrorAction SilentlyContinue)) -or (-not (Get-Command cl -ErrorAction SilentlyContinue))
    if (-not $needsVsDevEnvironment) {
        return
    }

    $vsDevCmd = Get-VsDevCmdPath
    $envDump = & cmd /c "call `"$vsDevCmd`" -host_arch=x64 -arch=x64 >nul && set"
    foreach ($line in $envDump) {
        $separator = $line.IndexOf('=')
        if ($separator -lt 1) {
            continue
        }

        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        Set-Item -Path ("Env:{0}" -f $name) -Value $value
    }
}