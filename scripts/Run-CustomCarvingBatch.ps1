# Run RawScan batch validation per carving-XX.dd and merge CSV.
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$BuildDir = "build/win2019-ninja",
    [int]$From = 0,
    [int]$To = 19
)

$ErrorActionPreference = "Stop"
$exe = Join-Path $RepoRoot "$BuildDir\tests\test_custom_carving_batch.exe"
$reportDir = Join-Path $RepoRoot "docs\reports"
$merged = Join-Path $reportDir "custom_carving_results.csv"
$tmpDir = Join-Path $env:TEMP "phoenix_custom_carve"
New-Item -ItemType Directory -Force -Path $tmpDir, $reportDir | Out-Null

if (-not (Test-Path $exe)) { throw "Build first: $exe" }

$allRows = @()
for ($i = $From; $i -le $To; $i++) {
    $name = "carving-{0:d2}.dd" -f $i
    $tmpCsv = Join-Path $tmpDir "$name.csv"
    Write-Host "=== $name ==="
    $env:PHOENIX_CUSTOM_CARVE_ONLY = $name
    $env:PHOENIX_CUSTOM_CARVE_REPORT_CSV = $tmpCsv
    & $exe
    if ($LASTEXITCODE -ne 0) { Write-Warning "$name exit=$LASTEXITCODE" }
    if (Test-Path $tmpCsv) {
        $allRows += Import-Csv $tmpCsv
    }
}

if ($allRows.Count -gt 0) {
    Write-Host "Run shard rows: $($allRows.Count)"
}
$pyMerge = Join-Path $RepoRoot "scripts\Merge-CustomCarvingCsv.py"
if (Test-Path $pyMerge) {
    & python $pyMerge --repo-root $RepoRoot --out $merged
}
