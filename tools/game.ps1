# Where Counter-Strike: Source is installed, the same way tools/game.py finds it:
# CSAI_GAME, then data/game.txt, then the Steam libraries, then Steam's default folder.

# Text as game.py reads it: UTF-16 by its byte order mark (what PowerShell's '>'
# writes), otherwise UTF-8, and the ANSI code page only if that fails.
function Read-Text([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    if ($b.Length -ge 2 -and $b[0] -eq 0xFF -and $b[1] -eq 0xFE) { return [Text.Encoding]::Unicode.GetString($b, 2, $b.Length - 2) }
    if ($b.Length -ge 2 -and $b[0] -eq 0xFE -and $b[1] -eq 0xFF) { return [Text.Encoding]::BigEndianUnicode.GetString($b, 2, $b.Length - 2) }
    try { return (New-Object Text.UTF8Encoding($false, $true)).GetString($b) }
    catch { return [Text.Encoding]::Default.GetString($b) }
}

function Find-Game {
    $ok = { param($p) $p -and (Test-Path (Join-Path $p 'cstrike')) }
    if (& $ok $env:CSAI_GAME) { return $env:CSAI_GAME }
    $named = Join-Path (Split-Path -Parent $PSScriptRoot) 'data\game.txt'
    if (Test-Path $named) {
        $p = ([string](Read-Text $named)).Trim([char]0xFEFF + " `t`r`n`"")
        if (& $ok $p) { return $p }
    }
    $steams = @()
    foreach ($k in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        try {
            $v = Get-ItemProperty $k -ErrorAction Stop
            foreach ($n in 'SteamPath', 'InstallPath') { if ($v.$n) { $steams += ($v.$n -replace '/', '\') } }
        } catch {}
    }
    foreach ($s in $steams) {
        $libs = @($s)
        $vdf = Join-Path $s 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Read-Text $vdf), '"path"\s+"([^"]+)"')) {
                $libs += ($m.Groups[1].Value -replace '\\\\', '\')
            }
        }
        foreach ($l in $libs) {
            $p = Join-Path $l 'steamapps\common\Counter-Strike Source'
            if (& $ok $p) { return $p }
        }
    }
    return 'C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source'
}

$GameRoot = Find-Game
