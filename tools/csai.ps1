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

function Start-Training {
    foreach ($slot in (Get-Slots)) {
        $sa = @(Read-SlotArgs $slot)
        if ($slot -ne 'main' -and -not ($sa -contains '-Slot')) { $sa += @('-Slot', $slot) }
        [void](Start-HiddenPowerShell $Daemon $sa $Root)
        Write-Host "  started $slot"
        Start-Sleep -Seconds 6
    }
}

function Stop-Training {
    $procs = @()
    foreach ($slot in (Get-Slots)) {
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
    $f = if ($first) { Join-Path $Data 'best.txt' } else { Join-Path $Data "best.$m.txt" }
    if (-not (Test-Path $f)) { return $null }
    $v = (Get-Content $f -Raw).Trim() -split '\s+'
    if ($v.Count -lt 4) { return $null }
    return [pscustomobject]@{ Gen = [int]$v[0]; Finished = [int]$v[1]; Runs = [int]$v[2]; Median = [double]$v[3] }
}

function Show-Status {
    $running = (Get-Supervisors).Count -gt 0
    Write-Host ("training is {0}" -f $(if ($running) { 'running' } else { 'stopped' }))
    Write-Host ''
    Write-Host ('{0,-22} {1,10} {2,12} {3,10}' -f 'map', 'record', 'bot best', 'finishes')
    $maps = Get-Maps 'main'
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
                if ($rec) {
                    $d = $b.Median - $rec
                    $note = if ($d -lt 0) { '  beats the record by {0:N2}s' -f (-$d) } else { '  {0:N2}s behind' -f $d }
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
        if (Test-Path $exe) { Start-Process $exe }
        else { [void](Start-HiddenPowerShell (Join-Path $Root 'tools\panel_gui.ps1') @() $Root) }
    }
    'teach' {
        if (-not $MapName) { Write-Host 'usage: CsAI.bat teach <map>'; exit 1 }
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
        if ((Get-Supervisors).Count -gt 0) {
            Write-Host 'restarting training so the servers pick it up'
            Stop-Training
            Start-Training
        } else {
            Write-Host 'training is stopped - start it from the panel or with: CsAI.bat start'
        }
    }
    'forget' {
        if (-not $MapName) { Write-Host 'usage: CsAI.bat forget <map>'; exit 1 }
        $maps = @(Get-Maps 'main' | Where-Object { $_ -ne $MapName })
        if (-not $maps) { Write-Host 'that is the only map in training - teach another one first'; exit 1 }
        Set-Maps 'main' $maps
        Write-Host "training on: $($maps -join ', ')"
        if ((Get-Supervisors).Count -gt 0) { Stop-Training; Start-Training }
    }
    'status' { Show-Status }
    'start'  { if ((Get-Supervisors).Count -gt 0) { Write-Host 'already running' } else { Start-Training } }
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
