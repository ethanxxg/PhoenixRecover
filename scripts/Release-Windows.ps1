<#
.SYNOPSIS
    本机 Windows 正式发布快捷脚本（Qt 5.14.2 msvc2017_64 + NSIS）。

.DESCRIPTION
    默认 QtPrefix 为本仓库约定路径；可通过 -QtPrefix 覆盖。
    -Mode Gate      : Regress + RepairFunctionalTest（不发安装包）
    -Mode Package   : Build-Release + NSIS + 冒烟（默认）
    -Mode Full      : PreRelease（测试）后 Package
#>
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$QtPrefix = "C:\Qt\Qt5.14.2\5.14.2\msvc2017_64",
    [ValidateSet("Gate", "Package", "Full")]
    [string]$Mode = "Package",
    [string]$Version = "",
    [switch]$TestsOnly,
    [string]$SignCommand = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = & (Join-Path $PSScriptRoot "Get-RepoVersion.ps1") -RepoRoot $RepoRoot
}

if (!(Test-Path (Join-Path $QtPrefix "lib\cmake\Qt5Core"))) {
    if (Test-Path (Join-Path $QtPrefix "..\lib\cmake\Qt5Core")) {
        $QtPrefix = (Resolve-Path (Join-Path $QtPrefix "..")).Path
        Write-Host "QtPrefix 已自动调整为 Kit 根目录: $QtPrefix" -ForegroundColor Yellow
    } else {
        throw "无效的 QtPrefix: $QtPrefix （需要包含 lib\cmake\Qt5Core）"
    }
}

Push-Location $RepoRoot
try {
    switch ($Mode) {
        "Gate" {
            $args = @(
                "-ExecutionPolicy", "Bypass",
                "-File", (Join-Path $RepoRoot "scripts\Regress.ps1"),
                "-QtPrefix", $QtPrefix,
                "-Config", "Release",
                "-RepairFunctionalTest"
            )
            if ($TestsOnly) { $args += "-TestsOnly" }
            & powershell.exe @args
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
        "Full" {
            & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $RepoRoot "scripts\PreRelease.ps1") `
                -QtPrefix $QtPrefix -Config Release -SkipInstaller
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
            # fall through to Package
        }
        "Package" { }
    }

    if ($Mode -eq "Gate" -or $TestsOnly) {
        Write-Output "Release-Windows: Gate OK"
        exit 0
    }

    $buildArgs = @(
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $RepoRoot "scripts\Build-Release.ps1"),
        "-QtPrefix", $QtPrefix,
        "-Config", "Release",
        "-Installer", "NSIS",
        "-Version", $Version,
        "-RunSmokeTest"
    )
    if ($SignCommand) { $buildArgs += "-SignCommand"; $buildArgs += $SignCommand }

    & powershell.exe @buildArgs
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
