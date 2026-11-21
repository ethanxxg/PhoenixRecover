# 生成 Office Open XML 修复测试样本（最小 docx + 截断损坏版）
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
)

$ErrorActionPreference = "Stop"
$outDir = Join-Path $RepoRoot "testdata\repair_samples"
$work = Join-Path $env:TEMP "phoenix_office_sample_build"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Force -Path $work, $outDir | Out-Null

$relsDir = Join-Path $work "_rels"
$wordDir = Join-Path $work "word"
New-Item -ItemType Directory -Force -Path $relsDir, $wordDir | Out-Null

$ctXml = @'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
</Types>
'@
[System.IO.File]::WriteAllText((Join-Path $work '[Content_Types].xml'), $ctXml, (New-Object System.Text.UTF8Encoding $false))

@'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>
'@ | Out-File -FilePath (Join-Path $relsDir ".rels") -Encoding utf8

@'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:body><w:p><w:r><w:t>PhoenixRecover repair sample</w:t></w:r></w:p></w:body>
</w:document>
'@ | Out-File -FilePath (Join-Path $wordDir "document.xml") -Encoding utf8

$zipTemp = Join-Path $env:TEMP "phoenix_normal_docx.zip"
if (Test-Path $zipTemp) { Remove-Item $zipTemp -Force }
Compress-Archive -Path (Join-Path $work "*") -DestinationPath $zipTemp -CompressionLevel Optimal

$normal = Join-Path $outDir "normal.docx"
if (Test-Path $normal) { Remove-Item $normal -Force }
Copy-Item $zipTemp $normal -Force
Remove-Item $zipTemp -Force

$bytes = [IO.File]::ReadAllBytes($normal)
if ($bytes.Length -le 120) { throw "normal.docx too small" }
[IO.File]::WriteAllBytes((Join-Path $outDir "truncated.docx"), $bytes[0..($bytes.Length - 101)])

Write-Host "Generated: $normal and truncated.docx"
