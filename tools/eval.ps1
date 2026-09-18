<#
    Greedy evaluation of the current policy.

    Training reports progress *gained* from random checkpoints, which is the right
    learning signal but says nothing about whether the agent can run the map.
    This answers that: argmax actions, always from state 0, reporting how far it
    gets and in what time.

    The number to beat is whatever clean_time the map's states file carries -
    39.045 s for surf_demise. The eval prints it alongside its own best.

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
    # This must match what the policy was TRAINED with, and it is not checked
    # anywhere. When they differed - this said 2 while training ran at 6 - every
    # eval replay drove the policy at three times its own decision rate, and the
    # only symptom was an eval score that made no sense next to training.
    # The daemon passes its own value; if you run this by hand, read the -FrameSkip
    # the daemon was started with (Get-CimInstance Win32_Process) rather than
    # trusting this default.
    [int]$PreLearn   = 0,
    [int]$FrameSkip  = 2,
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
    # Actor 0 runs on 27015 (daemon.ps1 assigns 27015 + 10*id), so an eval
    # launched while training is live was racing it for the socket.
    '-port', '27600',
    '+sv_lan', '1', '-insecure', '-condebug',
    '+servercfgfile', 'server_66.cfg',
    '+map', $Map,
    '+csai_eval', $Runs,
    '+csai_evalgreedy', $Greedy,
    '+csai_scripted', $Scripted,
    '+csai_prestrafe', '1',
    # Batch files are named a<actor>_batch_NNNN.bin. Without this, an eval is
    # actor 0 - the same name the first training actor writes under - and any
    # batch it flushed would land on top of a live actor's file.
    '+csai_actor', '99',
    '+csai_prelearn', $PreLearn,
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
        # If the log SHRANK (rotated, replaced), startLen is past the end and
        # seeking is wrong - but so is reading from 0, which reports the whole
        # history including previous runs as this run's result. Start at 0 only
        # when the file is genuinely shorter, and say so.
        if ($startLen -le $fs.Length) {
            $null = $fs.Seek($startLen, 'Begin')
        } else {
            Write-Host '    note: console.log shrank since launch; output may include earlier runs' -ForegroundColor DarkYellow
        }
        $sr = New-Object IO.StreamReader($fs)
        $sr.ReadToEnd() -split "`r?`n" | Where-Object { $_ -match 'eval|replay:' } | ForEach-Object { Write-Host $_ }
    } finally { $fs.Close() }
}
