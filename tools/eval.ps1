param(
    [int]$Runs       = 5,
    [double]$Timescale = 20,
    [string]$Map     = 'surf_demise',
    [string]$Slot    = '',      # blank = main
    [int]$Port       = 27600,
    [int]$Budget     = 6000,
    [double]$Deviation = 600,
    [int]$TimeoutSec = 600,
    [int]$Scripted   = 0,
    [int]$Greedy     = 1,
    [int]$Prestrafe  = 1,
    [int]$Windup     = 0,       # >0 = evaluate the wind-up, up to this many ground ticks
    [string]$Partner = '',      # slot whose policy runs alongside
    [int]$Learned    = 1,       # main: open with the learned wind-up when there is one
    [int]$FrameSkip  = 2,       # must match training
    [double]$DevCost = 0.5,
    [double]$SwitchCost = 0.15
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'hidden.ps1')

$Root = Split-Path -Parent $PSScriptRoot
$name = if ($Slot) { $Slot } else { 'main' }

$a = @(
    '+map', $Map,
    '+csai_eval', $Runs,
    '+csai_evalgreedy', $Greedy,
    '+csai_scripted', $Scripted,
    '+csai_prestrafe', $Prestrafe,
    '+csai_actor', '99',
    '+csai_evallearned', $Learned,
    '+csai_windup', $Windup,
    '+csai_budget', $Budget,
    '+csai_deviation', $Deviation,
    '+csai_frameskip', $FrameSkip,
    '+csai_devcost', $DevCost,
    '+csai_switchcost', $SwitchCost,
    '+csai_bench_timescale', $Timescale,
    '+csai_bench_quit', '1',
    '+csai_bench_delay', '8'
)
if ($Slot) { $a += @('+csai_slot', $Slot) }
if ($Partner) { $a += @('+csai_partner', $Partner) }

$mode = if ($Greedy -ne 0) { 'greedy' } else { 'sampled' }
Write-Host "==> eval $Map ($name): $Runs $mode runs, frameskip $FrameSkip" -ForegroundColor Cyan
$log = Invoke-Srcds "csai_${name}_eval" $Port $a $TimeoutSec

$lines = @($log -split "`r?`n" | Where-Object { $_ -match 'eval|replay:|wind-up:' })
$lines | ForEach-Object { Write-Host $_ }

# Kept across runs, for the report.
$hist = Join-Path $Root "logs\eval_$name.log"
New-Item -ItemType Directory -Force -Path (Split-Path $hist) | Out-Null
if ($lines.Count) {
    Add-Content -Path $hist -Value (@("# $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $Map") + $lines) -Encoding utf8
}
