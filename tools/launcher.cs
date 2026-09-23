// Opens the control panel without a console window. Built by CsAI.bat.
using System;
using System.Diagnostics;
using System.IO;

static class Launcher
{
    [STAThread]
    static void Main()
    {
        string root = AppDomain.CurrentDomain.BaseDirectory;
        string panel = Path.Combine(Path.Combine(root, "tools"), "panel_gui.ps1");
        ProcessStartInfo psi = new ProcessStartInfo("powershell.exe",
            "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + panel + "\"");
        psi.UseShellExecute = false;
        psi.CreateNoWindow = true;
        psi.WorkingDirectory = root;
        Process.Start(psi);
    }
}
