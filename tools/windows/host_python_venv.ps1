param(
    [string]$VenvDir = "host/python/.venv"
)

$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$pythonProjectDir = Join-Path $repoRoot "host\python"
$fullVenvDir = Join-Path $repoRoot $VenvDir

Push-Location $pythonProjectDir

try {
    if (-not (Test-Path $fullVenvDir)) {
        python -m venv $fullVenvDir
    }

    $python = Join-Path $fullVenvDir "Scripts\python.exe"

    & $python -m pip install --upgrade pip
    & $python -m pip install -r requirements.txt

    Write-Host "Virtual environment ready at $fullVenvDir"
    Write-Host "Activate with: $fullVenvDir\Scripts\Activate.ps1"
}
finally {
    Pop-Location
}
