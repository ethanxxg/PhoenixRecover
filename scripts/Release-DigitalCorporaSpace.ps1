# Free disk space by removing E01-derived .dd from build/ewf-cache only.
# testdata/ is NEVER touched (E01, raw, aff, dmg, local .dd stay).
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('1', '2', '3', '4', '5')]
    [string]$AfterBatch,
    [string]$RepoRoot = "",
    [switch]$WhatIf,
    [switch]$Confirm
)

$ErrorActionPreference = "Stop"
if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

$ReleaseAfter = @{
    '1' = @(
        'ubnist1.gen0.dd', 'ubnist1.gen1.dd', 'ubnist1.gen2.dd',
        'windows-ftkimager-first.dd', 'windows-ftkimager-second.dd'
    )
    '2' = @(
        'ubnist1.casper-rw.gen1.dd', 'ubnist1.casper-rw.gen2.dd', 'ubnist1.casper-rw.gen3.dd',
        'linux-dc3dd-first.dd', 'ubnist1.gen0.dd', 'ubnist1.gen1.dd'
    )
    '3' = @('nps-2009-domexusers.dd')
    '4' = @(
        'linux-dc3dd-second.dd', 'windows-ftkimager-first.dd', 'windows-ftkimager-second.dd'
    )
    '5' = @()
}

$names = $ReleaseAfter[$AfterBatch]
if (-not $names -or $names.Count -eq 0) {
    Write-Host "Batch $AfterBatch : nothing scheduled for cache release."
    exit 0
}

if (-not $WhatIf -and -not $Confirm) {
    Write-Host "Refusing to delete without -Confirm. Review with -WhatIf first."
    Write-Host "  powershell -File scripts\Release-DigitalCorporaSpace.ps1 -AfterBatch $AfterBatch -WhatIf"
    Write-Host "  powershell -File scripts\Release-DigitalCorporaSpace.ps1 -AfterBatch $AfterBatch -Confirm"
    Write-Host "(Only removes files under build\ewf-cache\; testdata is never deleted.)"
    exit 1
}

$cacheRoot = Join-Path $RepoRoot "build\ewf-cache\digital-corpora-disk-images"
$logPath = Join-Path $RepoRoot "docs\reports\digital_corpora_release.log"

Write-Host "Cache release batch $AfterBatch (testdata preserved):"
Write-Host "  cache root: $cacheRoot"

$freed = 0
$lines = @("=== Cache release after batch $AfterBatch $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ===")

foreach ($name in $names) {
    if (-not (Test-Path $cacheRoot)) { continue }
    $hits = Get-ChildItem $cacheRoot -Recurse -Filter $name -File -ErrorAction SilentlyContinue
    foreach ($f in $hits) {
        $sz = $f.Length
        $lines += "  $($f.FullName)  $([math]::Round($sz/1GB,3)) GiB"
        if ($WhatIf) {
            Write-Host "[WhatIf] remove $($f.FullName) ($([math]::Round($sz/1MB,1)) MB)"
        } else {
            Remove-Item $f.FullName -Force
            Write-Host "Removed $([math]::Round($sz/1MB,1)) MB  $($f.Name)"
        }
        $freed += $sz
    }
}

$lines += "Total freed: $([math]::Round($freed/1GB,2)) GiB (cache only)"
$lines | Add-Content -Path $logPath -Encoding UTF8
Write-Host "Total freed: $([math]::Round($freed/1GB,2)) GiB (logged to $logPath)"

wmic logicaldisk where "DeviceID='D:'" get FreeSpace /format:list 2>$null
