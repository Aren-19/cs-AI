<#
    Greedy evaluation of the current policy.

    Training reports progress *gained* from random checkpoints, which is the right
    learning signal but says nothing about whether the agent can run the map.
    This answers that: argmax actions, always from state 0, reporting how far it
    gets and in what time.

    The number to beat on surf_demise is 39.10 s (human, 66 tick, no deaths).

    Usage:
        .\tools\eval.ps1 -Runs 5
#>
param(
    [int]$Runs       = 5,
    [double]$Timescale = 20,
    [string]$Map     = 'surf_demise',
    [int]$Budget     = 6000,
    [double]$Deviation = 600,
    [int]$TimeoutSec = 600,
    [int]$Scripted   = 0,
    [int]$Greedy     = 1,
    # These must match what the policy was TRAINED with. They did not: this
    # hardcoded frameskip 2 while training ran at 6, so every eval replay from
    # generation 254 on drove the policy at three times its decision rate.
    [int]$FrameSkip  = 6,
    [double]$DevCost = 0.5,
    [double]$SwitchCost = 0.15
)

$ErrorActionPreference = 'Stop'

$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source'
$Srcds    = Join-Path $GameRoot 'srcds_win64.exe'
$ConLog   = Join-Path $GameRoot 'cstrike\console.log'

# The daemon's training actor writes the same console.log, and on Windows it
# holds the file open - deleting it silently fails and leaves stale content that
# reads as this run's output. Note the length instead and report only new lines.
$startLen = 0
if (Test-Path $ConLog) { $startLen = (Get-Item $ConLog).Length }

$a = @(
    '-console', '-game', 'cstrike', '-maxplayers', '6',
    '+sv_lan', '1', '-insecure', '-condebug',
    '+servercfgfile', 'server_66.cfg',
    '+map', $Map,
    '+csai_eval', $Runs,
    '+csai_evalgreedy', $Greedy,
    '+csai_scripted', $Scripted,
    '+csai_prestrafe', '1',
    '+csai_budget', $Budget,
    '+csai_deviation', $Deviation,
    '+csai_frameskip', $FrameSkip,
    '+csai_devcost', $DevCost,
    '+csai_switchcost', $SwitchCost,
    '+csai_bench_timescale', $Timescale,
    '+csai_bench_quit', '1',
    '+csai_bench_delay', '8'
)

Write-Host "==> eval $Map : $Runs greedy runs (human reference 39.10 s)" -ForegroundColor Cyan
$proc = Start-Process -FilePath $Srcds -ArgumentList $a -WorkingDirectory $GameRoot -PassThru -WindowStyle Hidden
if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
    Write-Host '    timed out, killing' -ForegroundColor Yellow
    try { $proc.Kill() } catch {}
}

if (Test-Path $ConLog) {
    $fs = [IO.File]::Open($ConLog, 'Open', 'Read', 'ReadWrite')
    try {
        if ($startLen -le $fs.Length) { $null = $fs.Seek($startLen, 'Begin') }
        $sr = New-Object IO.StreamReader($fs)
        $sr.ReadToEnd() -split "`r?`n" | Where-Object { $_ -match 'eval|replay:' } | ForEach-Object { Write-Host $_ }
    } finally { $fs.Close() }
}
