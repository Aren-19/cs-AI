param(
    [ValidateSet('idle', 'low', 'medium', 'high', 'max')]
    [string]$Power = '',
    [int]$Actors    = 0,           # parallel srcds actors; 0 = auto from core count
    [string]$Map    = '',          # blank = whatever data/map.txt says, else surf_demise
    [int]$FrameSkip = 2,           # ticks per decision
    [double]$DevCost = 0.5,        # penalty for drifting off the reference line
    [double]$TimeCost = 0.08,      # charged per decision, so finishing sooner pays
    [double]$EntFinal = 0.0,       # anneal the entropy bonus to this (0 = no anneal)
    [int]$EntAnneal = 2000,        # generations to reach EntFinal
    [int]$PreLearn = 0,            # trailing ticks of the wind-up the policy drives
    [string]$Slot = '',            # name a slot to train a second thing alongside the first
    [int]$Windup = 0,              # >0 = wind-up episodes of this many ticks
    [double]$FinishBonus = 50.0,   # base reward for finishing at all
    [double]$FinishFloor = 5.0,    # a finish never scores below this, however slow
    [double]$TrimCost = 0.05,      # per aim change between decisions - smoothness
    [double]$SwitchCost = 0.40,    # per strafe-key change; human does 0.95/s
    [int]$StallSeconds = 240,      # no new generation for this long = say so, loudly
    [double]$Entropy = 0.02,       # exploration pressure; 0.003 let the policy saturate
    [double]$StateMix = 0.3,       # share of episodes starting mid-map
    [double]$StateLo = 0.61,
    [double]$StateHi = 0.72,
    [double]$Gamma  = 0.997,       # discount; horizon is FrameSkip/(1-Gamma) ticks
    [int]$EvalEvery = 40,          # generations between automatic evaluations
    [int]$ReportEvery = 300,       # seconds between report snapshots
    [switch]$Stop
)

$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $PSScriptRoot
$PowerFile = Join-Path (Join-Path $Root 'data') $(if ($Slot) { "power_$Slot.txt" } else { 'power.txt' })
$LogFile = Join-Path $Root 'logs\daemon.log'
$TrainLog = Join-Path $Root 'data\train_log.csv'
$StopFile = Join-Path $Root $(if ($Slot) { "data\daemon_$Slot.stop" } else { 'data\daemon.stop' })
$MapFile  = Join-Path $Root 'data\map.txt'
$Game     = 'C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source'
$Data     = Join-Path $Game 'cstrike\addons\sourcemod\data\csai'

# One place records which map is being trained, so the reports and the panel stop
# naming a map they were written against and start naming the one that is running.
if (-not $Map) {
    if (Test-Path $MapFile) { $Map = (Get-Content $MapFile -Raw).Trim() }
    if (-not $Map) { $Map = 'surf_demise' }
}
New-Item -ItemType Directory -Force -Path (Split-Path $MapFile) | Out-Null
# ASCII, not utf8: Set-Content -Encoding utf8 prepends a BOM that Python reads
# as part of the map name.
Set-Content -Path $MapFile -Value $Map -Encoding ascii

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
        $v = (Get-Content $PowerFile -Raw -ErrorAction SilentlyContinue).Trim([char]0xFEFF + " `t`r`n").ToLower()
        if ($Levels.ContainsKey($v)) { return $v }
    }
    return 'high'
}

function Set-Power([string]$level) {
    New-Item -ItemType Directory -Force -Path (Split-Path $PowerFile) | Out-Null
    # ASCII for the same reason as the marker file above: -Encoding utf8
    # prepends a byte-order mark, and a level of "﻿high" matches nothing.
    Set-Content -Path $PowerFile -Value $level -Encoding ascii
}

function Get-Procs([string]$name, [string]$match) {
    Get-CimInstance Win32_Process -Filter "Name='$name'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like $match }
}

function Stop-All {
    # Only this slot's processes. Without the filter, stopping one slot killed
    # every other slot's actors and learner too, because they all match
    # "learn.py" and "csai_train_batches".
    foreach ($p in @(Get-Procs 'python.exe' '*learn.py*')) {
        $mine = if ($Slot) { $p.CommandLine -like "*out_$Slot*" }
                else       { $p.CommandLine -notlike '*--outdir*out_*' }
        if (-not $mine) { continue }
        Write-Log "  stop learner pid $($p.ProcessId)"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
    foreach ($p in @(Get-Procs 'srcds_win64.exe' '*csai_train_batches*')) {
        $mine = if ($Slot) { $p.CommandLine -like "*+csai_slot $Slot*" }
                else       { $p.CommandLine -notlike '*+csai_slot *' }
        if (-not $mine) { continue }
        Write-Log "  stop actor pid $($p.ProcessId)"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
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
              '--gamma', $Gamma, '--ent', $Entropy, '--frameskip', $FrameSkip,
              '--track', ('"' + (Join-Path $Data "$($Map)_track.txt") + '"'),
              '--states', ('"' + (Join-Path $Data "$($Map)_states.txt") + '"'))
    if ($EntFinal -gt 0.0) {
        $args += @('--ent-final', $EntFinal, '--ent-anneal', $EntAnneal)
    }
    if ($Slot) {
        $args += @('--weights', ('"' + (Join-Path $Data "weights_$Slot.txt") + '"'),
                   '--outdir',  ('"' + (Join-Path $Data "out_$Slot") + '"'),
                   '--ckpt',    ('"' + (Join-Path (Join-Path $Root "data") "ckpt_$Slot.npz") + '"'),
                   '--log',     ('"' + (Join-Path (Join-Path $Root "data") "train_log_$Slot.csv") + '"'),
                   '--kl-ref',  '0')
        if (Test-Path (Join-Path (Join-Path $Root "data") "ckpt_$Slot.npz")) { $args += '--resume' }
    }
    elseif (Test-Path (Join-Path $Root 'data\ckpt.npz')) { $args += '--resume' }
    # Per slot, or two learners write over each other's output and a stale error
    # from one looks like a live error from the other.
    $logDir = Join-Path $Root 'logs'
    $tag = if ($Slot) { "learner_$Slot" } else { 'learner' }
    $learnerLog = Join-Path $logDir "$tag.log"
    $learnerErr = Join-Path $logDir "$tag.err.log"
    New-Item -ItemType Directory -Force -Path (Split-Path $learnerLog) | Out-Null
    $lp = Start-Process -FilePath 'python' -ArgumentList $args `
        -WorkingDirectory (Join-Path $Root 'tools') -WindowStyle Hidden `
        -RedirectStandardOutput $learnerLog -RedirectStandardError $learnerErr -PassThru

    if ($lp) {
        try { $lp.PriorityClass = 'AboveNormal' } catch {}
    }
    Write-Log "  learner launched$(if ($args -contains '--resume') { ' (resuming)' }) - output in logs\learner.log"
}

function Start-Actor([string]$level, [int]$id = 0) {
    $cfg = $Levels[$level]

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
        '+csai_seed', (([int]((Get-Date).Ticks % 100000)) * 32 + $id * 7919 + 1),
        '+servercfgfile', 'server_66.cfg', '+map', $Map,
        '+csai_train_batches', '1000000', '+csai_train_sync', '1',
        '+csai_batch', $cfg.Batch, '+csai_frameskip', $FrameSkip,
        '+csai_states', '1', '+csai_budget', '6000', '+csai_deviation', '600',
        '+csai_statemix', $StateMix, '+csai_statelo', $StateLo, '+csai_statehi', $StateHi,
        '+csai_prestrafe', '1', '+csai_switchcost', $SwitchCost, '+csai_devcost', $DevCost,
        '+csai_timecost', $TimeCost, '+csai_trimcost', $TrimCost,
        '+csai_finishbonus', $FinishBonus, '+csai_finishfloor', $FinishFloor,
        '+csai_prelearn', $PreLearn, '+csai_windup', $Windup,
        '+csai_bench_timescale', $cfg.Timescale,
        '+csai_bench_quit', '0', '+csai_bench_delay', '8'
    )
    # Only actor 0 keeps -condebug, so parallel instances do not contend on one
    # console.log and eval's log parsing still sees a quiet file.
    if ($id -eq 0) { $a += '-condebug' }
    # Appended, not inlined: an inline conditional inside the array literal puts
    # a null element into the argument list and the process fails to start.
    if ($Slot) { $a += @('+csai_slot', $Slot) }
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
    Start-Process -FilePath 'powershell' `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                        (Join-Path $Root 'tools\eval.ps1'), '-Runs', '8', '-Map', $Map,
                        '-Greedy', '0',
                        '-FrameSkip', $FrameSkip, '-DevCost', $DevCost,
                        '-Timescale', '20', '-TimeoutSec', '600') `
        -WorkingDirectory $Root -WindowStyle Hidden -PassThru -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------------------

if ($Stop) {
    Write-Log 'stopping training'
    New-Item -ItemType File -Force -Path $StopFile | Out-Null
    Stop-All
    Write-Log 'stopped'
    exit 0
}

# One daemon per slot. Two daemons on the same slot would fight over the same
# weights file and batch directory; on different slots they share nothing.
$slotTag = if ($Slot) { "-Slot $Slot" } else { '' }
$others = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -like '*-File*daemon.ps1*' -and
                           $_.CommandLine -notlike '*-Stop*' } |
            Where-Object {
                $theirs = if ($_.CommandLine -match '-Slot\s+(\S+)') { $Matches[1] } else { '' }
                $theirs -eq $Slot
            })
if ($others.Count -gt 0) {
    Write-Log "another daemon is already running on this slot (pid $($others[0].ProcessId)) - refusing to start a second"
    exit 1
}

Remove-Item $StopFile -ErrorAction SilentlyContinue
if ($Power) { Set-Power $Power }
$level = Get-Power

Write-Log '================ CsAI training daemon ================'
Write-Log "power: $level - $($Levels[$level].Desc)"
Stop-All
Start-Sleep -Seconds 2

$outDir = 'C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source\cstrikeddons\sourcemod\data\csai\out'
if (Test-Path $outDir) {
    $swept = 0
    Get-ChildItem $outDir -Filter 'a*_batch_*.bin' -ErrorAction SilentlyContinue | ForEach-Object {
        if (-not (Test-Path ([IO.Path]::ChangeExtension($_.FullName, '.done')))) {
            Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue
            $swept++
        }
    }
    if ($swept -gt 0) { Write-Log "  swept $swept abandoned batch file(s)" }
}
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

$evalProc = $null
$evalStartedAt = $null

$lastGen = Get-Gen
$lastGenAt = Get-Date
$stallWarned = $false
$stallRestarts = 0
$lastEvalGen = $lastGen
$lastReport = Get-Date
$appliedLevel = $level

while ($true) {
    Start-Sleep -Seconds 15

    if (Test-Path $StopFile) { Write-Log 'stop requested'; Stop-All; break }

    if ($evalProc -and $evalProc.HasExited) {
        Write-Log ("  eval finished in {0:N0}s" -f ((Get-Date) - $evalStartedAt).TotalSeconds)
        Invoke-Report
        $evalProc = $null
    }

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
        $lastGenAt = Get-Date
        $stallWarned = $false
    } elseif (((Get-Date) - $lastGenAt).TotalSeconds -ge $StallSeconds) {
        $stalledFor = [int]((Get-Date) - $lastGenAt).TotalSeconds

        if (-not $stallWarned) {
            Write-Log "STALLED: no new generation for ${stalledFor}s (gen $gen)"

            $outDir = 'C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source\cstrikeddons\sourcemod\data\csai\out'
            if (Test-Path $outDir) {
                # a99 is an eval, whose batch never completes by design.
                $orphans = @(Get-ChildItem $outDir -Filter 'a*_batch_*.bin' -ErrorAction SilentlyContinue |
                             Where-Object { $_.Name -notlike 'a99_*' -and
                                            -not (Test-Path ($_.FullName -replace '\.bin$', '.done')) -and
                                            $_.LastWriteTime -lt (Get-Date).AddMinutes(-5) })
                if ($orphans.Count -gt 2) {
                    Write-Log "  $($orphans.Count) batch files have waited over five minutes for a .done"
                    Write-Log '  marker. The actors are producing data the learner cannot see - look at'
                    Write-Log '  Ep_OpenBatch and Train_WriteDoneMarker before anything else.'
                }
            }
            Write-Log '  last lines from the learner:'
            foreach ($f in @((Join-Path $Root 'logs\learner.err.log'), (Join-Path $Root 'logs\learner.log'))) {
                if (Test-Path $f) {
                    Get-Content $f -Tail 6 -ErrorAction SilentlyContinue |
                        ForEach-Object { if ($_.Trim()) { Write-Log "    $_" } }
                }
            }
            $stallWarned = $true
        }
        elseif ($stalledFor -ge (3 * $StallSeconds) -and $stallRestarts -lt 3) {
            $stallRestarts++
            Write-Log "  still stalled after ${stalledFor}s - restarting everything (attempt $stallRestarts of 3)"
            Stop-All
            Start-Sleep -Seconds 5
            Start-Learner
            Start-Sleep -Seconds 3
            for ($i = 0; $i -lt $Actors; $i++) {
                $ActorPids[$i] = Start-Actor $appliedLevel $i
                Start-Sleep -Seconds 2
            }
            $lastGenAt = Get-Date
            $stallWarned = $false
        }
    }

    if (((Get-Date) - $lastReport).TotalSeconds -ge $ReportEvery) {
        Invoke-Report -Snapshot
        Write-Log "report attempted - gen $gen"
        $lastReport = Get-Date
    }

    if ($EvalEvery -gt 0 -and ($gen - $lastEvalGen) -ge $EvalEvery) {
        if ($evalProc -and -not $evalProc.HasExited) {
            Write-Log '  eval still running, skipping this one'
        } else {
            $evalProc = Invoke-Eval
            $evalStartedAt = Get-Date
        }
        Invoke-Report
        $lastEvalGen = $gen
    }
}

Invoke-Report -Snapshot
Write-Log 'daemon exited'
