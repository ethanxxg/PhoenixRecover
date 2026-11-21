param(
    [Parameter(Mandatory = $true)]
    [string]$PackagePath,
    [string]$InstallDir = "",
    [int]$TimeoutSec = 30
)

$ErrorActionPreference = "Stop"
if (!(Test-Path -LiteralPath $PackagePath -PathType Leaf)) {
    throw "便携包不存在: $PackagePath"
}
$tempRoot = (Resolve-Path $env:TEMP).Path
$ownsInstallDir = [string]::IsNullOrWhiteSpace($InstallDir)
if ($ownsInstallDir) {
    $InstallDir = Join-Path $tempRoot ("PhoenixRecover_PortableUpgrade_" + [guid]::NewGuid().ToString("N"))
}
$installFull = [IO.Path]::GetFullPath($InstallDir)
if (!$installFull.StartsWith($tempRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "为保护用户数据，升级冒烟测试目录必须位于 TEMP 下: $installFull"
}

$userFile = Join-Path $installFull "crash_reports\user-created-test-data.txt"
try {
    New-Item -ItemType Directory -Path $installFull -Force | Out-Null
    Expand-Archive -LiteralPath $PackagePath -DestinationPath $installFull -Force
    if (!(Test-Path -LiteralPath (Join-Path $installFull "PhoenixRecover.exe") -PathType Leaf)) {
        throw "首次安装后缺少 PhoenixRecover.exe"
    }

    New-Item -ItemType Directory -Path (Split-Path -Parent $userFile) -Force | Out-Null
    Set-Content -LiteralPath $userFile -Value "must survive upgrade" -Encoding utf8
    $before = Get-Content -LiteralPath $userFile -Raw

    Expand-Archive -LiteralPath $PackagePath -DestinationPath $installFull -Force
    if (!(Test-Path -LiteralPath $userFile -PathType Leaf)) {
        throw "升级删除了用户测试数据: $userFile"
    }
    $after = Get-Content -LiteralPath $userFile -Raw
    if ($before -ne $after) {
        throw "升级修改了用户测试数据: $userFile"
    }
    Write-Host "Portable upgrade smoke test passed." -ForegroundColor Green
}
finally {
    if (Test-Path -LiteralPath $installFull) {
        Remove-Item -LiteralPath $installFull -Recurse -Force
    }
}
