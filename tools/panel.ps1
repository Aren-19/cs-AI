<#
    CsAI control panel.

    Launch it by double-clicking CsAI.bat in the project root. Everything you
    need for an unattended run is here: start training at a chosen power level,
    change that level while it runs, open the replay viewer, and read the report.
#>
$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $PSScriptRoot
$PowerFile = Join-Path $Root 'data\power.txt'
$TrainLog = Join-Path $Root 'data\train_log.csv'
$Report = Join-Path $Root 'reports\latest.md'

$Levels = @(
    @{ Key = '1'; Name = 'idle';   Desc = 'barely noticeable - gaming or video'; Ts = 5 }
    @{ Key = '2'; Name = 'low';    Desc = 'you are using the PC';                Ts = 20 }
    @{ Key = '3'; Name = 'medium'; Desc = 'background work';                     Ts = 50 }
    @{ Key = '4'; Name = 'high';   Desc = 'default - knee of the curve';         Ts = 80 }
    @{ Key = '5'; Name = 'max';    Desc = 'you are away';                        Ts = 150 }
)

function Get-Power {
    if (Test-Path $PowerFile) {
        $v = (Get-Content $PowerFile -Raw -ErrorAction SilentlyContinue).Trim().ToLower()
        if ($v) { return $v }
    }
    return 'high'
}

function Test-Daemon {
    $p = Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
         Where-Object { $_.CommandLine -like '*daemon.ps1*' -and $_.CommandLine -notlike '*-Stop*' }
    return [bool]$p
}
function Test-Actor  { [bool](Get-Process -Name 'srcds_win64' -ErrorAction SilentlyContinue) }
function Test-Viewer {
    try { Invoke-WebRequest 'http://127.0.0.1:3000/' -UseBasicParsing -TimeoutSec 1 | Out-Null; return $true }
    catch { return $false }
}

function Get-Stats {
    if (-not (Test-Path $TrainLog)) { return $null }
    try {
        $last = Get-Content $TrainLog -Tail 1 -ErrorAction SilentlyContinue
        if (-not $last -or $last -match '^gen,') { return $null }
        $f = $last -split ','
        return [pscustomobject]@{
            Gen  = [int]$f[0]
            Gain = [double]$f[5] * 100
            Best = [double]$f[6] * 100
            Ent  = [double]$f[11]
        }
    } catch { return $null }
}

function Show-Header {
    Clear-Host
    $running = Test-Daemon
    $stats = Get-Stats
    Write-Host ''
    Write-Host '  CsAI - Counter-Strike: Source surf AI' -ForegroundColor Cyan
    Write-Host '  ---------------------------------------------------------------'
    if ($running) {
        Write-Host '  training : ' -NoNewline; Write-Host 'RUNNING' -ForegroundColor Green -NoNewline
        Write-Host ("   power: {0}" -f (Get-Power).ToUpper())
        if (-not (Test-Actor)) { Write-Host '             (actor restarting...)' -ForegroundColor DarkYellow }
    } else {
        Write-Host '  training : ' -NoNewline; Write-Host 'stopped' -ForegroundColor DarkGray
    }
    if ($stats) {
        Write-Host ("  progress : gen {0}   gain {1:N2}%   best {2:N2}%   entropy {3:N2}" -f `
            $stats.Gen, $stats.Gain, $stats.Best, $stats.Ent)
    }
    Write-Host '  viewer   : ' -NoNewline
    if (Test-Viewer) { Write-Host 'http://127.0.0.1:3000' -ForegroundColor Green }
    else { Write-Host 'stopped' -ForegroundColor DarkGray }
    Write-Host '  target   : beat 39.10 s on surf_demise (100% of track)' -ForegroundColor DarkGray
    Write-Host '  ---------------------------------------------------------------'
    Write-Host ''
}

function Show-Menu {
    Write-Host '   [1] Start training          [2] Stop training'
    Write-Host '   [3] Change power level      [4] Watch replays (viewer)'
    Write-Host '   [5] Show report             [6] Make a replay now'
    Write-Host '   [7] Open reports folder     [8] Live log'
    Write-Host '   [0] Exit  (training keeps running)'
    Write-Host ''
}

function Choose-Power {
    Write-Host ''
    Write-Host '  Power level - how much of your machine training may use:' -ForegroundColor Cyan
    Write-Host ''
    $cur = Get-Power
    foreach ($l in $Levels) {
        $mark = '  '
        if ($l.Name -eq $cur) { $mark = ' *' }
        Write-Host ("  {0}[{1}] {2,-7} timescale {3,3}x   {4}" -f $mark, $l.Key, $l.Name, $l.Ts, $l.Desc)
    }
    Write-Host ''
    Write-Host '  Takes effect within ~15s. Nothing trained is lost - the learner' -ForegroundColor DarkGray
    Write-Host '  checkpoints every generation and resumes from it.' -ForegroundColor DarkGray
    Write-Host ''
    $c = Read-Host '  choice (blank to cancel)'
    $sel = $Levels | Where-Object { $_.Key -eq $c }
    if ($sel) {
        New-Item -ItemType Directory -Force -Path (Split-Path $PowerFile) | Out-Null
        Set-Content -Path $PowerFile -Value $sel.Name -Encoding utf8
        Write-Host ("  power set to {0}" -f $sel.Name.ToUpper()) -ForegroundColor Green
        if (-not (Test-Daemon)) { Write-Host '  (training is not running - start it with [1])' -ForegroundColor DarkYellow }
        Start-Sleep -Seconds 2
    }
}

function Start-Training {
    if (Test-Daemon) {
        Write-Host '  already running.' -ForegroundColor DarkYellow
        Start-Sleep -Seconds 2
        return
    }
    Choose-Power
    Write-Host '  starting...' -ForegroundColor Cyan
    Start-Process -FilePath 'powershell' `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                        (Join-Path $Root 'tools\daemon.ps1')) `
        -WorkingDirectory $Root -WindowStyle Minimized | Out-Null
    Start-Sleep -Seconds 6
    Write-Host '  training started. You can close this window; it keeps running.' -ForegroundColor Green
    Start-Sleep -Seconds 3
}

function Stop-Training {
    Write-Host '  stopping...' -ForegroundColor Yellow
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'tools\daemon.ps1') -Stop | Out-Null
    Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like '*daemon.ps1*' -and $_.CommandLine -notlike '*-Stop*' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Write-Host '  stopped.' -ForegroundColor Green
    Start-Sleep -Seconds 2
}

function Open-Viewer {
    if (-not (Test-Viewer)) {
        Write-Host '  starting the viewer (first run installs dependencies)...' -ForegroundColor Cyan
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'web\run.ps1') -NoBrowser
    }
    Start-Process 'http://127.0.0.1:3000'
    Write-Host '  opened http://127.0.0.1:3000' -ForegroundColor Green
    Write-Host '  the newest bot replay is at the top of Recent Times.' -ForegroundColor DarkGray
    Start-Sleep -Seconds 3
}

function Show-Report {
    & python (Join-Path $Root 'tools\report.py') | Out-Null
    Clear-Host
    if (Test-Path $Report) { Get-Content $Report | Out-Host }
    else { Write-Host '  no report yet.' }
    Write-Host ''
    Read-Host '  enter to return' | Out-Null
}

function Make-Replay {
    Write-Host '  running the current policy (takes a minute)...' -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'tools\eval.ps1') `
        -Runs 1 -Timescale 20 -TimeoutSec 300
    Write-Host ''
    Write-Host '  done - open the viewer with [4] to watch it.' -ForegroundColor Green
    Read-Host '  enter to return' | Out-Null
}

function Show-Live {
    $log = Join-Path $Root 'logs\daemon.log'
    Clear-Host
    Write-Host '  live daemon log - Ctrl+C to return' -ForegroundColor Cyan
    Write-Host ''
    if (Test-Path $log) { Get-Content $log -Tail 30 -Wait }
    else { Write-Host '  no log yet.'; Start-Sleep -Seconds 2 }
}

while ($true) {
    Show-Header
    Show-Menu
    $c = Read-Host '  choice'
    switch ($c) {
        '1' { Start-Training }
        '2' { Stop-Training }
        '3' { Choose-Power }
        '4' { Open-Viewer }
        '5' { Show-Report }
        '6' { Make-Replay }
        '7' { Start-Process (Join-Path $Root 'reports') }
        '8' { Show-Live }
        '0' { return }
        default { }
    }
}
