[CmdletBinding()]
param(
    [ValidateRange(1, 500000)]
    [int]$MockCount = 120,
    [ValidateRange(5, 120)]
    [int]$TimeoutSeconds = 20
)

$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe = Join-Path $workspace 'dist\PhoenixRecover.exe'
$dist = Join-Path $workspace 'dist'
$tempRoot = Join-Path $env:TEMP ('PhoenixRecover_ui_smoke_' + [guid]::NewGuid().ToString('N'))
$report = Join-Path $tempRoot 'ui-smoke-report.txt'

if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "PhoenixRecover.exe not found: $exe"
}

New-Item -ItemType Directory -Path $tempRoot | Out-Null
try {
    $env:PHOENIX_TEST_MODE = '1'
    $env:PHOENIX_MOCK_SCAN_COUNT = [string]$MockCount
    $env:PHOENIX_UI_SMOKE = '1'
    $env:PHOENIX_UI_SMOKE_REPORT = $report

    Write-Host 'PhoenixRecover UI smoke started' -ForegroundColor Green
    Write-Host "Mock result count: $MockCount"
    Write-Host 'Flow: mock scan -> scan card -> preview results -> result summary'

    $process = Start-Process -FilePath $exe -WorkingDirectory $dist -PassThru
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        throw "UI smoke timed out after $TimeoutSeconds seconds."
    }
    if ($process.ExitCode -ne 0) {
        throw "UI smoke process exit code: $($process.ExitCode)"
    }
    if (-not (Test-Path -LiteralPath $report -PathType Leaf)) {
        throw 'UI smoke report was not generated.'
    }

    $lines = Get-Content -LiteralPath $report -Encoding utf8
    $lines | ForEach-Object { Write-Host $_ }
    if ($lines -notcontains 'UI_SMOKE_RESULT=PASS') {
        throw 'UI smoke report did not pass.'
    }
    Write-Host 'UI smoke passed.' -ForegroundColor Green
}
finally {
    Remove-Item Env:PHOENIX_TEST_MODE -ErrorAction SilentlyContinue
    Remove-Item Env:PHOENIX_MOCK_SCAN_COUNT -ErrorAction SilentlyContinue
    Remove-Item Env:PHOENIX_UI_SMOKE -ErrorAction SilentlyContinue
    Remove-Item Env:PHOENIX_UI_SMOKE_REPORT -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
