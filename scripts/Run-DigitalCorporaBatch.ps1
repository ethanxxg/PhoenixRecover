# Phased digital-corpora validation: E01 -> build/ewf-cache, then recover batch test.

param(

    [Parameter(Mandatory = $true)]

    [ValidateSet('1', '2', '3', '4', '5')]

    [string]$Batch,

    [string]$RepoRoot = "",

    [string]$BuildDir = "build/win2019-ninja",

    [switch]$StrictRecover

)



$ErrorActionPreference = "Stop"

if (-not $RepoRoot) {

    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

}



$Batches = @{

    '1' = @{

        Name    = 'core-small'

        Only    = @('canon2', 'nps-2010-emails', 'ntfs1-gen', 'ubnist1.gen3.raw', 'casper-rw.gen0')

        Convert = @()

    }

    '2' = @{

        Name    = 'linux-medium'

        Only    = @('ubnist1.gen0', 'ubnist1.gen1', 'linux-dc3dd-first', 'casper-rw.gen1', 'casper-rw.gen2', 'casper-rw.gen3')

        Convert = @('ubnist1.gen0.E01', 'ubnist1.gen1.E01', 'ubnist1.casper-rw.gen1.E01', 'ubnist1.casper-rw.gen2.E01', 'ubnist1.casper-rw.gen3.E01', 'linux-dc3dd-first.E01')

    }

    '3' = @{

        Name    = 'domexusers-large'

        Only    = @('domexusers\nps-2009-domexusers.dd')

        Convert = @('nps-2009-domexusers.E01')

    }

    '4' = @{

        Name    = 'usb-pending'

        Only    = @('linux-dc3dd-second', 'windows-ftkimager-first', 'windows-ftkimager-second')

        Convert = @('linux-dc3dd-second.E01', 'windows-ftkimager-first.E01', 'windows-ftkimager-second.E01')

    }

    '5' = @{

        Name    = 'smoke-wiped'

        Only    = @('wiped_disk')

        Convert = @()

    }

}



function Get-EwfCacheDdPath {

    param([string]$RepoRoot, [string]$E01Path)

    $srcRoot = Join-Path $RepoRoot "testdata\digital-corpora-disk-images"

    $cacheRoot = Join-Path $RepoRoot "build\ewf-cache\digital-corpora-disk-images"

    $rel = $E01Path.Substring($srcRoot.Length).TrimStart('\', '/')

    return Join-Path $cacheRoot ([IO.Path]::ChangeExtension($rel, ".dd"))

}



$spec = $Batches[$Batch]

$reportDir = Join-Path $RepoRoot "docs\reports"

New-Item -ItemType Directory -Force -Path $reportDir | Out-Null



$exe = Join-Path $RepoRoot "$BuildDir\tests\test_digital_corpora_batch.exe"

if (-not (Test-Path $exe)) {

    Write-Host "Building..."

    Push-Location (Join-Path $RepoRoot $BuildDir)

    cmd /c _build_release.bat | Out-Null

    Pop-Location

}



$dataRoot = Join-Path $RepoRoot "testdata\digital-corpora-disk-images"

$cacheRoot = Join-Path $RepoRoot "build\ewf-cache\digital-corpora-disk-images"

$pyConvert = Join-Path $RepoRoot "testdata\tools\Expand-EwfToRaw.py"



# E01 -> build/ewf-cache (never writes into testdata)

if ($spec.Convert.Count -gt 0) {

    Write-Host "=== Batch $Batch convert -> cache ($($spec.Name)) ==="

    Write-Host "  cache: $cacheRoot"

    foreach ($e01Name in $spec.Convert) {

        $found = Get-ChildItem $dataRoot -Recurse -Filter $e01Name -File -ErrorAction SilentlyContinue | Select-Object -First 1

        if (-not $found) {

            Write-Warning "E01 not found: $e01Name"

            continue

        }

        $dd = Get-EwfCacheDdPath -RepoRoot $RepoRoot -E01Path $found.FullName

        if ((Test-Path $dd) -and (Get-Item $dd).Length -gt 512) {

            Write-Host "  skip (cache dd exists): $($found.Name)"

            continue

        }

        $ddDir = Split-Path $dd -Parent

        if ($ddDir -and !(Test-Path $ddDir)) {

            New-Item -ItemType Directory -Force -Path $ddDir | Out-Null

        }

        Write-Host "  converting $($found.Name) -> cache ..."

        & python $pyConvert $found.FullName $dd

        if ($LASTEXITCODE -ne 0) { throw "E01 convert failed: $($found.Name)" }

    }

}



$onlyStr = ($spec.Only -join ';')

$env:PHOENIX_DC_ONLY = $onlyStr

$env:PHOENIX_DC_EWF_CACHE = $cacheRoot

$env:PHOENIX_DC_REPORT_CSV = Join-Path $reportDir "digital_corpora_batch${Batch}_results.csv"

$env:PHOENIX_DC_FAIL_CSV = Join-Path $reportDir "digital_corpora_batch${Batch}_failures.csv"

if ($StrictRecover) { $env:PHOENIX_DC_STRICT_RECOVER = '1' } else { Remove-Item Env:PHOENIX_DC_STRICT_RECOVER -ErrorAction SilentlyContinue }



$stdoutLog = Join-Path $reportDir "digital_corpora_batch${Batch}_run.log"

$stderrLog = Join-Path $reportDir "digital_corpora_batch${Batch}_stderr.log"



Write-Host "=== Batch $Batch validate ($($spec.Name)) ==="

Write-Host "PHOENIX_DC_ONLY=$onlyStr"

Write-Host "PHOENIX_DC_EWF_CACHE=$cacheRoot"

Write-Host "CSV -> $($env:PHOENIX_DC_REPORT_CSV)"



& $exe 1> $stdoutLog 2> $stderrLog

$code = $LASTEXITCODE



Get-Content $stdoutLog -Tail 15

Write-Host "exit=$code"



if ($code -eq 0) {

    Write-Host ""

    Write-Host "Batch $Batch PASSED."

    Write-Host "testdata sources are never deleted."

    Write-Host "To free cache space after review:"

    Write-Host "  powershell -File scripts\Release-DigitalCorporaSpace.ps1 -AfterBatch $Batch -WhatIf"

    Write-Host "  powershell -File scripts\Release-DigitalCorporaSpace.ps1 -AfterBatch $Batch -Confirm"

}



exit $code

