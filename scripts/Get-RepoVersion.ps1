param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
)

$path = Join-Path $RepoRoot "VERSION"
if (!(Test-Path $path)) {
    throw "Missing VERSION file: $path"
}
return (Get-Content $path -Raw).Trim()
