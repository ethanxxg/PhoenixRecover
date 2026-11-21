<#!
.SYNOPSIS
    Build a PhoenixRecover release directory and optionally create an installer.

.NOTES
    NSIS is used when available. Portable creates a zip package and is useful
    for safe local validation when NSIS is not installed.
#>
param(
    [string]$QtPrefix = "",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",
    [string]$OutputDir = "release",
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$BuildDir = "",
    [switch]$SkipBuild,
    [switch]$SkipDeploy,
    [ValidateSet("None", "NSIS", "WiX", "Portable")]
    [string]$Installer = "NSIS",
    [string]$Version = "",
    [switch]$RunSmokeTest,
    [string]$InstallDir = "",
    [string]$SignCommand = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = (& (Join-Path $PSScriptRoot "Get-RepoVersion.ps1") -RepoRoot $RepoRoot).Trim()
}
if ([string]::IsNullOrWhiteSpace($QtPrefix) -and $env:CMAKE_PREFIX_PATH) {
    $QtPrefix = $env:CMAKE_PREFIX_PATH
}
if ([string]::IsNullOrWhiteSpace($QtPrefix)) {
    throw "QtPrefix is required."
}

$repoFull = (Resolve-Path -LiteralPath $RepoRoot).Path
$qtFull = (Resolve-Path -LiteralPath $QtPrefix).Path
$buildPath = if ($BuildDir) {
    if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repoFull $BuildDir }
} else {
    Join-Path $repoFull "build\release"
}
$outputPath = if ([IO.Path]::IsPathRooted($OutputDir)) { $OutputDir } else { Join-Path $repoFull $OutputDir }
$windeployqt = Join-Path $qtFull "bin\windeployqt.exe"

Push-Location $repoFull
try {
    if (!$SkipBuild) {
        Write-Host "[1/8] Configure CMake" -ForegroundColor Cyan
        & cmake -S $repoFull -B $buildPath -DCMAKE_PREFIX_PATH=$qtFull
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

        Write-Host "[2/8] Build $Config" -ForegroundColor Cyan
        & cmake --build $buildPath --config $Config --parallel
        if ($LASTEXITCODE -ne 0) { throw "Build failed." }
    }

    New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
    $exeCandidates = @(
        (Join-Path $buildPath "$Config\PhoenixRecover.exe"),
        (Join-Path $buildPath "PhoenixRecover.exe")
    )
    $exeSource = $exeCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (!$exeSource) { throw "PhoenixRecover.exe was not found in the build output." }

    Write-Host "[3/8] Copy executable" -ForegroundColor Cyan
    Copy-Item -LiteralPath $exeSource -Destination (Join-Path $outputPath "PhoenixRecover.exe") -Force

    Write-Host "[4/8] Deploy Qt dependencies" -ForegroundColor Cyan
    if (!$SkipDeploy -and (Test-Path -LiteralPath $windeployqt -PathType Leaf)) {
        & $windeployqt --release --no-translations --no-opengl-sw --no-system-d3d-compiler (Join-Path $outputPath "PhoenixRecover.exe")
        if ($LASTEXITCODE -ne 0) { Write-Warning "windeployqt returned $LASTEXITCODE." }
    } elseif (!$SkipDeploy) {
        Write-Warning "windeployqt was not found: $windeployqt"
    }

    Write-Host "[5/8] Copy release metadata" -ForegroundColor Cyan
    foreach ($file in @("LICENSE", "README.md", "VERSION")) {
        $source = Join-Path $repoFull $file
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination $outputPath -Force
        }
    }
    $docsOut = Join-Path $outputPath "docs"
    New-Item -ItemType Directory -Path $docsOut -Force | Out-Null
    foreach ($file in @("CHANGELOG.md", "USER_GUIDE.md", "FAQ.md", "RELEASE_CHECKLIST.md", "THIRD_PARTY_NOTICE.md")) {
        $source = Join-Path $repoFull "docs\$file"
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination $docsOut -Force
        }
    }

    $installerPath = ""
    Write-Host "[6/8] Create package: $Installer" -ForegroundColor Cyan
    switch ($Installer) {
        "None" { }
        "NSIS" {
            $makensis = Get-Command makensis.exe -ErrorAction SilentlyContinue
            if (!$makensis) { throw "makensis.exe was not found. Use -Installer Portable or install NSIS." }
            $script = Join-Path $repoFull "scripts\Create-Installer.ps1"
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script `
                -RepoRoot $repoFull -SourceDir $outputPath -Version $Version
            if ($LASTEXITCODE -ne 0) { throw "NSIS package generation failed." }
            $installerPath = Join-Path $outputPath "PhoenixRecover-Setup-$Version.exe"
        }
        "Portable" {
            $portableOutput = Join-Path (Split-Path $outputPath -Parent) "packages"
            $script = Join-Path $repoFull "scripts\Create-PortablePackage.ps1"
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script `
                -SourceDir $outputPath -Version $Version -OutputDir $portableOutput
            if ($LASTEXITCODE -ne 0) { throw "Portable package generation failed." }
            $installerPath = Join-Path $portableOutput "PhoenixRecover-Portable-$Version.zip"
        }
        "WiX" { throw "WiX packaging is not implemented. Use NSIS or Portable." }
    }

    Write-Host "[7/8] Smoke test" -ForegroundColor Cyan
    if ($RunSmokeTest -and $installerPath) {
        if ($Installer -eq "Portable") {
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repoFull "scripts\Smoke-PortableUpgrade.ps1") `
                -PackagePath $installerPath
        } else {
            $smokeArgs = @("-InstallerPath", $installerPath)
            if (![string]::IsNullOrWhiteSpace($InstallDir)) { $smokeArgs += @("-InstallDir", $InstallDir) }
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repoFull "scripts\Smoke-Installer.ps1") @smokeArgs
        }
        if ($LASTEXITCODE -ne 0) { throw "Package smoke test failed." }
    } else {
        Write-Host "Smoke test skipped." -ForegroundColor Yellow
    }

    Write-Host "[8/8] Optional signing" -ForegroundColor Cyan
    if ($SignCommand) {
        $command = $SignCommand.Replace("{output}", '"' + $outputPath + '"')
        cmd.exe /c $command
        if ($LASTEXITCODE -ne 0) { throw "Signing command failed." }
    } else {
        Write-Host "Signing skipped." -ForegroundColor Yellow
    }

    Write-Host "Release completed." -ForegroundColor Green
    Write-Host "Release directory: $outputPath"
    if ($installerPath) { Write-Host "Package: $installerPath" }
}
finally {
    Pop-Location
}
