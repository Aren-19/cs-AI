param(
    [double[]]$Scales = @(1),
    [int]$Ticks       = 2000,
    [string]$Map      = 'surf_demise',
    [int]$State       = 0,
    [int]$Port        = 26950,
    [int]$TimeoutSec  = 300,
    [switch]$KeepLog
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'hidden.ps1')

$results = @()

foreach ($scale in $Scales) {
    Write-Host "==> $Map  state $State  timescale $scale" -ForegroundColor Cyan

    # No -tickrate: the engine default is 66.7, and server_66.cfg sets the rest.
    $a = @(
        '+map', $Map,
        '+csai_bench_ticks', $Ticks,
        '+csai_bench_timescale', $scale,
        '+csai_bench_state', $State,
        '+csai_bench_quit', '1',
        '+csai_bench_delay', '8'
    )
    $log = Invoke-Srcds 'csai_bench' $Port $a $TimeoutSec

    $tps   = if ($log -match 'throughput\s*:\s*([\d.]+)\s*ticks/s') { [double]$Matches[1] } else { $null }
    $speed = if ($log -match 'speedup\s*:\s*([\d.]+)x')             { [double]$Matches[1] } else { $null }
    $hs    = if ($log -match 'h-speed\s*:\s*start\s*([\d.]+)\s*->\s*end\s*([\d.]+)\s*\(peak\s*([\d.]+)\)') {
                 @([double]$Matches[1], [double]$Matches[2], [double]$Matches[3])
             } else { $null }

    if ($null -eq $tps) {
        Write-Host '    run did not complete - last CsAI lines:' -ForegroundColor Red
        $log -split "`r?`n" | Where-Object { $_ -match 'CsAI' } | Select-Object -Last 8 | ForEach-Object { "      $_" }
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
        Set-Content -Path (Join-Path $PSScriptRoot ("..\data\console_{0}_ts{1}.log" -f $Map, $scale)) -Value $log -Encoding utf8
    }
}

Write-Host ''
$results | Format-Table -AutoSize
