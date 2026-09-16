<#
    Unattended training supervisor.

    Keeps the learner and the actor alive, applies a power level you can change
    at any time (including while it runs), evaluates the policy periodically so
    there is always a fresh replay to watch, and writes reports you can read when
    you come back.

    Normally launched from the control panel, but usable directly:

        .\tools\daemon.ps1                 # run until stopped
        .\tools\daemon.ps1 -Power low
        .\tools\daemon.ps1 -Stop

    Power is read from data/power.txt on every cycle. Writing a new level into
    that file (which the panel does) makes the daemon restart the actor with the
    new settings within ~15 s. Training is not lost: the learner checkpoints every
    generation and resumes from it.
#>
param(
    [ValidateSet('idle', 'low', 'medium', 'high', 'max')]
    [string]$Power = '',
    [int]$Actors    = 0,           # parallel srcds actors; 0 = auto from core count
    [int]$FrameSkip = 2,           # ticks per decision
    [double]$DevCost = 0.5,        # penalty for drifting off the reference line
    [double]$StateMix = 0.3,       # share of episodes starting mid-map
    [double]$StateLo = 0.68,       # window for those, as a fraction of the reference run's time
    [double]$StateHi = 0.79,
    [double]$Gamma  = 0.997,       # discount; horizon is FrameSkip/(1-Gamma) ticks
    [int]$EvalEvery = 40,          # generations between automatic evaluations
    [int]$ReportEvery = 300,       # seconds between report snapshots
    [switch]$Stop
)

$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $PSScriptRoot
$PowerFile = Join-Path $Root 'data\power.txt'
$LogFile = Join-Path $Root 'logs\daemon.log'
$TrainLog = Join-Path $Root 'data\train_log.csv'
$StopFile = Join-Path $Root 'data\daemon.stop'

# timescale 80 is the measured knee of the throughput curve (5136 ticks/s);
# 150 buys only ~6% more. The lower levels exist to give the machine back.
# How much of the machine to use. The lever is the NUMBER of servers, not the
# timescale: one server is single-threaded and, once several are running, each
# manages only 23-32x realtime - far below even the 80 it is told to target. So
# raising timescale past that does nothing at all, which is why the old 'max'
# (150x, same single server) was no faster than 'high'.
#
# Measured on a 6-core / 12-thread Ryzen 7500F, total simulated ticks/s:
#     6 actors 15276   10 actors 21586   12 actors 22915   14 actors 22066
# It keeps climbing past the physical core count because the server loop stalls
# on memory often enough for SMT to do real work. 12 is the knee; 14 is worse.
# The learner needs about half a core, so the top level leaves it room.
$Levels = @{
    'idle'   = @{ Timescale = 5;   Priority = 'Idle';        Batch = 32; Actors = 1;  Desc = 'barely noticeable - gaming or video' }
    'low'    = @{ Timescale = 20;  Priority = 'BelowNormal'; Batch = 48; Actors = 2;  Desc = 'you are using the PC' }
    'medium' = @{ Timescale = 80;  Priority = 'Normal';      Batch = 64; Actors = 4;  Desc = 'background work' }
    'high'   = @{ Timescale = 80;  Priority = 'Normal';      Batch = 64; Actors = 6;  Desc = 'default - half the machine' }
    'max'    = @{ Timescale = 80;  Priority = 'AboveNormal'; Batch = 96; Actors = 11; Desc = 'you are away - all of it' }
}

function Write-Log([string]$msg) {
    $line = "{0}  {1}" -f (Get-Date -Format 'HH:mm:ss'), $msg
    Write-Host $line
    try {
        New-Item -ItemType Directory -Force -Path (Split-Path $LogFile) | Out-Null
        Add-Content -Path $LogFile -Value $line -Encoding utf8
    } catch {}
}

function Get-Power {
    if (Test-Path $PowerFile) {
        $v = (Get-Content $PowerFile -Raw -ErrorAction SilentlyContinue).Trim().ToLower()
        if ($Levels.ContainsKey($v)) { return $v }
    }
    return 'high'
}

function Set-Power([string]$level) {
    New-Item -ItemType Directory -Force -Path (Split-Path $PowerFile) | Out-Null
    Set-Content -Path $PowerFile -Value $level -Encoding utf8
}

function Get-Procs([string]$name, [string]$match) {
    Get-CimInstance Win32_Process -Filter "Name='$name'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like $match }
}

function Stop-All {
    foreach ($p in @(Get-Procs 'python.exe' '*learn.py*')) {
        Write-Log "  stop learner pid $($p.ProcessId)"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
    foreach ($p in @(Get-Process -Name 'srcds_win64' -ErrorAction SilentlyContinue)) {
        Write-Log "  stop actor pid $($p.Id)"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
}

function Get-Gen {
    if (-not (Test-Path $TrainLog)) { return 0 }
    try {
        $last = Get-Content $TrainLog -Tail 1 -ErrorAction SilentlyContinue
        if ($last -and $last -notmatch '^gen,') { return [int]($last -split ',')[0] }
    } catch {}
    return 0
}

function Start-Learner {
    $args = @((Join-Path $Root 'tools\learn.py'), '--batches', '1000000', '--timeout', '900',
              '--gamma', $Gamma)
    if (Test-Path (Join-Path $Root 'data\ckpt.npz')) { $args += '--resume' }
    Start-Process -FilePath 'python' -ArgumentList $args `
        -WorkingDirectory (Join-Path $Root 'tools') -WindowStyle Hidden | Out-Null
    Write-Log "  learner started$(if ($args -contains '--resume') { ' (resumed)' })"
}

function Start-Actor([string]$level, [int]$id = 0) {
    $cfg = $Levels[$level]

    # A killed actor leaves a half-written .bin with no .done marker. It restarts
    # its batch counter at 0, so those orphans would otherwise pile up and shadow
    # real batches. Completed ones (.done present) are left for the learner.
    $outDir = 'C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source\cstrike\addons\sourcemod\data\csai\out'
    if (Test-Path $outDir) {
        Get-ChildItem $outDir -Filter "a${id}_batch_*.bin" -ErrorAction SilentlyContinue | ForEach-Object {
            $done = [IO.Path]::ChangeExtension($_.FullName, '.done')
            if (-not (Test-Path $done)) { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
        }
    }
    $game = 'C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source'
    $a = @(
        '-console', '-game', 'cstrike', '-maxplayers', '6',
        '+sv_lan', '1', '-insecure',
        '-port', (27015 + 10 * $id),
        '+csai_actor', $id,
        '+servercfgfile', 'server_66.cfg', '+map', 'surf_demise',
        '+csai_train_batches', '1000000', '+csai_train_sync', '1',
        '+csai_batch', $cfg.Batch, '+csai_frameskip', $FrameSkip,
        # csai_states 1 = every episode starts at the map start, i.e. one
        # continuous run attempted over and over. 0 would sample all 24 replay
        # checkpoints instead (segmented practice) - easier exploration, but it
        # optimises "advance from anywhere" rather than "complete the map".
        '+csai_states', '1', '+csai_budget', '6000', '+csai_deviation', '600',
        '+csai_statemix', $StateMix, '+csai_statelo', $StateLo, '+csai_statehi', $StateHi,
        '+csai_prestrafe', '1', '+csai_switchcost', '0.15', '+csai_devcost', $DevCost,
        '+csai_bench_timescale', $cfg.Timescale,
        '+csai_bench_quit', '0', '+csai_bench_delay', '8'
    )
    # Only actor 0 keeps -condebug, so parallel instances do not contend on one
    # console.log and eval's log parsing still sees a quiet file.
    if ($id -eq 0) { $a += '-condebug' }
    $p = Start-Process -FilePath (Join-Path $game 'srcds_win64.exe') -ArgumentList $a `
        -WorkingDirectory $game -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 2
    try { $p.PriorityClass = $cfg.Priority } catch {}
    Write-Log "  actor $id started pid $($p.Id) - timescale $($cfg.Timescale), priority $($cfg.Priority), batch $($cfg.Batch)"
    return $p.Id
}

function Invoke-Report([switch]$Snapshot) {
    $a = @((Join-Path $Root 'tools\report.py'))
    if ($Snapshot) { $a += '--snapshot' }
    Start-Process -FilePath 'python' -ArgumentList $a -WorkingDirectory $Root `
        -WindowStyle Hidden -Wait -ErrorAction SilentlyContinue | Out-Null
}

function Invoke-Eval {
    Write-Log '  evaluating policy (produces a replay to watch)'
    # -Greedy 0 samples the policy instead of taking its argmax. The argmax is a
    # different controller: at gen 143 it scored 6.0% where the sampled policy
    # scored 32-63% from the same start, because a discrete action space reaches
    # an intermediate steering angle by mixing adjacent trims.
    Start-Process -FilePath 'powershell' `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                        (Join-Path $Root 'tools\eval.ps1'), '-Runs', '3',
                        '-Greedy', '0',
                        '-FrameSkip', $FrameSkip, '-DevCost', $DevCost,
                        '-Timescale', '20', '-TimeoutSec', '600') `
        -WorkingDirectory $Root -WindowStyle Hidden -Wait -ErrorAction SilentlyContinue | Out-Null
}

# ---------------------------------------------------------------------------

if ($Stop) {
    Write-Log 'stopping training'
    New-Item -ItemType File -Force -Path $StopFile | Out-Null
    Stop-All
    Write-Log 'stopped'
    exit 0
}

Remove-Item $StopFile -ErrorAction SilentlyContinue
if ($Power) { Set-Power $Power }
$level = Get-Power

Write-Log '================ CsAI training daemon ================'
Write-Log "power: $level - $($Levels[$level].Desc)"
Stop-All
Start-Sleep -Seconds 2
# One srcds saturates a core at ~5400 ticks/s, so throughput past that comes
# from more instances. Leave headroom for the learner, the eval run and the
# machine itself rather than claiming every core.
# -Actors overrides the power level's own count; 0 means follow the level.
$ActorsOverride = $Actors
if ($Actors -le 0) { $Actors = $Levels[$level].Actors }

Start-Learner
Start-Sleep -Seconds 3
$ActorPids = @{}
for ($i = 0; $i -lt $Actors; $i++) {
    $ActorPids[$i] = Start-Actor $level $i
    Start-Sleep -Seconds 2
}
Write-Log "  $Actors actor(s) running on $([Environment]::ProcessorCount) logical cores"

$lastGen = Get-Gen
$lastEvalGen = $lastGen
$lastReport = Get-Date
$appliedLevel = $level

while ($true) {
    Start-Sleep -Seconds 15

    if (Test-Path $StopFile) { Write-Log 'stop requested'; Stop-All; break }

    # power change -> restart the actor with new settings. The learner keeps its
    # checkpoint, so nothing trained is lost.
    $want = Get-Power
    if ($want -ne $appliedLevel) {
        Write-Log "power changed: $appliedLevel -> $want ($($Levels[$want].Desc))"
        $newCount = if ($ActorsOverride -gt 0) { $ActorsOverride } else { $Levels[$want].Actors }
        if ($newCount -ne $Actors) {
            Write-Log "  actors: $Actors -> $newCount"
            foreach ($k in @($ActorPids.Keys)) {
                if ($k -ge $newCount) {
                    Stop-Process -Id $ActorPids[$k] -Force -ErrorAction SilentlyContinue
                    $ActorPids.Remove($k)
                }
            }
            $Actors = $newCount
        }
        foreach ($apid in @($ActorPids.Values)) {
            Stop-Process -Id $apid -Force -ErrorAction SilentlyContinue
        }
        Start-Sleep -Seconds 3
        for ($i = 0; $i -lt $Actors; $i++) {
            $ActorPids[$i] = Start-Actor $want $i
            Start-Sleep -Seconds 2
        }
        $appliedLevel = $want
    }

    # keep both halves alive
    if (-not (Get-Procs 'python.exe' '*learn.py*')) {
        Write-Log 'learner died - restarting'
        Start-Learner
        Start-Sleep -Seconds 3
    }
    # Checked per tracked pid. "is any srcds running" would be satisfied by the
    # eval instance, and would hide a dead actor for as long as an eval lasts.
    for ($i = 0; $i -lt $Actors; $i++) {
        $alive = $false
        if ($ActorPids.ContainsKey($i)) {
            $alive = [bool](Get-Process -Id $ActorPids[$i] -ErrorAction SilentlyContinue)
        }
        if (-not $alive) {
            Write-Log "actor $i died - restarting"
            $ActorPids[$i] = Start-Actor $appliedLevel $i
        }
    }

    $gen = Get-Gen
    if ($gen -gt $lastGen) {
        $lastGen = $gen
    }

    if (((Get-Date) - $lastReport).TotalSeconds -ge $ReportEvery) {
        Invoke-Report -Snapshot
        Write-Log "report written - gen $gen"
        $lastReport = Get-Date
    }

    if ($EvalEvery -gt 0 -and ($gen - $lastEvalGen) -ge $EvalEvery) {
        Invoke-Eval
        Invoke-Report
        $lastEvalGen = $gen
    }
}

Invoke-Report -Snapshot
Write-Log 'daemon exited'
