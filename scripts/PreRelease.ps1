# PhoenixRecover 发布前门禁：Release 构建 + CTest +（可选）金标 MD5 严对比
# 用法:
#   powershell -ExecutionPolicy Bypass -File scripts/PreRelease.ps1 -QtPrefix "C:\Qt\5.14.2\msvc2017_64"
#   powershell -ExecutionPolicy Bypass -File scripts/PreRelease.ps1 -TestsOnly
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$QtPrefix = "",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",
    [switch]$TestsOnly,
    [switch]$SkipInstaller,
    [string]$RecoveryOutput = "",
    [string]$ExpectedCsv = "",
    [string]$CompareReport = ""
)

$ErrorActionPreference = "Stop"

$regressArgs = @(
    "-ExecutionPolicy", "Bypass",
    "-File", (Join-Path $RepoRoot "scripts\Regress.ps1"),
    "-RepoRoot", $RepoRoot,
    "-Config", $Config
)

if ($TestsOnly) {
    $regressArgs += "-TestsOnly"
} elseif (![string]::IsNullOrEmpty($QtPrefix)) {
    $regressArgs += "-QtPrefix", $QtPrefix
} elseif ($env:CMAKE_PREFIX_PATH) {
    $regressArgs += "-QtPrefix", $env:CMAKE_PREFIX_PATH
}

if (![string]::IsNullOrEmpty($RecoveryOutput)) {
    $regressArgs += "-RecoveryOutput", $RecoveryOutput
    if ($ExpectedCsv) { $regressArgs += "-ExpectedCsv", $ExpectedCsv }
    if ($CompareReport) { $regressArgs += "-CompareReport", $CompareReport }
}

Write-Host "=== PreRelease: Regress (build + ctest) ===" -ForegroundColor Cyan
& powershell.exe @regressArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (!$TestsOnly -and !$SkipInstaller) {
    if ([string]::IsNullOrEmpty($QtPrefix) -and $env:CMAKE_PREFIX_PATH) {
        $QtPrefix = $env:CMAKE_PREFIX_PATH
    }
    if ([string]::IsNullOrEmpty($QtPrefix)) {
        Write-Warning "未指定 QtPrefix，跳过 Build-Release（仅完成测试门禁）。"
        Write-Output "PreRelease: OK (tests only, no installer build)"
        exit 0
    }

    Write-Host "=== PreRelease: Build-Release (no smoke) ===" -ForegroundColor Cyan
    $buildArgs = @(
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $RepoRoot "scripts\Build-Release.ps1"),
        "-QtPrefix", $QtPrefix,
        "-Config", $Config,
        "-Installer", "NSIS"
    )
    & powershell.exe @buildArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Output "PreRelease: OK"
exit 0
