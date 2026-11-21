# testdata 全量测试执行脚本
# 用法: powershell -File scripts/Run-TestdataValidation.ps1
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$BuildDir = "build/win2019-ninja",
    [string]$Config = "Release",
    [string]$ReportDir = ""
)

$ErrorActionPreference = "Stop"

if (-not $ReportDir) { $ReportDir = Join-Path $RepoRoot "docs\reports" }
New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null

$runId = "RUN-$(Get-Date -Format 'yyyy-MM-dd-HHmm')"
$csvPath = Join-Path $ReportDir "testdata_validation_$($runId -replace 'RUN-','').csv"
$logPath = Join-Path $ReportDir "testdata_validation_$($runId -replace 'RUN-','').log"

$qtBin = "C:\Qt\Qt5.14.2\5.14.2\msvc2017_64\bin"
if (Test-Path $qtBin) {
    $env:PHOENIX_QT_BIN = $qtBin
    $env:PATH = "$qtBin;$env:PATH"
}

function Write-Log($msg) {
    $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $msg"
    Add-Content -Path $logPath -Value $line
    Write-Host $msg
}

function Add-Result($dataset, $phase, $result, $notes) {
    $script:results += [PSCustomObject]@{
        run_id    = $runId
        dataset   = $dataset
        phase     = $phase
        result    = $result
        notes     = $notes
    }
}

$results = @()
$testdata = Join-Path $RepoRoot "testdata"
Write-Log "=== testdata validation $runId ==="

# --- Phase 0a: optional data prep (media, carving, EWF) ---
$prepScripts = @(
    @{ Name = "Fetch-MediaSamples"; Path = Join-Path $testdata "tools\Fetch-MediaSamples.ps1" },
    @{ Name = "Expand-FileCarvingImages"; Path = Join-Path $testdata "tools\Expand-FileCarvingImages.ps1" },
    @{ Name = "Download-CfttImages"; Path = Join-Path $testdata "tools\Download-CfttImages.ps1" },
    @{ Name = "Expand-CfttImages"; Path = Join-Path $testdata "tools\Expand-CfttImages.ps1" },
    @{ Name = "Download-SleuthkitImages"; Path = Join-Path $testdata "tools\Download-SleuthkitImages.ps1" },
    @{ Name = "Expand-EwfToRaw"; Path = Join-Path $testdata "tools\Expand-EwfToRaw.ps1" }
)
foreach ($s in $prepScripts) {
    if (Test-Path $s.Path) {
        Write-Log "Prep: $($s.Name)"
        $prepOut = & powershell -NoProfile -ExecutionPolicy Bypass -File $s.Path -RepoRoot $RepoRoot 2>&1 | Out-String
        Add-Content -Path $logPath -Value $prepOut
    }
}
$genRepair = Join-Path $RepoRoot "scripts\Generate-RepairSamples.ps1"
if ((Test-Path (Join-Path $testdata "media\xiph-poster_big.jpg")) -and (Test-Path $genRepair)) {
    Write-Log "Prep: Generate-RepairSamples"
    & powershell -NoProfile -ExecutionPolicy Bypass -File $genRepair -RepoRoot $RepoRoot 2>&1 | Out-String | ForEach-Object { Add-Content -Path $logPath -Value $_ }
}

# --- Phase 0: inventory ---
Get-ChildItem $testdata -Directory | ForEach-Object {
    $files = Get-ChildItem $_.FullName -Recurse -File -ErrorAction SilentlyContinue
    $dd = ($files | Where-Object { $_.Extension -in '.dd','.raw' }).Count
    $e01 = ($files | Where-Object { $_.Extension -eq '.E01' }).Count
    $arch = ($files | Where-Object { $_.Extension -in '.bz2','.zip','.tbz2','.dd.bz2' }).Count
    $sizeGB = [math]::Round(($files | Measure-Object Length -Sum).Sum / 1GB, 2)
    $notes = "files=$($files.Count) sizeGB=$sizeGB dd=$dd e01=$e01 archive=$arch"
    if ($dd -gt 0 -or $e01 -gt 0) {
        Add-Result $_.Name "inventory" "READY" $notes
    } elseif ($arch -gt 0) {
        Add-Result $_.Name "inventory" "NEEDS_EXTRACT" $notes
    } else {
        Add-Result $_.Name "inventory" "MATERIAL" $notes
    }
}

# --- Phase 1: CTest ---
$buildPath = Join-Path $RepoRoot $BuildDir
if (-not (Test-Path $buildPath)) { throw "Build dir missing: $buildPath" }

Write-Log "Phase 1: CTest full suite"
Push-Location $buildPath
try {
    $ctestOut = & ctest -C $Config --output-on-failure 2>&1 | Out-String
    Add-Content -Path $logPath -Value $ctestOut
    if ($ctestOut -match "100% tests passed") {
        Add-Result "ALL" "ctest" "PASS" "full suite"
    } elseif ($ctestOut -match "(\d+)% tests passed") {
        Add-Result "ALL" "ctest" "PARTIAL" ($ctestOut -split "`n" | Select-String "FAILED" | Select-Object -First 5).Line
    } else {
        Add-Result "ALL" "ctest" "FAIL" "see log"
    }
} finally { Pop-Location }

# --- Phase 2: raw image smoke (SectorReader) ---
$smokeExe = Join-Path $buildPath "tests\test_testdata_image_smoke.exe"
if (Test-Path $smokeExe) {
    Write-Log "Phase 2: Testdata_ImageSmoke (all .dd/.raw in configured roots)"
    $env:PHOENIX_TESTDATA_SMOKE_ROOTS = "dftt,cfreds,dfr-test-images,custom-carving-images,digital-corpora-disk-images,file-carving-cftt-images,sleuthkit_test_data"
    $smokeOut = & $smokeExe 2>&1 | Out-String
    Add-Content -Path $logPath -Value $smokeOut
    if ($LASTEXITCODE -eq 0) {
        if ($smokeOut -match "opened=(\d+)") { Add-Result "raw-dd-smoke" "smoke" "PASS" "opened=$($Matches[1])" }
        else { Add-Result "raw-dd-smoke" "smoke" "PASS" "completed" }
    } else {
        Add-Result "raw-dd-smoke" "smoke" "FAIL" "exit=$LASTEXITCODE"
    }
} else {
    Add-Result "raw-dd-smoke" "smoke" "BLOCKED" "exe missing; rebuild first"
}

# --- Phase 3: E01 inventory ---
$e01Files = Get-ChildItem $testdata -Recurse -Filter "*.E01" -File -ErrorAction SilentlyContinue
$e01Converted = ($e01Files | ForEach-Object {
    $dd = [IO.Path]::ChangeExtension($_.FullName, ".dd")
    if (Test-Path $dd) { $dd }
}) | Where-Object { $_ }
if ($e01Converted.Count -gt 0) {
    Add-Result "ewf-converted" "e01-format" "PASS" "converted=$($e01Converted.Count) of $($e01Files.Count) E01"
} else {
    Add-Result "digital-corpora-disk-images" "e01-format" "NEEDS_EWF_CONVERT" "count=$($e01Files.Count) - install libewf ewfexport or run Expand-EwfToRaw.ps1"
}

# --- Phase 4: compressed carving archives ---
$fcDd = (Get-ChildItem (Join-Path $testdata "file-carving-images\Images") -Filter "*.dd" -ErrorAction SilentlyContinue).Count
$fcBz2 = (Get-ChildItem (Join-Path $testdata "file-carving-images\Images") -Filter "*.dd.bz2" -ErrorAction SilentlyContinue).Count
if ($fcDd -gt 0) {
    Add-Result "file-carving-images" "carve" "READY" "$fcDd .dd extracted; CTest FileCarving_L0_Graphic"
} elseif ($fcBz2 -gt 0) {
    Add-Result "file-carving-images" "carve" "NEEDS_EXTRACT" "$fcBz2 .dd.bz2 in Images/"
}
if (Test-Path (Join-Path $testdata "file-carving-cftt-images\images\graphic-images.tbz2")) {
    $cfttDd = (Get-ChildItem (Join-Path $testdata "file-carving-cftt-images\images") -Recurse -Filter "*.dd" -File -ErrorAction SilentlyContinue).Count
    if ($cfttDd -gt 0) {
        Add-Result "file-carving-cftt-images" "carve" "READY" "$cfttDd .dd extracted from graphic-images.tbz2"
    } else {
        Add-Result "file-carving-cftt-images" "carve" "NEEDS_EXTRACT" "tbz2 present; run Expand-CfttImages.ps1"
    }
} else {
    Add-Result "file-carving-cftt-images" "carve" "NEEDS_DOWNLOAD" "run Download-CfttImages.ps1"
}

$tskRoot = Join-Path $testdata "sleuthkit_test_data"
$tskDd = (Get-ChildItem $tskRoot -Recurse -Include *.dd,*.raw -File -ErrorAction SilentlyContinue).Count
$tskE01 = (Get-ChildItem $tskRoot -Recurse -Filter *.E01 -File -ErrorAction SilentlyContinue).Count
$tskConverted = 0
Get-ChildItem $tskRoot -Recurse -Filter "*.E01" -File -ErrorAction SilentlyContinue | ForEach-Object {
    $dd = [IO.Path]::ChangeExtension($_.FullName, ".dd")
    if (Test-Path $dd) { $script:tskConverted++ }
}
if ($tskDd -gt 0) {
    Add-Result "sleuthkit_test_data" "smoke" "READY" "dd/raw=$tskDd e01=$tskE01 converted=$tskConverted"
} elseif ($tskE01 -gt 0) {
    Add-Result "sleuthkit_test_data" "smoke" "NEEDS_EWF_CONVERT" "e01=$tskE01; run Expand-EwfToRaw.ps1"
} else {
    Add-Result "sleuthkit_test_data" "smoke" "NEEDS_DOWNLOAD" "run Download-SleuthkitImages.ps1"
}

# --- Phase 5: material-only (no disk image) ---
@("napierone-raw-files", "napierone-raw-files-2", "govdocs1", "filetypes1") | ForEach-Object {
    Add-Result $_ "carve-repair-material" "MANUAL" "single-file corpus; use for carving/repair bench, not volume scan"
}

# --- Phase 6: missing CTest deps ---
@(
    @{Name="repair_samples"; Note="run scripts/Generate-RepairSamples.ps1 (needs testdata/media)"},
    @{Name="media"; Note="run testdata/tools/Fetch-MediaSamples.ps1"},
    @{Name="ext4-variants"; Note="optional ext4 variant images for DFR_Linux_Batch"},
    @{Name="DFR"; Note="symlink/copy of cfreds or dfr-test-images if desired"}
) | ForEach-Object {
    $p = Join-Path $testdata $_.Name
    if (-not (Test-Path $p)) {
        Add-Result $_.Name "ctest-gap" "MISSING" $_.Note
    } else {
        Add-Result $_.Name "ctest-gap" "READY" "present"
    }
}

# --- write CSV ---
$results | Export-Csv -Path $csvPath -NoTypeInformation -Encoding UTF8
Write-Log "Report: $csvPath"
Write-Log "Log: $logPath"
Write-Log "Done."
