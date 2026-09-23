# Starts background processes where no window of theirs can ever be seen.
# srcds is a GUI program that opens its own console, so hiding its window at
# launch is not enough; it runs on a separate desktop that is never shown.

if (-not ('CsAI.Hidden' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

namespace CsAI {
public static class Hidden {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateDesktop(string name, IntPtr dev, IntPtr mode, int flags, uint access, IntPtr sa);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcess(string app, System.Text.StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit,
                                     uint flags, IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    const string DesktopName = "CsAI";
    const uint CREATE_NO_WINDOW = 0x08000000;
    const uint CREATE_NEW_PROCESS_GROUP = 0x00000200;
    static IntPtr desktop = IntPtr.Zero;

    public static int Start(string exe, string args, string cwd) {
        if (desktop == IntPtr.Zero) {
            desktop = CreateDesktop(DesktopName, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero);
            if (desktop == IntPtr.Zero)
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        }
        STARTUPINFO si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        si.lpDesktop = "WinSta0\\" + DesktopName;
        si.dwFlags = 1;          // STARTF_USESHOWWINDOW
        si.wShowWindow = 0;      // SW_HIDE
        PROCESS_INFORMATION pi;
        System.Text.StringBuilder cmd = new System.Text.StringBuilder("\"" + exe + "\" " + args);
        if (!CreateProcess(null, cmd, IntPtr.Zero, IntPtr.Zero, false,
                           CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, IntPtr.Zero, cwd, ref si, out pi))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return pi.dwProcessId;
    }
}
}
"@
}

function Join-Args([object[]]$ArgList) {
    ($ArgList | ForEach-Object {
        $a = [string]$_
        if ($a -eq '') { '""' }
        elseif ($a -match '[\s"]') { '"' + ($a -replace '"', '\"') + '"' }
        else { $a }
    }) -join ' '
}

function Start-Hidden([string]$FilePath, [object[]]$ArgList = @(), [string]$WorkingDirectory = $PWD.Path) {
    if (-not [IO.Path]::IsPathRooted($FilePath)) {
        $cmd = Get-Command $FilePath -CommandType Application -ErrorAction Stop | Select-Object -First 1
        $FilePath = $cmd.Source
    }
    $procId = [CsAI.Hidden]::Start($FilePath, (Join-Args $ArgList), $WorkingDirectory)
    return (Get-Process -Id $procId -ErrorAction SilentlyContinue)
}

function Start-HiddenPowerShell([string]$Script, [object[]]$ArgList = @(), [string]$WorkingDirectory = $PWD.Path) {
    $a = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $Script) + $ArgList
    return (Start-Hidden 'powershell.exe' $a $WorkingDirectory)
}

$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source'
$SrcdsLogDir = Join-Path $GameRoot 'cstrike\logs'

# Every server is LAN only and insecure, so it can never reach VAC.
function Start-Srcds([string]$LogName, [int]$Port, [object[]]$ArgList) {
    New-Item -ItemType Directory -Force -Path $SrcdsLogDir | Out-Null
    Remove-Item (Join-Path $SrcdsLogDir "$LogName.log") -ErrorAction SilentlyContinue
    $a = @('-console', '-game', 'cstrike', '-maxplayers', '6',
           '+sv_lan', '1', '-insecure', '-port', $Port,
           '+con_logfile', "logs/$LogName.log",
           '+servercfgfile', 'server_66.cfg') + $ArgList
    return (Start-Hidden (Join-Path $GameRoot 'srcds_win64.exe') $a $GameRoot)
}

function Get-SrcdsLog([string]$LogName) {
    $f = Join-Path $SrcdsLogDir "$LogName.log"
    if (-not (Test-Path $f)) { return '' }
    $fs = [IO.File]::Open($f, 'Open', 'Read', 'ReadWrite')
    try { return (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Close() }
}

# One server run to completion; returns its console output.
function Invoke-Srcds([string]$LogName, [int]$Port, [object[]]$ArgList, [int]$TimeoutSec = 600) {
    $p = Start-Srcds $LogName $Port $ArgList
    if ($p -and -not $p.WaitForExit($TimeoutSec * 1000)) {
        Write-Host '    timed out, killing' -ForegroundColor Yellow
        try { $p.Kill() } catch {}
        Start-Sleep -Seconds 1
    }
    return (Get-SrcdsLog $LogName)
}
