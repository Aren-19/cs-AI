<#
    Compile the CsAI plugin(s) with the server's own SourceMod compiler and
    deploy the .smx into the live server.

    Never install a downloaded .smx here: this server is a Windows-x64 build and
    pre-64-bit binaries use the old ABI. Always compile from source, with the
    compiler that ships alongside the server's SourceMod.

    Usage:  .\tools\build.ps1            # compile + deploy
            .\tools\build.ps1 -NoDeploy  # compile only
#>
param(
    [switch]$NoDeploy
)

$ErrorActionPreference = 'Stop'

$Root      = Split-Path -Parent $PSScriptRoot
$GameRoot  = 'C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source'
$Cstrike   = Join-Path $GameRoot 'cstrike'
$Scripting = Join-Path $Cstrike  'addons\sourcemod\scripting'
$Compiler  = Join-Path $Scripting 'spcomp64.exe'
$Include   = Join-Path $Scripting 'include'
$PluginDir = Join-Path $Cstrike  'addons\sourcemod\plugins'
$BuildDir  = Join-Path $Root     'build'

if (-not (Test-Path $Compiler)) { throw "SourcePawn compiler not found at $Compiler" }
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$sources = Get-ChildItem (Join-Path $Root 'plugin') -Filter *.sp
if (-not $sources) { throw 'No .sp sources found under plugin/' }

$failed = $false
foreach ($src in $sources) {
    $out = Join-Path $BuildDir ($src.BaseName + '.smx')
    Write-Host "==> compiling $($src.Name)" -ForegroundColor Cyan

    & $Compiler -i"$Include" -i"$Root\plugin\include" -o"$out" $src.FullName
    if ($LASTEXITCODE -ne 0) {
        Write-Host "    FAILED ($LASTEXITCODE)" -ForegroundColor Red
        $failed = $true
        continue
    }

    if (-not $NoDeploy) {
        Copy-Item $out (Join-Path $PluginDir ($src.BaseName + '.smx')) -Force
        Write-Host "    deployed -> plugins\$($src.BaseName).smx" -ForegroundColor Green
    }
}

if ($failed) { exit 1 }
Write-Host 'build ok' -ForegroundColor Green
