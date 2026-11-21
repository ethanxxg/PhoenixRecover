[CmdletBinding()]
param(
    [ValidateRange(1, 500000)]
    [int]$MockCount = 120
)

$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe = Join-Path $workspace 'dist\PhoenixRecover.exe'
$dist = Join-Path $workspace 'dist'

if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "未找到交付程序：$exe"
}

# 测试启动入口强制启用安全模式：不查询物理 SMART，不启动真实卷、镜像或分区扫描，
# 不执行文件修复导出和恢复写出。验证数据只来自模拟扫描结果或 testdata 镜像。
$env:PHOENIX_TEST_MODE = '1'
$env:PHOENIX_MOCK_SCAN_COUNT = [string]$MockCount

Write-Host 'PhoenixRecover 测试模式已启用' -ForegroundColor Green
Write-Host "模拟结果数量：$MockCount"
Write-Host '不会访问物理设备、不会制作真实镜像、不会向真实磁盘写入数据。'
Write-Host "程序：$exe"

Start-Process -FilePath $exe -WorkingDirectory $dist
