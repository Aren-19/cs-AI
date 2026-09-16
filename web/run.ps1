<#
    Start the local replay viewer: backend + frontend.

        .\web\run.ps1              # start both, open the browser
        .\web\run.ps1 -NoBrowser
        .\web\run.ps1 -Stop        # stop both

    Backend  : http://127.0.0.1:8787   (tools/webserve.py)
    Frontend : http://127.0.0.1:3000   (offstyles-web, Vite)

    The frontend's Vite config proxies /api and /maps to the backend, so the
    viewer reads our replays and our local CS:S install instead of offstyles.net.
#>
param(
    [int]$ApiPort = 8787,
    [int]$WebPort = 3000,
    [switch]$NoBrowser,
    [switch]$Stop
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Web = Join-Path $Root 'web\offstyles-web'

function Stop-Existing {
    Get-CimInstance Win32_Process -Filter "Name='python.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like '*webserve.py*' } |
        ForEach-Object { Write-Host "  stopping backend pid $($_.ProcessId)"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Get-CimInstance Win32_Process -Filter "Name='node.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like '*vite*' } |
        ForEach-Object { Write-Host "  stopping frontend pid $($_.ProcessId)"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

if ($Stop) { Write-Host 'Stopping...' -ForegroundColor Yellow; Stop-Existing; exit 0 }

Stop-Existing

if (-not (Test-Path (Join-Path $Web 'node_modules'))) {
    Write-Host '==> installing frontend dependencies (first run)' -ForegroundColor Cyan
    Push-Location $Web
    cmd /c npm install --no-audit --no-fund
    cmd /c npm install-scripts approve esbuild 2>$null   # vite needs esbuild's platform binary
    cmd /c npm install --no-audit --no-fund
    Pop-Location
}

Write-Host "==> backend  http://127.0.0.1:$ApiPort" -ForegroundColor Cyan
Start-Process -FilePath 'python' `
    -ArgumentList @((Join-Path $Root 'tools\webserve.py'), '--port', $ApiPort) `
    -WorkingDirectory $Root -WindowStyle Hidden | Out-Null

# wait for it to answer before starting the frontend
$ok = $false
foreach ($i in 1..40) {
    Start-Sleep -Milliseconds 500
    try { Invoke-WebRequest "http://127.0.0.1:$ApiPort/health" -UseBasicParsing -TimeoutSec 2 | Out-Null; $ok = $true; break } catch {}
}
if (-not $ok) { Write-Host '    backend did not come up; check tools/webserve.py' -ForegroundColor Red; exit 1 }
Write-Host '    backend ready' -ForegroundColor Green

Write-Host "==> frontend http://127.0.0.1:$WebPort" -ForegroundColor Cyan
# Start-Process cannot resolve 'npx' on Windows (it is npx.cmd), so go via cmd.
Start-Process -FilePath 'cmd.exe' `
    -ArgumentList @('/c', 'npx', 'vite', '--port', $WebPort, '--host', '127.0.0.1') `
    -WorkingDirectory $Web -WindowStyle Hidden | Out-Null

$ok = $false
foreach ($i in 1..60) {
    Start-Sleep -Milliseconds 500
    try { Invoke-WebRequest "http://127.0.0.1:$WebPort/" -UseBasicParsing -TimeoutSec 2 | Out-Null; $ok = $true; break } catch {}
}
if (-not $ok) { Write-Host '    frontend did not come up' -ForegroundColor Red; exit 1 }
Write-Host '    frontend ready' -ForegroundColor Green

Write-Host ''
Write-Host "  open  http://127.0.0.1:$WebPort" -ForegroundColor White
Write-Host '  stop  .\web\run.ps1 -Stop' -ForegroundColor DarkGray
Write-Host ''

if (-not $NoBrowser) { Start-Process "http://127.0.0.1:$WebPort" }
