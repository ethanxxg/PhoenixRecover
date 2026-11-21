# 监视本地构建/测试是否“卡住”（长时间无进展）。
# 用法:
#   powershell -ExecutionPolicy Bypass -File scripts/Watch-StuckTests.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/Watch-StuckTests.ps1 -StuckMinutes 10 -KillOnStuck
# 在另一个窗口跑 ctest 时，本脚本每 IntervalSeconds 巡检一次。
param(
    [int]$IntervalSeconds = 60,
    [int]$StuckMinutes = 20,
    [switch]$KillOnStuck,
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
)

$ErrorActionPreference = "Continue"
$names = @('ctest', 'cmake', 'msbuild', 'ninja', 'cl', 'link')
$stuckThreshold = [TimeSpan]::FromMinutes($StuckMinutes)

function Get-SuspectProcesses {
    Get-Process -ErrorAction SilentlyContinue | Where-Object {
        ($names -contains $_.ProcessName) -or ($_.ProcessName -like 'test_*')
    }
}

function Emit-StuckAlert([string]$Message) {
    Write-Output "STUCK_ALERT $Message"
}

Write-Output "Watch-StuckTests: interval=${IntervalSeconds}s threshold=${StuckMinutes}m kill=$KillOnStuck"

while ($true) {
    $now = Get-Date
    $suspects = @(Get-SuspectProcesses)
    if ($suspects.Count -eq 0) {
        Write-Output "[$(Get-Date -Format 'HH:mm:ss')] OK: no build/test processes"
    } else {
        foreach ($p in $suspects) {
            $runtime = $now - $p.StartTime
            $cpu = [math]::Round($p.CPU, 1)
            $line = "[$(Get-Date -Format 'HH:mm:ss')] pid=$($p.Id) name=$($p.ProcessName) runtime=$([int]$runtime.TotalMinutes)m cpu=${cpu}s"
            Write-Output $line
            if ($runtime -gt $stuckThreshold) {
                Emit-StuckAlert "pid=$($p.Id) name=$($p.ProcessName) runtime=$([int]$runtime.TotalMinutes)m (threshold=${StuckMinutes}m)"
                if ($KillOnStuck) {
                    try {
                        Stop-Process -Id $p.Id -Force -ErrorAction Stop
                        Write-Output "STUCK_ALERT killed pid=$($p.Id)"
                    } catch {
                        Write-Output "STUCK_ALERT failed to kill pid=$($p.Id): $_"
                    }
                }
            }
        }
    }
    Start-Sleep -Seconds $IntervalSeconds
}
