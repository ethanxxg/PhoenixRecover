# PhoenixRecover 回归入口：配置/编译 → CTest →（可选）恢复结果 MD5 严对比
# 用法（在仓库根目录）:
#   powershell -ExecutionPolicy Bypass -File "scripts/Regress.ps1" -QtPrefix "C:\Qt\5.14.2\msvc2017_64"
# 仅单测（无 Qt）: -TestsOnly
# 全量 + 金标对比: -QtPrefix "..." -RecoveryOutput "D:\gold\recover"
# 环境变量: CMAKE_PREFIX_PATH 可与 -QtPrefix 二选一
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$BuildDir = "",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",
    [string]$QtPrefix = "",
    [switch]$TestsOnly,
    # When set, only run the core recovery/fragment baseline gates.
    [switch]$CoreGates,
    [string]$RecoveryOutput = "",
    [string]$ExpectedCsv = "",
    [string]$CompareReport = "",
    [switch]$SkipConfigure,
    [switch]$RepairFunctionalTest,
    # Start scripts/Watch-StuckTests.ps1 in background during ctest (Windows).
    [switch]$WatchStuck,
    [int]$WatchStuckMinutes = 20
)

$ErrorActionPreference = "Stop"

$buildPath = if ($BuildDir) {
    if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $RepoRoot $BuildDir }
} else {
    Join-Path $RepoRoot "build\regress"
}

if ([string]::IsNullOrEmpty($QtPrefix) -and $env:CMAKE_PREFIX_PATH) {
    $QtPrefix = $env:CMAKE_PREFIX_PATH
}

Push-Location $RepoRoot
try {
    if (!$SkipConfigure) {
        $cmakeArgs = @("-S", $RepoRoot, "-B", $buildPath)
        if ($TestsOnly) {
            $cmakeArgs += @("-DPHOENIX_BUILD_GUI=OFF")
            if (![string]::IsNullOrEmpty($QtPrefix)) {
                $cmakeArgs += @("-DCMAKE_PREFIX_PATH=$QtPrefix")
            }
        } else {
            if (![string]::IsNullOrEmpty($QtPrefix)) {
                $cmakeArgs += @("-DCMAKE_PREFIX_PATH=$QtPrefix")
            }
        }
        & cmake @cmakeArgs
        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed: $LASTEXITCODE" }
    }

    $buildArgs = @("--build", $buildPath, "--config", $Config)
    & cmake @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed: $LASTEXITCODE" }

    $ctestArgs = @("--test-dir", $buildPath, "-C", $Config, "--output-on-failure")
    if ($CoreGates) {
        # Keep this list short and Windows-focused: fragment quality curve, key dataset smoke tests.
        $ctestArgs += @("-R", "^(CFReDS_DFR05_QualityCurve|CFReDS_DFR05_RecoverSmoke|CFReDS_BraidNtfs_FragmentStitch|MediaFormatQualityBaseline|FatFragmentStitchSynthetic|FragmentFatStructured_QualityGate|FragmentNtfsStructured_QualityGate|NtfsFragmentStitch_Dispatch|GraphCarveFragmentStitch|CarveGraphMerge|DFTT_FAT_Undelete|DFTT_NTFS_Undelete|RecoveryModules_FsTypeRouting|PartitionRecovery_ProbeImage|CFReDS_EXT4_DFR01|DFR_Linux_Batch|SqliteRecover_PageLevel)$")
    }

    $watchJob = $null
    if ($WatchStuck) {
        $watchScript = Join-Path $RepoRoot "scripts\Watch-StuckTests.ps1"
        if (!(Test-Path $watchScript)) { throw "Missing $watchScript" }
        $watchJob = Start-Job -ScriptBlock {
            param($script, $minutes)
            & powershell -NoProfile -ExecutionPolicy Bypass -File $script -StuckMinutes $minutes
        } -ArgumentList $watchScript, $WatchStuckMinutes
        Write-Output "Watch-StuckTests: background job started (threshold=${WatchStuckMinutes}m)"
    }

    try {
        & ctest @ctestArgs
        if ($LASTEXITCODE -ne 0) { throw "ctest failed: $LASTEXITCODE" }
    }
    finally {
        if ($watchJob) {
            Stop-Job -Job $watchJob -ErrorAction SilentlyContinue
            Remove-Job -Job $watchJob -Force -ErrorAction SilentlyContinue
            Write-Output "Watch-StuckTests: stopped"
        }
    }

    if ($RepairFunctionalTest) {
        $repairScript = Join-Path $RepoRoot "testdata\tools\Run-RepairSamplesTest.ps1"
        $rp = @{
            RepoRoot  = $RepoRoot
            BuildDir  = $buildPath
            Config    = $Config
        }
        if ($QtPrefix) { $rp.QtPrefix = $QtPrefix }
        & powershell -NoProfile -ExecutionPolicy Bypass -File $repairScript @rp
        if ($LASTEXITCODE -ne 0) { throw "Run-RepairSamplesTest failed: $LASTEXITCODE" }
    }

    if (![string]::IsNullOrEmpty($RecoveryOutput)) {
        $compareScript = Join-Path $RepoRoot "testdata\tools\Compare-RecoveryResults.ps1"
        if (!(Test-Path $compareScript)) { throw "Missing $compareScript" }

        $exp = if ($ExpectedCsv) { $ExpectedCsv } else { "testdata\manifests\expected_results.csv" }
        $out = if ($CompareReport) { $CompareReport } else { "testdata\manifests\compare_report.csv" }
        if (![IO.Path]::IsPathRooted($exp)) { $exp = Join-Path $RepoRoot $exp }
        if (![IO.Path]::IsPathRooted($out)) { $out = Join-Path $RepoRoot $out }

        $p = Start-Process -FilePath "powershell.exe" -ArgumentList @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", $compareScript,
            "-RecoveredRoot", $RecoveryOutput,
            "-ExpectedCsv", $exp,
            "-OutReport", $out,
            "-Strict"
        ) -PassThru -Wait -NoNewWindow
        if ($p.ExitCode -ne 0) { throw "Compare-RecoveryResults failed: exit code $($p.ExitCode) (1=hash/size mismatch, 2=missing file)" }
    }

    Write-Output "Regress: OK (build=$buildPath config=$Config)"
    exit 0
}
finally {
    Pop-Location
}
