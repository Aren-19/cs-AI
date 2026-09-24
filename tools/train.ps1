param(
    [int]$Batches    = 2,
    [int]$Sync       = 0,
    [double]$Timescale = 80,
    [int]$BatchSize  = 32,
    [int]$FrameSkip  = 2,
    [int]$States     = 1,       # 1 = always the map start; 0 = all checkpoints
    [int]$Prestrafe  = 1,
    [double]$SwitchCost = 0.15,
    [int]$Budget     = 4000,
    [double]$Deviation = 600,
    [int]$Seed       = 0,
    [int]$ObsDump    = 0,
    [double]$LineJitter = 0,    # training: shift the line the policy sees by up to this many units
    [string]$Map     = 'surf_demise',
    [int]$Port       = 26900,
    [int]$TimeoutSec = 900,
    [switch]$Wait               # block until the server exits
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'hidden.ps1')

$a = @(
    '+map', $Map,
    '+csai_train_batches', $Batches,
    '+csai_train_sync', $Sync,
    '+csai_batch', $BatchSize,
    '+csai_frameskip', $FrameSkip,
    '+csai_states', $States,
    '+csai_prestrafe', $Prestrafe,
    '+csai_switchcost', $SwitchCost,
    '+csai_budget', $Budget,
    '+csai_deviation', $Deviation,
    '+csai_seed', $Seed,
    '+csai_obsdump', $ObsDump,
    '+csai_linejitter', $LineJitter,
    '+csai_bench_timescale', $Timescale,
    '+csai_bench_quit', '1',
    '+csai_bench_delay', '8'
)

Write-Host "==> $Map  batches=$Batches sync=$Sync timescale=$Timescale batch=$BatchSize" -ForegroundColor Cyan
if ($Wait) {
    $log = Invoke-Srcds 'csai_train' $Port $a $TimeoutSec
    $log -split "`r?`n" | Where-Object { $_ -match 'CsAI' }
} else {
    $proc = Start-Srcds 'csai_train' $Port $a
    Write-Host "    srcds pid $($proc.Id), output in cstrike\logs\csai_train.log"
}
