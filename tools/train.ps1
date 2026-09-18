param(
    [int]$Batches    = 2,
    [int]$Sync       = 0,
    [double]$Timescale = 80,
    [int]$BatchSize  = 32,
    [int]$FrameSkip  = 2,
    [int]$States     = 1,       # 1 = always the map start (continuous run); 0 = all checkpoints
    [int]$Prestrafe  = 1,
    [double]$SwitchCost = 0.15,
    [int]$Budget     = 4000,
    [double]$Deviation = 600,
    [int]$Seed       = 0,
    [int]$ObsDump    = 0,
    [string]$Map     = 'surf_demise',
    [int]$TimeoutSec = 900,
    [switch]$Wait               # block until the server exits
)

$ErrorActionPreference = 'Stop'

$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source'
$Cstrike  = Join-Path $GameRoot 'cstrike'
$Srcds    = Join-Path $GameRoot 'srcds_win64.exe'
$ConLog   = Join-Path $Cstrike  'console.log'

Remove-Item $ConLog -ErrorAction SilentlyContinue

$a = @(
    '-console', '-game', 'cstrike', '-maxplayers', '6',
    '+sv_lan', '1', '-insecure', '-condebug',
    '+servercfgfile', 'server_66.cfg',
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
    '+csai_bench_timescale', $Timescale,
    '+csai_bench_quit', '1',
    '+csai_bench_delay', '8'
)

Write-Host "==> $Map  batches=$Batches sync=$Sync timescale=$Timescale batch=$BatchSize" -ForegroundColor Cyan
$proc = Start-Process -FilePath $Srcds -ArgumentList $a -WorkingDirectory $GameRoot -PassThru -WindowStyle Hidden
Write-Host "    srcds pid $($proc.Id)"

if ($Wait) {
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        Write-Host '    timed out, killing' -ForegroundColor Yellow
        try { $proc.Kill() } catch {}
    }
    Select-String -Path $ConLog -Pattern 'CsAI' | ForEach-Object { $_.Line }
}
