# Digital Corpora disk-images validation (SectorReader smoke + FS metadata scan).
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$BuildDir = "build/win2019-ninja"
)

$ErrorActionPreference = "Stop"
$exe = Join-Path $RepoRoot "$BuildDir\tests\test_digital_corpora_batch.exe"
$reportDir = Join-Path $RepoRoot "docs\reports"
New-Item -ItemType Directory -Force -Path $reportDir | Out-Null

if (-not (Test-Path $exe)) {
    Write-Host "Building..."
    Push-Location (Join-Path $RepoRoot $BuildDir)
    cmd /c _build_release.bat | Out-Null
    Pop-Location
}

$env:PHOENIX_DC_REPORT_CSV = Join-Path $reportDir "digital_corpora_results.csv"
$env:PHOENIX_DC_FAIL_CSV = Join-Path $reportDir "digital_corpora_recover_failures.csv"
$stdoutLog = Join-Path $reportDir "digital_corpora_run.log"
$stderrLog = Join-Path $reportDir "digital_corpora_run_stderr.log"
Write-Host "=== Digital Corpora batch ==="
Write-Host "stdout -> $stdoutLog"
Write-Host "stderr -> $stderrLog"
& $exe 1> $stdoutLog 2> $stderrLog
$code = $LASTEXITCODE
Get-Content $stdoutLog -Tail 20
exit $code
