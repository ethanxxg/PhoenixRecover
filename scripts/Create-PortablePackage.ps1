param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir,
    [string]$Version = "",
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = (& (Join-Path $PSScriptRoot "Get-RepoVersion.ps1") -RepoRoot $repoRoot).Trim()
}
$sourceFull = (Resolve-Path -LiteralPath $SourceDir).Path
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $repoRoot "release"
}
$outputFull = [IO.Path]::GetFullPath($OutputDir)
if ($outputFull.TrimEnd('\') -eq $sourceFull.TrimEnd('\')) {
    throw "OutputDir must be different from SourceDir."
}
New-Item -ItemType Directory -Path $outputFull -Force | Out-Null
$package = Join-Path $outputFull "PhoenixRecover-Portable-$Version.zip"
if (Test-Path -LiteralPath $package) {
    Remove-Item -LiteralPath $package -Force
}
Compress-Archive -Path (Join-Path $sourceFull "*") -DestinationPath $package -CompressionLevel Optimal
if (!(Test-Path -LiteralPath $package -PathType Leaf)) {
    throw "Portable package was not generated: $package"
}
Write-Host "Portable package generated: $package" -ForegroundColor Green
