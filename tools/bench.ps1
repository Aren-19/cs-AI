param(
    [double[]]$Scales = @(1),
    [int]$Ticks       = 2000,
    [string]$Map      = 'surf_demise',
    [int]$State       = 0,
    [int]$TimeoutSec  = 300,
    [switch]$KeepLog
)

$ErrorActionPreference = 'Stop'

$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source'
$Cstrike  = Join-Path $GameRoot 'cstrike'
$Srcds    = Join-Path $GameRoot 'srcds_win64.exe'
$ConLog   = Join-Path $Cstrike  'console.log'

if (-not (Test-Path $Srcds)) { throw "srcds not found at $Srcds" }

$results = @()

foreach ($scale in $Scales) {
    Write-Host "==> $Map  state $State  timescale $scale" -ForegroundColor Cyan
    Remove-Item $ConLog -ErrorAction SilentlyContinue

    # NOTE: no -tickrate (engine default 66.7), and server_66.cfg for 66-tick cvars.
    $a = @(
        '-console', '-game', 'cstrike', '-maxplayers', '6',
        '+sv_lan', '1', '-insecure', '-condebug',
        '+servercfgfile', 'server_66.cfg',
        '+map', $Map,
        '+csai_bench_ticks', $Ticks,
        '+csai_bench_timescale', $scale,
        '+csai_bench_state', $State,
        '+csai_bench_quit', '1',
        '+csai_bench_delay', '8'
    )

    # Launch via Start-Process so srcds gets a real console: with stdin redirected
    # it hits EOF and shuts down before the run starts.
    $proc = Start-Process -FilePath $Srcds -ArgumentList $a -WorkingDirectory $GameRoot -PassThru -WindowStyle Hidden
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        Write-Host '    timed out, killing' -ForegroundColor Yellow
        try { $proc.Kill() } catch {}
        Start-Sleep -Seconds 2
    }

    if (-not (Test-Path $ConLog)) {
        Write-Host '    no console.log produced' -ForegroundColor Red
        continue
    }
    $log = Get-Content $ConLog -Raw

    $tps   = if ($log -match 'throughput\s*:\s*([\d.]+)\s*ticks/s') { [double]$Matches[1] } else { $null }
    $speed = if ($log -match 'speedup\s*:\s*([\d.]+)x')             { [double]$Matches[1] } else { $null }
    $hs    = if ($log -match 'h-speed\s*:\s*start\s*([\d.]+)\s*->\s*end\s*([\d.]+)\s*\(peak\s*([\d.]+)\)') {
                 @([double]$Matches[1], [double]$Matches[2], [double]$Matches[3])
             } else { $null }

    if ($null -eq $tps) {
        Write-Host '    run did not complete - last CsAI lines:' -ForegroundColor Red
        Select-String -Path $ConLog -Pattern 'CsAI' | Select-Object -Last 8 | ForEach-Object { "      $($_.Line)" }
        continue
    }

    $results += [pscustomobject]@{
        Timescale   = $scale
        TicksPerSec = [math]::Round($tps, 0)
        SpeedupX    = [math]::Round($speed, 2)
        StartSpeed  = if ($hs) { [math]::Round($hs[0], 0) } else { $null }
        EndSpeed    = if ($hs) { [math]::Round($hs[1], 0) } else { $null }
        PeakSpeed   = if ($hs) { [math]::Round($hs[2], 0) } else { $null }
    }

    Write-Host ("    {0} ticks/s ({1}x)   h-speed {2} -> {3}, peak {4}" -f `
        [math]::Round($tps,0), [math]::Round($speed,2),
        $(if ($hs) { [math]::Round($hs[0],0) } else { '?' }),
        $(if ($hs) { [math]::Round($hs[1],0) } else { '?' }),
        $(if ($hs) { [math]::Round($hs[2],0) } else { '?' })) -ForegroundColor Green

    if ($KeepLog) {
        Copy-Item $ConLog (Join-Path $PSScriptRoot ("..\data\console_{0}_ts{1}.log" -f $Map, $scale)) -Force
    }
}

Write-Host ''
$results | Format-Table -AutoSize
