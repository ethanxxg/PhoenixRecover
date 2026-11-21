param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$SourceDir,
    [string]$Version = "",
    [string]$OutputDir = "",
    [string]$InstallDir = "$env:ProgramFiles\\PhoenixRecover"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = & (Join-Path $PSScriptRoot "Get-RepoVersion.ps1") -RepoRoot $RepoRoot
}

if ([string]::IsNullOrEmpty($SourceDir)) {
    throw "缺少 -SourceDir 参数（发布目录）"
}
if ([string]::IsNullOrEmpty($OutputDir)) {
    $OutputDir = $SourceDir
}

$nsisScript = Join-Path $RepoRoot "installer\PhoenixRecover.nsi"
if (!(Test-Path $nsisScript)) {
    throw "未找到 NSIS 模板: $nsisScript"
}

$makensis = (Get-Command makensis.exe -ErrorAction SilentlyContinue)
if (!$makensis) {
    throw "未找到 makensis.exe，请先安装 NSIS 并确保在 PATH 中"
}

$argList = @(
    "/DAPP_VERSION=$Version",
    "/DAPP_SOURCE_DIR=$SourceDir",
    "/DAPP_OUTPUT_DIR=$OutputDir",
    "/DAPP_INSTALL_DIR=$InstallDir",
    $nsisScript
)

& $makensis.Source @argList
if ($LASTEXITCODE -ne 0) {
    throw "makensis 失败: exit code $LASTEXITCODE"
}

$out = Join-Path $OutputDir "PhoenixRecover-Setup-$Version.exe"
if (!(Test-Path $out)) {
    throw "安装包未生成: $out"
}

Write-Host "NSIS installer generated: $out" -ForegroundColor Green
