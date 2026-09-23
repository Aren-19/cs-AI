param(
    [int]$ApiPort = 8787,
    [int]$WebPort = 3000,
    [switch]$NoBrowser,
    [switch]$Stop
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Web = Join-Path $Root 'web\offstyles-web'
. (Join-Path $Root 'tools\hidden.ps1')

function Stop-Existing {
    Get-CimInstance Win32_Process -Filter "Name='python.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like '*webserve.py*' } |
        ForEach-Object { Write-Host "  stopping backend pid $($_.ProcessId)"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Get-CimInstance Win32_Process -Filter "Name='node.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like '*vite*' } |
        ForEach-Object { Write-Host "  stopping frontend pid $($_.ProcessId)"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

function Test-Up([string]$url) {
    try { Invoke-WebRequest $url -UseBasicParsing -TimeoutSec 2 | Out-Null; return $true } catch { return $false }
}

function Wait-Up([string]$url, [int]$tries) {
    foreach ($i in 1..$tries) {
        Start-Sleep -Milliseconds 500
        if (Test-Up $url) { return $true }
    }
    return $false
}

if ($Stop) { Write-Host 'Stopping...' -ForegroundColor Yellow; Stop-Existing; exit 0 }

$api = "http://127.0.0.1:$ApiPort/health"
$site = "http://127.0.0.1:$WebPort/"

if ((Test-Up $api) -and (Test-Up $site)) {
    Write-Host "  already running at $site" -ForegroundColor Green
} else {
    Stop-Existing

    # First use: fetch the two upstream repos at a known commit and apply the local changes.
    $upstream = @(
        @{ Name = 'replay-viewer'; Commit = '928473902377a5f4689841f8082b4104e5a290d9' },
        @{ Name = 'offstyles-web'; Commit = '658a451b9bf75426fd2083f0e82edc7376ebb1af' }
    )
    foreach ($u in $upstream) {
        $dir = Join-Path $Root "web\$($u.Name)"
        if (Test-Path $dir) { continue }
        Write-Host "==> fetching $($u.Name)" -ForegroundColor Cyan
        git clone -q "https://github.com/offstyles/$($u.Name).git" $dir
        git -C $dir checkout -q $u.Commit
        git -C $dir apply (Join-Path $Root "web\patches\$($u.Name).patch")
        if ($LASTEXITCODE -ne 0) { Write-Host "    could not apply the local changes to $($u.Name)" -ForegroundColor Red; exit 1 }
    }

    if (-not (Test-Path (Join-Path $Web 'node_modules'))) {
        Write-Host '==> installing frontend dependencies (first run)' -ForegroundColor Cyan
        Push-Location $Web
        cmd /c npm install --no-audit --no-fund
        cmd /c npm install-scripts approve esbuild 2>$null   # vite needs esbuild's platform binary
        cmd /c npm install --no-audit --no-fund
        Pop-Location
    }

    Write-Host "==> backend  http://127.0.0.1:$ApiPort" -ForegroundColor Cyan
    [void](Start-Hidden 'python.exe' @((Join-Path $Root 'tools\webserve.py'), '--port', $ApiPort) $Root)
    if (-not (Wait-Up $api 40)) { Write-Host '    backend did not come up; check tools/webserve.py' -ForegroundColor Red; exit 1 }
    Write-Host '    backend ready' -ForegroundColor Green

    Write-Host "==> frontend $site" -ForegroundColor Cyan
    # npx is a .cmd script, so it goes through cmd.
    [void](Start-Hidden 'cmd.exe' @('/c', 'npx', 'vite', '--port', $WebPort, '--host', '127.0.0.1') $Web)
    if (-not (Wait-Up $site 60)) { Write-Host '    frontend did not come up' -ForegroundColor Red; exit 1 }
    Write-Host '    frontend ready' -ForegroundColor Green
}

Write-Host ''
Write-Host "  open  $site" -ForegroundColor White
Write-Host '  stop  .\web\run.ps1 -Stop' -ForegroundColor DarkGray
Write-Host ''

if (-not $NoBrowser) { Start-Process $site }
