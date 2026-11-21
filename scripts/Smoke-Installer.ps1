param(
    [Parameter(Mandatory = $true)]
    [string]$InstallerPath,
    [string]$InstallDir = "",
    [int]$TimeoutSec = 120
)

$ErrorActionPreference = "Stop"

if (!(Test-Path -LiteralPath $InstallerPath -PathType Leaf)) {
    throw "安装包不存在: $InstallerPath"
}

$tempRoot = (Resolve-Path $env:TEMP).Path
$ownsInstallDir = [string]::IsNullOrWhiteSpace($InstallDir)
if ($ownsInstallDir) {
    $InstallDir = Join-Path $tempRoot ("PhoenixRecover_InstallerSmoke_" + [guid]::NewGuid().ToString("N"))
}
$installFull = [IO.Path]::GetFullPath($InstallDir)
if (!$installFull.StartsWith($tempRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "为保护用户数据，安装冒烟测试目录必须位于 TEMP 下: $installFull"
}

$sentinel = Join-Path $installFull "crash_reports\user-created-test-data.txt"
$envBackup = @{
    PHOENIX_TEST_MODE = $env:PHOENIX_TEST_MODE
    PHOENIX_MOCK_SCAN_COUNT = $env:PHOENIX_MOCK_SCAN_COUNT
}

try {
    Write-Host "[Smoke] Install into isolated directory: $installFull" -ForegroundColor Cyan
    $args = @("/S", "/D=$installFull")
    $p = Start-Process -FilePath (Resolve-Path -LiteralPath $InstallerPath) -ArgumentList $args -PassThru
    if (!$p.WaitForExit($TimeoutSec * 1000)) {
        try { $p.Kill() } catch {}
        throw "静默安装超时 (${TimeoutSec}s)"
    }
    if ($p.ExitCode -ne 0) {
        throw "静默安装失败，退出码: $($p.ExitCode)"
    }

    $exe = Join-Path $installFull "PhoenixRecover.exe"
    if (!(Test-Path -LiteralPath $exe -PathType Leaf)) {
        throw "安装后未找到主程序: $exe"
    }

    New-Item -ItemType Directory -Path (Split-Path -Parent $sentinel) -Force | Out-Null
    Set-Content -LiteralPath $sentinel -Value "installer smoke test data" -Encoding utf8

    Write-Host "[Smoke] Launch in test mode and terminate" -ForegroundColor Cyan
    $env:PHOENIX_TEST_MODE = "1"
    $env:PHOENIX_MOCK_SCAN_COUNT = "8"
    $app = Start-Process -FilePath $exe -WorkingDirectory $installFull -PassThru
    Start-Sleep -Seconds 3
    try { Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue } catch {}

    $uninstaller = Join-Path $installFull "Uninstall.exe"
    if (!(Test-Path -LiteralPath $uninstaller -PathType Leaf)) {
        throw "未找到卸载器: $uninstaller"
    }

    Write-Host "[Smoke] Uninstall and verify user data is preserved" -ForegroundColor Cyan
    $u = Start-Process -FilePath $uninstaller -ArgumentList "/S" -PassThru
    if (!$u.WaitForExit($TimeoutSec * 1000)) {
        try { $u.Kill() } catch {}
        throw "静默卸载超时 (${TimeoutSec}s)"
    }
    if ($u.ExitCode -ne 0) {
        throw "静默卸载失败，退出码: $($u.ExitCode)"
    }
    if (!(Test-Path -LiteralPath $sentinel -PathType Leaf)) {
        throw "卸载删除了安装目录内的用户测试数据: $sentinel"
    }

    Write-Host "Installer smoke test passed." -ForegroundColor Green
}
finally {
    if ($null -eq $envBackup.PHOENIX_TEST_MODE) {
        Remove-Item Env:PHOENIX_TEST_MODE -ErrorAction SilentlyContinue
    } else {
        $env:PHOENIX_TEST_MODE = $envBackup.PHOENIX_TEST_MODE
    }
    if ($null -eq $envBackup.PHOENIX_MOCK_SCAN_COUNT) {
        Remove-Item Env:PHOENIX_MOCK_SCAN_COUNT -ErrorAction SilentlyContinue
    } else {
        $env:PHOENIX_MOCK_SCAN_COUNT = $envBackup.PHOENIX_MOCK_SCAN_COUNT
    }
    if (Test-Path -LiteralPath $installFull) {
        Remove-Item -LiteralPath $installFull -Recurse -Force
    }
}
