param(
    [string]$VenvDir = ".venv",
    [string]$Python = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "common.ps1")

$repoRoot = Get-RepoRoot
$pythonProjectDir = Join-Path $repoRoot "host\python"
$fullVenvDir = Join-Path $repoRoot $VenvDir

function Get-PythonLauncher {
    param([string]$RequestedPython)

    if ($RequestedPython) {
        return ,@($RequestedPython)
    }

    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($pythonCommand -ne $null) {
        return ,@($pythonCommand.Source)
    }

    $pyCommand = Get-Command py -ErrorAction SilentlyContinue
    if ($pyCommand -ne $null) {
        return ,@($pyCommand.Source, "-3")
    }

    throw "Python 3 launcher not found. Install Python 3 or pass -Python <path-to-python>."
}

Push-Location $pythonProjectDir

try {
    $launcher = @(Get-PythonLauncher -RequestedPython $Python)
    $venvPython = Join-Path $fullVenvDir "Scripts\python.exe"

    if (-not (Test-Path $venvPython)) {
        if ($launcher.Length -eq 1) {
            Invoke-NativeCommand "Create Python virtual environment" { & $launcher[0] -m venv $fullVenvDir }
        }
        else {
            Invoke-NativeCommand "Create Python virtual environment" { & $launcher[0] $launcher[1] -m venv $fullVenvDir }
        }
    }

    Invoke-NativeCommand "Upgrade pip" { & $venvPython -m pip install --upgrade pip }
    Invoke-NativeCommand "Install Python host requirements" { & $venvPython -m pip install -r requirements.txt }

    Write-Host "Virtual environment ready at $fullVenvDir"
    Write-Host "Activate with: $fullVenvDir\Scripts\Activate.ps1"
}
finally {
    Pop-Location
}
