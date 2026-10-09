# CsAI from the command line. CsAI.bat passes its arguments here.
#
#   CsAI.bat                  open the panel
#   CsAI.bat teach <map>      learn a map from its record run and train on it
#   CsAI.bat forget <map>     stop training on a map
#   CsAI.bat status           each map: the record, the bot's best, how often it finishes
#   CsAI.bat start | stop     start or stop training

param(
    [Parameter(Position = 0)][string]$Command = 'panel',
    [Parameter(Position = 1)][string]$MapName = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'hidden.ps1')

$Root   = Split-Path -Parent $PSScriptRoot
$Data   = Join-Path $Root 'data'
$Daemon = Join-Path $Root 'tools\daemon.ps1'
$SrvData = Join-Path $GameRoot 'cstrike\addons\sourcemod\data\csai'

function Get-ArgsFile([string]$slot) {
    if ($slot -eq 'main') { return Join-Path $Data 'daemon_args.txt' }
    return Join-Path $Data "daemon_args_$slot.txt"
}

function Get-Slots {
    $out = @('main')
    Get-ChildItem (Join-Path $Data 'daemon_args_*.txt') -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.Name -match '^daemon_args_(.+)\.txt$') { $out += $Matches[1] }
    }
    return $out
}

function Read-SlotArgs([string]$slot) {
    $f = Get-ArgsFile $slot
    if (-not (Test-Path $f)) { return @('-Map', 'surf_demise') }
    return @((Get-Content $f -Raw).Trim() -split '\s+')
}

function Get-Maps([string]$slot) {
    $t = Read-SlotArgs $slot
    $i = [array]::IndexOf($t, '-Map')
    if ($i -ge 0 -and $i + 1 -lt $t.Count) { return @($t[$i + 1] -split ',' | Where-Object { $_ }) }
    return @('surf_demise')
}

function Set-Maps([string]$slot, [string[]]$maps) {
    $t = @(Read-SlotArgs $slot)
    $i = [array]::IndexOf($t, '-Map')
    if ($i -ge 0 -and $i + 1 -lt $t.Count) { $t[$i + 1] = ($maps -join ',') }
    else { $t = @('-Map', ($maps -join ',')) + $t }
    Set-Content -Path (Get-ArgsFile $slot) -Value ($t -join ' ') -Encoding ascii
}

function Get-Supervisors {
    @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
      Where-Object { $_.CommandLine -like '*daemon.ps1*' -and $_.CommandLine -notlike '*-Stop*' })
}

function Get-RunningSlots {
    @(Get-Supervisors | ForEach-Object {
        if ($_.CommandLine -match '(^|\s)-Slot\s+(\S+)') { $Matches[2] } else { 'main' }
    } | Select-Object -Unique)
}

# Starts every slot (or only those named) that has no supervisor yet.
function Start-Training([string[]]$Only = @()) {
    $running = @(Get-RunningSlots)
    foreach ($slot in (Get-Slots)) {
        if ($Only.Count -gt 0 -and $Only -notcontains $slot) { continue }
        if ($running -contains $slot) { Write-Host "  $slot is already running"; continue }
        $sa = @(Read-SlotArgs $slot)
        if ($slot -ne 'main' -and -not ($sa -contains '-Slot')) { $sa += @('-Slot', $slot) }
        [void](Start-HiddenPowerShell $Daemon $sa $Root)
        Write-Host "  started $slot"
        Start-Sleep -Seconds 6
    }
}

function Stop-Training([string[]]$Only = @()) {
    $procs = @()
    foreach ($slot in (Get-Slots)) {
        if ($Only.Count -gt 0 -and $Only -notcontains $slot) { continue }
        $a = @('-Stop')
        if ($slot -ne 'main') { $a += @('-Slot', $slot) }
        $procs += Start-HiddenPowerShell $Daemon $a $Root
    }
    foreach ($p in $procs) { if ($p) { [void]$p.WaitForExit(90000) } }
    Write-Host '  training stopped'
}

function Get-Record([string]$m) {
    $f = Join-Path $SrvData "$($m)_states.txt"
    if (-not (Test-Path $f)) { return $null }
    $head = Get-Content $f -TotalCount 1
    if ($head -match 'clean_time=([\d.]+)') { return [double]$Matches[1] }
    return $null
}

function Get-BotBest([string]$m, [bool]$first) {
    $f = Join-Path $Data "best.$m.txt"
    if ($first -and -not (Test-Path $f)) { $f = Join-Path $Data 'best.txt' }   # written before best files were named
    if (-not (Test-Path $f)) { return $null }
    $v = (Get-Content $f -Raw).Trim() -split '\s+'
    if ($v.Count -lt 4) { return $null }
    # The fifth field, the median of runs with the bot's own wind-up, is newer.
    $own = if ($v.Count -ge 5) { [double]$v[4] } else { 999.0 }
    return [pscustomobject]@{ Gen = [int]$v[0]; Finished = [int]$v[1]; Runs = [int]$v[2]; Median = [double]$v[3]; Own = $own }
}

function Show-Status {
    $running = @(Get-Supervisors).Count -gt 0
    Write-Host ("training is {0}" -f $(if ($running) { 'running' } else { 'stopped' }))
    Write-Host ''
    Write-Host ('{0,-22} {1,10} {2,12} {3,10}' -f 'map', 'record', 'bot best', 'finishes')
    $maps = @(Get-Maps 'main')
    for ($i = 0; $i -lt $maps.Count; $i++) {
        $m = $maps[$i]
        $rec = Get-Record $m
        $b = Get-BotBest $m ($i -eq 0)
        $recS = if ($rec) { '{0:N2}s' -f $rec } else { '-' }
        $botS = '-'; $finS = '-'; $note = ''
        if ($b) {
            $finS = '{0}/{1}' -f $b.Finished, $b.Runs
            if ($b.Finished -gt 0) {
                $botS = '{0:N2}s' -f $b.Median
                # Only runs that opened with the bot's own wind-up are held against the record.
                if ($rec -and $b.Own -lt 999) {
                    $d = $b.Own - $rec
                    $note = if ($d -lt 0) { '  beats the record by {0:N2}s' -f (-$d) } else { '  {0:N2}s behind' -f $d }
                } elseif ($rec) {
                    $note = '  (no finish with its own wind-up yet)'
                }
            }
        }
        Write-Host ('{0,-22} {1,10} {2,12} {3,10}{4}' -f $m, $recS, $botS, $finS, $note)
    }
    $rf = Join-Path $Data 'records.txt'
    if (Test-Path $rf) {
        Write-Host ''
        Write-Host 'records the bot has beaten:'
        Get-Content $rf | ForEach-Object { Write-Host "  $_" }
    }
}

switch ($Command.ToLower()) {
    'panel' {
        $exe = Join-Path $Root 'CsAI.exe'
        # The panel is the one window that must be seen: never the hidden desktop.
        if (Test-Path $exe) { Start-Process $exe -WorkingDirectory $Root }
        else {
            Start-Process powershell.exe -WorkingDirectory $Root -WindowStyle Hidden -ArgumentList @(
                '-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden',
                '-File', ('"' + (Join-Path $Root 'tools\panel_gui.ps1') + '"'))
        }
    }
    'teach' {
        if (-not $MapName) { Write-Host 'usage: CsAI.bat teach <map>'; exit 1 }
        $MapName = $MapName.ToLower()           # as the timer and the game name it
        Write-Host "learning $MapName from its record run"
        & python (Join-Path $Root 'tools\setup_map.py') $MapName --brief
        if ($LASTEXITCODE -ne 0) { Write-Host "$MapName is not ready - see above"; exit 1 }
        $maps = @(Get-Maps 'main')
        if ($maps -contains $MapName) {
            Write-Host "$MapName is already in training"
        } else {
            Set-Maps 'main' ($maps + $MapName)
            Write-Host "added $MapName to training ($((@($maps) + $MapName) -join ', '))"
        }
        $running = @(Get-RunningSlots)
        if ($running.Count -gt 0) {
            Write-Host 'restarting training so the servers pick it up'
            Stop-Training $running
            Start-Training $running
        } else {
            Write-Host 'training is stopped - start it from the panel or with: CsAI.bat start'
        }
    }
    'forget' {
        if (-not $MapName) { Write-Host 'usage: CsAI.bat forget <map>'; exit 1 }
        $MapName = $MapName.ToLower()
        $all = @(Get-Maps 'main')
        if ($all -notcontains $MapName) { Write-Host "$MapName is not in training ($($all -join ', '))"; exit 1 }
        $maps = @($all | Where-Object { $_ -ne $MapName })
        if (-not $maps) { Write-Host 'that is the only map in training - teach another one first'; exit 1 }
        # Older best files kept the first map's under the plain names; name them first.
        foreach ($sfx in @('', '_windup')) {
            foreach ($pair in @(@("best$sfx.txt", "best$sfx.$($all[0]).txt"), @("ckpt_best$sfx.npz", "ckpt_best$sfx.$($all[0]).npz"))) {
                $old = Join-Path $Data $pair[0]; $new = Join-Path $Data $pair[1]
                if ((Test-Path $old) -and -not (Test-Path $new)) { Move-Item $old $new }
            }
        }
        Set-Maps 'main' $maps
        Write-Host "training on: $($maps -join ', ')"
        $running = @(Get-RunningSlots)
        if ($running.Count -gt 0) { Stop-Training $running; Start-Training $running }
    }
    'status' { Show-Status }
    'start'  { Start-Training }
    'stop'   { Stop-Training }
    default {
        Write-Host 'CsAI.bat                  open the panel'
        Write-Host 'CsAI.bat teach <map>      learn a map from its record run and train on it'
        Write-Host 'CsAI.bat forget <map>     stop training on a map'
        Write-Host 'CsAI.bat status           the record and the bot''s best on each map'
        Write-Host 'CsAI.bat start | stop     start or stop training'
        exit 1
    }
}
