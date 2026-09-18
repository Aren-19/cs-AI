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
    # Must match what the CHECKPOINT was trained with - a policy is not portable
    # across decision rates. The current one measures 73% at 2 and 8.5% at 6.
    # This defaulted to 2 while training ran at 6 once and silently reverted the
    # rate on a restart; nothing looked wrong because eval defaulted the same way.
    [string]$Map    = '',          # blank = whatever data/map.txt says, else surf_demise
    [int]$FrameSkip = 2,           # ticks per decision
    [double]$DevCost = 0.5,        # penalty for drifting off the reference line
    [double]$TimeCost = 0.08,      # charged per decision, so finishing sooner pays
    [double]$EntFinal = 0.0,       # anneal the entropy bonus to this (0 = no anneal)
    [int]$EntAnneal = 2000,        # generations to reach EntFinal
    [int]$PreLearn = 0,            # trailing ticks of the wind-up the policy drives
    [double]$FinishBonus = 50.0,   # base reward for finishing at all
    [double]$FinishFloor = 5.0,    # a finish never scores below this, however slow
    [double]$TrimCost = 0.05,      # per aim change between decisions - smoothness
    [double]$SwitchCost = 0.40,    # per strafe-key change; human does 0.95/s
    [int]$StallSeconds = 240,      # no new generation for this long = say so, loudly
    [double]$Entropy = 0.02,       # exploration pressure; 0.003 let the policy saturate
    [double]$StateMix = 0.3,       # share of episodes starting mid-map
    # As a fraction of the TRACK, which is what the plugin now resolves these
    # against. They used to be compared to a fraction of the reference run's
    # TIME, and the run is much slower at the start than the end, so the two
    # differ by up to 11 points - 0.73 in time is 66% of the track. These
    # defaults are the old ones converted, so the same three checkpoints are
    # selected as before.
    [double]$StateLo = 0.61,
    [double]$StateHi = 0.72,
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
    # Only OUR training actors, identified by the +csai_train_batches they were
    # launched with. This used to kill every srcds on the machine, so stopping
    # the daemon also killed any eval, bench or diagnostic run that happened to
    # be in flight - and the victim then parsed a truncated console.log and
    # reported the fragment as its result.
    foreach ($p in @(Get-Procs 'srcds_win64.exe' '*csai_train_batches*')) {
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
    # Entropy raised from 0.003 after the policy was measured to have collapsed at
    # the 72% drop: 100% of decisions on one side with p(everything else) ~ 0.001.
    # A saturated policy cannot discover the action that works there - forcing
    # phi 92 clears the drop, and it had essentially zero chance of sampling it.
    $args = @((Join-Path $Root 'tools\learn.py'), '--batches', '1000000', '--timeout', '900',
              '--gamma', $Gamma, '--ent', $Entropy, '--frameskip', $FrameSkip,
              '--track', ('"' + (Join-Path $Data "$($Map)_track.txt") + '"'),
              '--states', ('"' + (Join-Path $Data "$($Map)_states.txt") + '"'))
    if ($EntFinal -gt 0.0) {
        $args += @('--ent-final', $EntFinal, '--ent-anneal', $EntAnneal)
    }
    if (Test-Path (Join-Path $Root 'data\ckpt.npz')) { $args += '--resume' }
    # Captured, not discarded. The learner died on every restart for 90 minutes
    # with a KeyError that went to a hidden console, while this line cheerfully
    # logged "learner started (resumed)" each time - Start-Process returns before
    # Python has parsed anything, so the log asserted a success it never checked.
    $learnerLog = Join-Path $Root 'logs\learner.log'
    $learnerErr = Join-Path $Root 'logs\learner.err.log'
    New-Item -ItemType Directory -Force -Path (Split-Path $learnerLog) | Out-Null
    $lp = Start-Process -FilePath 'python' -ArgumentList $args `
        -WorkingDirectory (Join-Path $Root 'tools') -WindowStyle Hidden `
        -RedirectStandardOutput $learnerLog -RedirectStandardError $learnerErr -PassThru

    # The actors run AboveNormal and there are eleven of them on twelve logical
    # cores, so a learner left at Normal loses every scheduling contest on a
    # saturated machine. It was taking 17 seconds per 100k steps while actors
    # finished a batch every 5, which meant more than half of everything they
    # produced was dropped as stale before it could be used. Matching their
    # priority is the whole fix; it costs the actors nothing they were using.
    if ($lp) {
        try { $lp.PriorityClass = 'AboveNormal' } catch {}
    }
    Write-Log "  learner launched$(if ($args -contains '--resume') { ' (resuming)' }) - output in logs\learner.log"
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
        # A distinct RNG stream per actor, and a different one on every restart.
        # Without this every actor shares the default seed 0x1234567, so any two
        # sitting on the same policy generation run byte-identical episodes -
        # measured: all four surviving batch_0000 files identical, and a5/a6/a7
        # identical again at batch_0002. The learner then spends a separate PPO
        # update on each copy, so 11 actors bought far less than 11 actors.
        '+csai_seed', (([int]((Get-Date).Ticks % 100000)) * 32 + $id * 7919 + 1),
        '+servercfgfile', 'server_66.cfg', '+map', $Map,
        '+csai_train_batches', '1000000', '+csai_train_sync', '1',
        '+csai_batch', $cfg.Batch, '+csai_frameskip', $FrameSkip,
        # csai_states 1 = every episode starts at the map start, i.e. one
        # continuous run attempted over and over. 0 would sample all 24 replay
        # checkpoints instead (segmented practice) - easier exploration, but it
        # optimises "advance from anywhere" rather than "complete the map".
        '+csai_states', '1', '+csai_budget', '6000', '+csai_deviation', '600',
        '+csai_statemix', $StateMix, '+csai_statelo', $StateLo, '+csai_statehi', $StateHi,
        '+csai_prestrafe', '1', '+csai_switchcost', $SwitchCost, '+csai_devcost', $DevCost,
        '+csai_timecost', $TimeCost, '+csai_trimcost', $TrimCost,
        '+csai_finishbonus', $FinishBonus, '+csai_finishfloor', $FinishFloor,
        '+csai_prelearn', $PreLearn,
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
                        # 8 runs, not 3: eval cycles the recorded prestrafes and
                        # there are eight of them. Three covered sets 0-2, one of
                        # which hands over 100 units off where the other seven do,
                        # so a third of every reported score came from the least
                        # representative opening on the map.
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

# One daemon at a time. Two would share actor ids, ports and batch filenames,
# and each one's startup Stop-All would kill the other's actors - producing a
# permanent restart fight that looks like healthy activity in the log.
$others = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -like '*-File*daemon.ps1*' -and
                           $_.CommandLine -notlike '*-Stop*' })
if ($others.Count -gt 0) {
    Write-Log "another daemon is already running (pid $($others[0].ProcessId)) - refusing to start a second"
    exit 1
}

Remove-Item $StopFile -ErrorAction SilentlyContinue
if ($Power) { Set-Power $Power }
$level = Get-Power

Write-Log '================ CsAI training daemon ================'
Write-Log "power: $level - $($Levels[$level].Desc)"
Stop-All
Start-Sleep -Seconds 2

# Sweep every batch file with no .done marker. Start-Actor only cleans its own
# actor id, so dropping the power level from max (11 actors) to high (6) left
# a6 through a10's half-written batches with no owner and nothing that would
# ever remove them - and an eval, which runs as actor 99 and never completes a
# batch by design, leaks one every time. Safe here and only here: nothing is
# running yet, so every markerless file is genuinely abandoned.
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

# Evaluation runs alongside the loop instead of blocking it. With -Wait the
# supervisor was blind for up to 10 minutes per eval: no stop-flag check, no
# power change, no learner or actor liveness. A -Stop took that long to land and
# a dead actor stayed dead for the duration.
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

    # A stall watchdog. $lastGen was previously assigned and never read - it
    # looked like this and was not, which is why a total training outage could
    # run for 90 minutes without a single line in this log.
    $gen = Get-Gen
    if ($gen -gt $lastGen) {
        $lastGen = $gen
        $lastGenAt = Get-Date
        $stallWarned = $false
    } elseif (((Get-Date) - $lastGenAt).TotalSeconds -ge $StallSeconds) {
        $stalledFor = [int]((Get-Date) - $lastGenAt).TotalSeconds

        if (-not $stallWarned) {
            Write-Log "STALLED: no new generation for ${stalledFor}s (gen $gen)"

            # Every liveness check above can pass while nothing is being learned.
            # Eleven actors once ran flat out for half an hour writing .bin files
            # whose .done marker the plugin never produced: the learner polls for
            # markers, found none, and this log said nothing but RUNNING. Orphaned
            # .bin files are the fingerprint of that, so name it rather than
            # printing log tails and hoping someone is awake to read them.
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
            # Warning alone is not a watchdog. This used to only write a line,
            # so a total outage ran for half an hour overnight with everything
            # reporting healthy. Restart once the stall is long enough that it
            # cannot be a slow batch, and stop after three tries rather than
            # thrashing against a problem restarting will not fix.
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
