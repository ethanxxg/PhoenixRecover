# 从 testdata/media 重新生成 repair_samples（与 repair_samples.csv 规格一致）
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
)

$ErrorActionPreference = "Stop"

$media = Join-Path $RepoRoot "testdata\media"
$outDir = Join-Path $RepoRoot "testdata\repair_samples"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$jpgSrc = Join-Path $media "xiph-poster_big.jpg"
$mp4Src = Join-Path $media "sample-5s.mp4"
if (!(Test-Path $jpgSrc)) { throw "Missing $jpgSrc" }
if (!(Test-Path $mp4Src)) { throw "Missing $mp4Src" }

# normal.zip：压缩 media 下小文件
$zipPath = Join-Path $outDir "normal.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path @($jpgSrc) -DestinationPath $zipPath -CompressionLevel Optimal

# truncated.jpg：80%
$jpgBytes = [IO.File]::ReadAllBytes($jpgSrc)
$truncLen = [int]($jpgBytes.Length * 0.8)
[IO.File]::WriteAllBytes((Join-Path $outDir "truncated.jpg"), $jpgBytes[0..($truncLen - 1)])

# truncated.zip：去掉末尾 100 字节
$zipBytes = [IO.File]::ReadAllBytes($zipPath)
if ($zipBytes.Length -le 100) { throw "normal.zip too small" }
[IO.File]::WriteAllBytes((Join-Path $outDir "truncated.zip"), $zipBytes[0..($zipBytes.Length - 101)])

# original.mp4 + corrupted_ftyp.mp4（ftyp 前 4 字节清零）
$mp4Bytes = [IO.File]::ReadAllBytes($mp4Src)
[IO.File]::WriteAllBytes((Join-Path $outDir "original.mp4"), $mp4Bytes)
$corrupt = $mp4Bytes.Clone()
if ($corrupt.Length -lt 8) { throw "sample-5s.mp4 too small" }
$corrupt[0] = 0; $corrupt[1] = 0; $corrupt[2] = 0; $corrupt[3] = 0
[IO.File]::WriteAllBytes((Join-Path $outDir "corrupted_ftyp.mp4"), $corrupt)

Write-Host "Generated repair_samples under $outDir"
Write-Host "Run: testdata\tools\Verify-RepairSamplesIntegrity.ps1 to check MD5 (re-run after updating CSV if sizes changed)."

$verify = Join-Path $RepoRoot "testdata\tools\Verify-RepairSamplesIntegrity.ps1"
if (Test-Path $verify) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $verify -RepoRoot $RepoRoot
}
