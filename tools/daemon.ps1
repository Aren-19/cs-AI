param(
    [ValidateSet('idle', 'low', 'medium', 'high', 'max')]
    [string]$Power = '',
    [int]$Actors    = 0,           # parallel servers; 0 = from the power level
    [string]$Map    = '',          # one map, or several separated by commas; blank = data/map.txt
    [string]$Slot   = '',          # blank = main; a name trains a second run alongside
    [int]$FrameSkip = 2,           # ticks per decision
    [double]$DevCost = 0.5,        # penalty for drifting off the reference line
    [double]$TimeCost = 0.08,      # charged per decision
    [double]$FinishBonus = 50.0,   # what a finish at the reference time pays
    [double]$FinishFloor = 0.5,    # a finish never pays less than this
    [double]$TrimCost = 0.05,      # per aim change
    [double]$SwitchCost = 0.40,    # per strafe key change
    [double]$Entropy = 0.01,       # exploration bonus
    [double]$EntFinal = 0.0,       # anneal the bonus to this (0 = no anneal)
    [int]$EntAnneal = 2000,        # generations to reach EntFinal
    [double]$StateMix = 0.3,       # share of episodes starting mid-map
    [double]$FocusMix = 0.0,       # share starting just before where recent runs failed
    [double]$StateLo = 0.61,
    [double]$StateHi = 0.72,
    [double]$Gamma  = 0.997,
    [int]$Windup = 0,              # >0 = this slot trains the wind-up, up to this many ground ticks
    [string]$Partner = '',         # slot whose policy runs alongside: the wind-up for main, main for a wind-up
    [double]$LearnedMix = 0.0,     # main: share of runs from the start opened by the learned wind-up
    [double]$LineJitter = 0.0,     # shift the line the policy sees by up to this many units, per episode
    [double]$KlRef = 0.0,          # pull towards the anchor checkpoint; 0 = off
    [double]$MouseAcc = 1.5,       # run mouse acceleration, deg/tick^2; 0 = view set directly
    [double]$MouseMax = 7.0,       # air mouse speed, deg/tick
    [int]$MinPress = 12,           # ticks a strafe key stays down
    [int]$MinCoast = 6,            # ticks with no key before the next press
    [int]$StallSeconds = 240,      # no new generation for this long counts as a stall
    [int]$SilentSeconds = 600,     # a server quiet this long while others work is restarted
    [int]$EvalEvery = 40,          # generations between evaluations
    [int]$ReportEvery = 300,       # seconds between reports
    [int]$Guard = 4,               # evals in a row clearly worse than the best before rolling back; 0 = off
    [switch]$Stop
)

$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'hidden.ps1')

$Root     = Split-Path -Parent $PSScriptRoot
$DataDir  = Join-Path $Root 'data'
$LogDir   = Join-Path $Root 'logs'
$Name     = if ($Slot) { $Slot } else { 'main' }
$Sfx      = if ($Slot) { "_$Slot" } else { '' }
$PowerFile = Join-Path $DataDir "power$Sfx.txt"
$StopFile  = Join-Path $DataDir "daemon$Sfx.stop"
$TrainLog  = Join-Path $DataDir "train_log$Sfx.csv"
$Ckpt      = Join-Path $DataDir "ckpt$Sfx.npz"
$LogFile   = Join-Path $LogDir "daemon$Sfx.log"
$LearnLog  = Join-Path $LogDir "learner$Sfx.log"
$MapFile   = Join-Path $DataDir 'map.txt'
$Data      = Join-Path $GameRoot 'cstrike\addons\sourcemod\data\csai'
$OutDir    = Join-Path $Data "out$Sfx"
$Weights   = Join-Path $Data "weights$Sfx.txt"
$EvalCkpt  = Join-Path $DataDir "eval_ckpt$Sfx.npz"
$BestCkpt  = Join-Path $DataDir "ckpt_best$Sfx.npz"
$BestFile  = Join-Path $DataDir "best$Sfx.txt"
$EvalLog   = Join-Path $LogDir "eval_$Name.log"

# Ports: 27015 + 10 per actor for main; other slots get their own block.
$PortBase = 27015
if ($Slot) { $PortBase = 27215 + 200 * ((($Slot.ToCharArray() | ForEach-Object { [int]$_ }) | Measure-Object -Sum).Sum % 8) }
$EvalPort = $PortBase + 150

New-Item -ItemType Directory -Force -Path $DataDir, $LogDir | Out-Null
foreach ($f in @($LogFile, $LearnLog)) {
    if ((Test-Path $f) -and (Get-Item $f).Length -gt 5MB) { Move-Item $f ($f -replace '\.log$', '.old.log') -Force }
}

if (-not $Map) {
    if (Test-Path $MapFile) { $Map = (Get-Content $MapFile -Raw).Trim([char]0xFEFF + " `t`r`n") }
    if (-not $Map) { $Map = 'surf_demise' }
}
# Several maps train one policy: the servers are shared out between them and
# evaluations take turns. The first map is the one reports are about.
$Maps = @($Map -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$Map = $Maps[0]
if (-not $Slot) { Set-Content -Path $MapFile -Value $Map -Encoding ascii }

$Levels = @{
    'idle'   = @{ Timescale = 5;   Priority = 'Idle';        Batch = 32; Actors = 1;  Desc = 'barely noticeable' }
    'low'    = @{ Timescale = 20;  Priority = 'BelowNormal'; Batch = 48; Actors = 2;  Desc = 'while using the PC' }
    'medium' = @{ Timescale = 80;  Priority = 'Normal';      Batch = 64; Actors = 4;  Desc = 'background work' }
    'high'   = @{ Timescale = 80;  Priority = 'Normal';      Batch = 64; Actors = 6;  Desc = 'about half the machine' }
    'max'    = @{ Timescale = 80;  Priority = 'AboveNormal'; Batch = 96; Actors = 11; Desc = 'all of it' }
}

function Write-Log([string]$msg) {
    $line = "{0}  {1}" -f (Get-Date -Format 'HH:mm:ss'), $msg
    Write-Host $line
    try { Add-Content -Path $LogFile -Value $line -Encoding utf8 } catch {}
}

function Get-Power {
    if (Test-Path $PowerFile) {
        $v = (Get-Content $PowerFile -Raw -ErrorAction SilentlyContinue)
        if ($v) {
            $v = $v.Trim([char]0xFEFF + " `t`r`n").ToLower()
            if ($Levels.ContainsKey($v)) { return $v }
        }
    }
    return 'high'
}

function Get-Procs([string]$name, [string]$match) {
    @(Get-CimInstance Win32_Process -Filter "Name='$name'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like $match })
}

function Test-Mine([string]$cl) {
    # Which slot a process belongs to, from its command line.
    $theirs = 'main'
    if ($cl -match '\+csai_slot\s+(\S+)') { $theirs = $Matches[1] }
    elseif ($cl -match '-Slot\s+(\S+)') { $theirs = $Matches[1] }
    elseif ($cl -match 'out_([A-Za-z0-9]+)') { $theirs = $Matches[1] }
    return ($theirs -eq $Name)
}

function Get-MyLearners { @(Get-Procs 'python.exe' '*learn.py*' | Where-Object { Test-Mine $_.CommandLine }) }
function Get-MySupervisors {
    @(Get-Procs 'powershell.exe' '*daemon.ps1*' | Where-Object {
        $_.ProcessId -ne $PID -and $_.CommandLine -notlike '*-Stop*' -and (Test-Mine $_.CommandLine) })
}

function Stop-All {
    foreach ($p in (Get-MyLearners)) {
        Write-Log "  stop learner pid $($p.ProcessId)"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
    foreach ($p in (Get-Procs 'powershell.exe' '*eval.ps1*')) {
        if (Test-Mine $p.CommandLine) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
    }
    foreach ($p in (Get-Procs 'srcds_win64.exe' '*+csai_actor*')) {
        if (-not (Test-Mine $p.CommandLine)) { continue }
        Write-Log "  stop server pid $($p.ProcessId)"
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
    $la = @((Join-Path $Root 'tools\learn.py'), '--batches', '1000000', '--timeout', '900',
            '--gamma', $Gamma, '--ent', $Entropy, '--frameskip', $FrameSkip,
            '--track', (Join-Path $Data "$($Map)_track.txt"),
            '--states', (Join-Path $Data "$($Map)_states.txt"), '--data', $Data,
            '--weights', $Weights, '--outdir', $OutDir,
            '--ckpt', $Ckpt, '--log', $TrainLog, '--out', $LearnLog)
    if ($EntFinal -gt 0.0) { $la += @('--ent-final', $EntFinal, '--ent-anneal', $EntAnneal) }
    $la += @('--kl-ref', $KlRef)
    if (Test-Path $Ckpt) { $la += '--resume' }
    $lp = Start-Hidden 'python.exe' $la (Join-Path $Root 'tools')
    if ($lp) { try { $lp.PriorityClass = 'AboveNormal' } catch {} }
    Write-Log "  learner started$(if ($la -contains '--resume') { ' (resuming)' }), output in logs\$(Split-Path $LearnLog -Leaf)"
}

function Start-Actor([string]$level, [int]$id) {
    $cfg = $Levels[$level]
    # A half-written batch from a killed server would otherwise sit there forever.
    Get-ChildItem $OutDir -Filter "a${id}_batch_*.bin" -ErrorAction SilentlyContinue | ForEach-Object {
        if (-not (Test-Path ([IO.Path]::ChangeExtension($_.FullName, '.done')))) {
            Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue
        }
    }
    $a = @(
        '+csai_actor', $id,
        '+csai_seed', (([int]((Get-Date).Ticks % 100000)) * 32 + $id * 7919 + 1),
        '+map', $Maps[$id % $Maps.Count],
        '+csai_train_batches', '1000000', '+csai_train_sync', '1',
        '+csai_batch', $cfg.Batch, '+csai_frameskip', $FrameSkip,
        '+csai_states', '1', '+csai_budget', '6000', '+csai_deviation', '600',
        '+csai_statemix', $StateMix, '+csai_statelo', $StateLo, '+csai_statehi', $StateHi,
        '+csai_focusmix', $FocusMix,
        '+csai_prestrafe', '1', '+csai_switchcost', $SwitchCost, '+csai_devcost', $DevCost,
        '+csai_timecost', $TimeCost, '+csai_trimcost', $TrimCost,
        '+csai_finishbonus', $FinishBonus, '+csai_finishfloor', $FinishFloor,
        '+csai_windup', $Windup, '+csai_learnedmix', $LearnedMix, '+csai_linejitter', $LineJitter,
        '+csai_mouseacc', $MouseAcc, '+csai_mousemax', $MouseMax,
        '+csai_minpress', $MinPress, '+csai_mincoast', $MinCoast,
        '+csai_bench_timescale', $cfg.Timescale,
        '+csai_bench_quit', '0', '+csai_bench_delay', '8'
    )
    if ($Slot) { $a += @('+csai_slot', $Slot) }
    if ($Partner) { $a += @('+csai_partner', $Partner) }
    $p = Start-Srcds "csai_${Name}_a$id" ($PortBase + 10 * $id) $a
    if (-not $p) { Write-Log "  actor $id did not start"; return -1 }
    Start-Sleep -Seconds 2
    try { $p.PriorityClass = $cfg.Priority } catch {}
    Write-Log "  actor $id started pid $($p.Id) - timescale $($cfg.Timescale), priority $($cfg.Priority), batch $($cfg.Batch)"
    return $p.Id
}

function Invoke-Report([switch]$Snapshot) {
    if ($Slot) { return }
    $a = @((Join-Path $Root 'tools\report.py'))
    if ($Snapshot) { $a += '--snapshot' }
    $p = Start-Hidden 'python.exe' $a $Root
    if ($p) { [void]$p.WaitForExit(120000) }
}

function Invoke-Eval {
    $m = $Maps[$script:EvalCount % $Maps.Count]
    $script:EvalCount++
    $script:EvalMap = $m
    Write-Log "  evaluating the policy on $m (writes a replay)"
    # The checkpoint as it stands now, in case this eval turns out to be the best.
    try { Copy-Item $Ckpt $EvalCkpt -Force -ErrorAction Stop } catch { Remove-Item $EvalCkpt -ErrorAction SilentlyContinue }
    $ea = @('-Runs', '8', '-Map', $m, '-Greedy', '1', '-Port', $EvalPort,
            '-FrameSkip', $FrameSkip, '-DevCost', $DevCost, '-Windup', $Windup,
            '-Partner', $Partner, '-MouseAcc', $MouseAcc, '-MouseMax', $MouseMax,
            '-MinPress', $MinPress, '-MinCoast', $MinCoast,
            '-Timescale', '80', '-TimeoutSec', '600')
    if ($Slot) { $ea += @('-Slot', $Slot) }
    return (Start-HiddenPowerShell (Join-Path $Root 'tools\eval.ps1') $ea $Root)
}

# ---------------------------------------------------------------------------

if ($Stop) {
    Write-Log "stopping slot $Name"
    New-Item -ItemType File -Force -Path $StopFile | Out-Null
    Stop-All
    # The supervisor only sees the marker on its next poll; give it that long.
    for ($i = 0; $i -lt 40 -and @(Get-MySupervisors).Count -gt 0; $i++) { Start-Sleep -Seconds 1 }
    foreach ($d in (Get-MySupervisors)) {
        Write-Log "  supervisor $($d.ProcessId) did not exit - ending it"
        Stop-Process -Id $d.ProcessId -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 2
    Stop-All
    Write-Log 'stopped'
    exit 0
}

# One supervisor per slot.
$others = @(Get-MySupervisors)
if ($others.Count -gt 0) {
    Write-Log "refusing to start: slot '$Name' already has a supervisor (pid $($others[0].ProcessId))"
    exit 1
}

Remove-Item $StopFile -ErrorAction SilentlyContinue
if ($Power) { Set-Content -Path $PowerFile -Value $Power -Encoding ascii }
$level = Get-Power

Write-Log "================ CsAI training ================  slot: $Name"
Write-Log "power: $level - $($Levels[$level].Desc)"
Stop-All
Start-Sleep -Seconds 2

# Leftovers from the last run are from an older policy.
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$swept = @(Get-ChildItem $OutDir -Filter 'a*_batch_*' -ErrorAction SilentlyContinue)
$swept | Remove-Item -Force -ErrorAction SilentlyContinue
if ($swept.Count -gt 0) { Write-Log "  cleared $($swept.Count) old batch file(s)" }
Remove-Item (Join-Path $LogDir "learner$Sfx.err.log") -ErrorAction SilentlyContinue

$ActorsOverride = $Actors
if ($Actors -le 0) { $Actors = $Levels[$level].Actors }

Start-Learner
Start-Sleep -Seconds 3
$ActorPids = @{}
$ActorLen = @{}
$ActorSeen = @{}
for ($i = 0; $i -lt $Actors; $i++) {
    $ActorPids[$i] = Start-Actor $level $i
    $ActorSeen[$i] = Get-Date
    Start-Sleep -Seconds 2
}
Write-Log "  $Actors server(s) running on $([Environment]::ProcessorCount) logical cores"

$evalProc = $null
$evalStartedAt = $null
$lastGen = Get-Gen
$lastGenAt = Get-Date
$stallWarned = $false
$stallRestarts = 0
$badEvals = 0
$rolledBack = $false
$EvalCount = 0
$EvalMap = $Map
$lastEvalGen = $lastGen
$lastReport = Get-Date
$appliedLevel = $level

# The last evaluation in the eval log: runs, finishes and the median finish time.
function Read-EvalResult {
    if (-not (Test-Path $EvalLog)) { return $null }
    $block = @()
    foreach ($l in (Get-Content $EvalLog -Tail 60)) {
        if ($l -like '# *') { $block = @() } else { $block += $l }
    }
    $gen = 0
    $times = @()
    $runs = 0
    $fastest = 0.0
    $record = 0.0
    foreach ($l in $block) {
        if ($l -match 'best time ([\d.]+)s \(human ([\d.]+)s\)') { $fastest = [double]$Matches[1]; $record = [double]$Matches[2] }
        if ($l -match 'policy gen (\d+)') { $gen = [int]$Matches[1] }
        if ($l -match 'eval run \d+/\d+: (\S+) at [\d.]+% in ([\d.]+)s') {
            $runs++
            if ($Matches[1] -eq 'FINISHED') { $times += [double]$Matches[2] }
        }
    }
    if ($runs -eq 0) { return $null }
    $med = 999.0
    if ($times.Count) {
        $st = @($times | Sort-Object)
        $med = ($st[[int][math]::Floor(($st.Count - 1) / 2)] + $st[[int][math]::Ceiling(($st.Count - 1) / 2)]) / 2
    }
    return [pscustomobject]@{ Gen = $gen; Runs = $runs; Finished = $times.Count; Median = $med
                              Fastest = $fastest; Record = $record }
}

# How human the evaluated run looks, next to the record on that map.
function Test-Humanlike([string]$m, [int]$gen) {
    $tag = if ($Slot) { "${Slot}_gen$gen" } else { "gen$gen" }
    $rep = Join-Path $Data "replays\$($m)_$tag.replay"
    if (-not (Test-Path $rep)) { return }
    $sum = Join-Path $LogDir "humanlike$Sfx.txt"
    Remove-Item $sum -ErrorAction SilentlyContinue
    $hp = Start-Hidden 'python.exe' @((Join-Path $Root 'tools\humanlike.py'), $rep, '--map', $m, '--summary', $sum) (Join-Path $Root 'tools')
    if ($hp) { [void]$hp.WaitForExit(60000) }
    if (Test-Path $sum) { Write-Log "  looks: $((Get-Content $sum -Raw).Trim())" }
}

# The first map keeps the plain names; the others add the map to them.
function Best-Paths([string]$m) {
    $tag = if ($m -eq $Map) { $Sfx } else { "$Sfx.$m" }
    return @((Join-Path $DataDir "best$tag.txt"), (Join-Path $DataDir "ckpt_best$tag.npz"))
}

function Read-Best([string]$file) {
    if (-not (Test-Path $file)) { return $null }
    $f = (Get-Content $file -Raw).Trim() -split '\s+'
    if ($f.Count -lt 4) { return $null }
    return [pscustomobject]@{ Gen = [int]$f[0]; Finished = [int]$f[1]; Runs = [int]$f[2]; Median = [double]$f[3] }
}

# Keeps the best checkpoint by evaluation, and rolls back once if training falls
# clearly behind it for several evaluations in a row.
function Update-Best {
    $r = Read-EvalResult
    if (-not $r) { return }
    $m = $script:EvalMap
    $bestFile, $bestCkpt = Best-Paths $m
    $b = Read-Best $bestFile
    $line = "{0} {1} {2} {3:F3}" -f $r.Gen, $r.Finished, $r.Runs, $r.Median
    Write-Log ("  eval {0} gen {1}: {2}/{3} finished, median {4}" -f $m, $r.Gen, $r.Finished, $r.Runs,
               $(if ($r.Finished) { '{0:N2}s' -f $r.Median } else { '-' }))
    Test-Humanlike $m $r.Gen
    if ($r.Fastest -gt 0 -and $r.Record -gt 0 -and $r.Fastest -lt $r.Record) {
        $msg = "{0}: {1:N3}s against the record {2:N3}s, gen {3}, {4:yyyy-MM-dd HH:mm}" -f $m, $r.Fastest, $r.Record, $r.Gen, (Get-Date)
        Write-Log "  RECORD BEATEN - $msg"
        Add-Content -Path (Join-Path $DataDir 'records.txt') -Value $msg -Encoding ascii
    }
    $better = (-not $b) -or ($r.Finished -gt $b.Finished) -or
              ($r.Finished -eq $b.Finished -and $r.Median -lt $b.Median - 0.001)
    if ($better -and (Test-Path $EvalCkpt)) {
        Copy-Item $EvalCkpt $bestCkpt -Force
        Set-Content -Path $bestFile -Value $line -Encoding ascii
        Write-Log "  new best on $m - checkpoint kept"
        if ($m -eq $Map) { $script:badEvals = 0 }
        return
    }
    if ($m -ne $Map) { return }             # rolling back is judged on the first map only
    $worse = $b -and (($r.Finished -le $b.Finished - 2) -or ($r.Finished -gt 0 -and $b.Finished -gt 0 -and $r.Median -gt $b.Median + 0.25))
    $script:badEvals = if ($worse) { $script:badEvals + 1 } else { 0 }
    if ($Guard -gt 0 -and $script:badEvals -ge $Guard -and -not $script:rolledBack -and (Test-Path $BestCkpt)) {
        Write-Log "  $($script:badEvals) evals in a row clearly behind the best (gen $($b.Gen)) - rolling back to it"
        foreach ($p in (Get-MyLearners)) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Seconds 2
        $rp = Start-Hidden 'python.exe' @((Join-Path $Root 'tools\restore.py'), $BestCkpt, $Ckpt) $Root
        if ($rp) { [void]$rp.WaitForExit(60000) }
        Start-Learner
        $script:rolledBack = $true
        $script:badEvals = 0
    }
}

function Restart-Actor([int]$i) {
    if ($ActorPids.ContainsKey($i) -and $ActorPids[$i] -gt 0) {
        Stop-Process -Id $ActorPids[$i] -Force -ErrorAction SilentlyContinue
    }
    $script:ActorPids[$i] = Start-Actor $appliedLevel $i
    $script:ActorSeen[$i] = Get-Date
    $script:ActorLen.Remove($i)
}

while ($true) {
    Start-Sleep -Seconds 15
    if (Test-Path $StopFile) { Write-Log 'stop requested'; Stop-All; break }

    if ($evalProc -and $evalProc.HasExited) {
        Write-Log ("  eval finished in {0:N0}s" -f ((Get-Date) - $evalStartedAt).TotalSeconds)
        Update-Best
        Invoke-Report
        $evalProc = $null
    }

    # Power change: restart the servers with the new settings. The learner keeps going.
    $want = Get-Power
    if ($want -ne $appliedLevel) {
        Write-Log "power changed: $appliedLevel -> $want ($($Levels[$want].Desc))"
        $newCount = if ($ActorsOverride -gt 0) { $ActorsOverride } else { $Levels[$want].Actors }
        foreach ($k in @($ActorPids.Keys)) {
            Stop-Process -Id $ActorPids[$k] -Force -ErrorAction SilentlyContinue
            if ($k -ge $newCount) { $ActorPids.Remove($k) }
        }
        $Actors = $newCount
        $appliedLevel = $want
        Start-Sleep -Seconds 3
        for ($i = 0; $i -lt $Actors; $i++) { Restart-Actor $i; Start-Sleep -Seconds 2 }
    }

    if (Test-Path $StopFile) { Write-Log 'stop requested'; Stop-All; break }

    if (@(Get-MyLearners).Count -eq 0) {
        Write-Log 'learner died - restarting'
        if (Test-Path $LearnLog) {
            Get-Content $LearnLog -Tail 6 -ErrorAction SilentlyContinue | ForEach-Object { if ($_.Trim()) { Write-Log "    $_" } }
        }
        Start-Learner
        Start-Sleep -Seconds 3
    }

    $gen = Get-Gen
    $learning = ($gen -ne $lastGen)

    for ($i = 0; $i -lt $Actors; $i++) {
        $alive = $ActorPids.ContainsKey($i) -and $ActorPids[$i] -gt 0 -and
                 [bool](Get-Process -Id $ActorPids[$i] -ErrorAction SilentlyContinue)
        if (-not $alive) {
            Write-Log "server $i died - restarting"
            Restart-Actor $i
            continue
        }
        # A server stuck on an error box prints nothing while the rest keep training.
        $f = Join-Path $SrcdsLogDir "csai_${Name}_a$i.log"
        $len = if (Test-Path $f) { (Get-Item $f).Length } else { 0 }
        if (-not $ActorLen.ContainsKey($i) -or $ActorLen[$i] -ne $len) {
            $ActorLen[$i] = $len
            $ActorSeen[$i] = Get-Date
        } elseif ($learning -and ((Get-Date) - $ActorSeen[$i]).TotalSeconds -ge $SilentSeconds) {
            Write-Log "server $i has been silent for ${SilentSeconds}s - restarting"
            Restart-Actor $i
        }
    }

    if ($learning) {
        $lastGen = $gen
        $lastGenAt = Get-Date
        $stallWarned = $false
    } elseif (((Get-Date) - $lastGenAt).TotalSeconds -ge $StallSeconds) {
        $stalledFor = [int]((Get-Date) - $lastGenAt).TotalSeconds
        if (-not $stallWarned) {
            Write-Log "STALLED: no new generation for ${stalledFor}s (gen $gen)"
            $orphans = @(Get-ChildItem $OutDir -Filter 'a*_batch_*.bin' -ErrorAction SilentlyContinue |
                         Where-Object { -not (Test-Path ([IO.Path]::ChangeExtension($_.FullName, '.done'))) -and
                                        $_.LastWriteTime -lt (Get-Date).AddMinutes(-5) })
            if ($orphans.Count -gt 2) { Write-Log "  $($orphans.Count) batch files have no .done marker after five minutes" }
            Write-Log '  last lines from the learner:'
            if (Test-Path $LearnLog) {
                Get-Content $LearnLog -Tail 6 -ErrorAction SilentlyContinue | ForEach-Object { if ($_.Trim()) { Write-Log "    $_" } }
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
            for ($i = 0; $i -lt $Actors; $i++) { Restart-Actor $i; Start-Sleep -Seconds 2 }
            $lastGenAt = Get-Date
            $stallWarned = $false
        }
    }

    if (((Get-Date) - $lastReport).TotalSeconds -ge $ReportEvery) {
        Invoke-Report -Snapshot
        $lastReport = Get-Date
    }

    if ($EvalEvery -gt 0 -and ($gen - $lastEvalGen) -ge $EvalEvery) {
        if ($evalProc -and -not $evalProc.HasExited) {
            Write-Log '  eval still running, skipping this one'
        } else {
            $evalProc = Invoke-Eval
            $evalStartedAt = Get-Date
        }
        $lastEvalGen = $gen
    }
}

Invoke-Report -Snapshot
Write-Log 'daemon exited'
