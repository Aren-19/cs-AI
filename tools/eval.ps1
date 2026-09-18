param(
    [int]$Runs       = 5,
    [double]$Timescale = 20,
    [string]$Map     = 'surf_demise',
    [int]$Budget     = 6000,
    [double]$Deviation = 600,
    [int]$TimeoutSec = 600,
    [int]$Scripted   = 0,
    [int]$Greedy     = 1,
    [int]$PreLearn   = 0,
    [int]$Prestrafe  = 1,
    [int]$Windup     = 0,   # >0 = wind-up episodes of this many ticks   # 0 = skip the recorded wind-up, teleport to state 0
    [int]$FrameSkip  = 2,   # must match what the policy was trained with
    [double]$DevCost = 0.5,
    [double]$SwitchCost = 0.15
)

$ErrorActionPreference = 'Stop'

$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source'
$Srcds    = Join-Path $GameRoot 'srcds_win64.exe'
$ConLog   = Join-Path $GameRoot 'cstrike\console.log'

$startLen = 0
if (Test-Path $ConLog) { $startLen = (Get-Item $ConLog).Length }

$a = @(
    '-console', '-game', 'cstrike', '-maxplayers', '6',
    # Actor 0 runs on 27015 (daemon.ps1 assigns 27015 + 10*id), so an eval
    # launched while training is live was racing it for the socket.
    '-port', '27600',
    '+sv_lan', '1', '-insecure', '-condebug',
    '+servercfgfile', 'server_66.cfg',
    '+map', $Map,
    '+csai_eval', $Runs,
    '+csai_evalgreedy', $Greedy,
    '+csai_scripted', $Scripted,
    '+csai_prestrafe', $Prestrafe,
    '+csai_actor', '99',
    '+csai_prelearn', $PreLearn,
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

$mode = if ($Greedy -ne 0) { 'greedy' } else { 'sampled' }
Write-Host "==> eval $Map : $Runs $mode runs, frameskip $FrameSkip" -ForegroundColor Cyan
$proc = Start-Process -FilePath $Srcds -ArgumentList $a -WorkingDirectory $GameRoot -PassThru -WindowStyle Hidden
if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
    Write-Host '    timed out, killing' -ForegroundColor Yellow
    try { $proc.Kill() } catch {}
}

if (Test-Path $ConLog) {
    $fs = [IO.File]::Open($ConLog, 'Open', 'Read', 'ReadWrite')
    try {
        if ($startLen -le $fs.Length) {
            $null = $fs.Seek($startLen, 'Begin')
        } else {
            Write-Host '    note: console.log shrank since launch; output may include earlier runs' -ForegroundColor DarkYellow
        }
        $sr = New-Object IO.StreamReader($fs)
        $sr.ReadToEnd() -split "`r?`n" | Where-Object { $_ -match 'eval|replay:' } | ForEach-Object { Write-Host $_ }
    } finally { $fs.Close() }
}
