param([switch]$NoAutoRefresh, [switch]$SelfTest)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$Root = Split-Path -Parent $PSScriptRoot
$Data = Join-Path $Root 'data'
$Logs = Join-Path $Root 'logs'
$PowerFile = Join-Path $Data 'power.txt'
$ArgsFile  = Join-Path $Data 'daemon_args.txt'

Add-Type -Namespace CsAI -Name Win -MemberDefinition @"
    public delegate bool EnumProc(System.IntPtr hWnd, System.IntPtr lParam);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumProc cb, System.IntPtr lParam);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(System.IntPtr hWnd, out uint pid);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    public static extern bool IsWindowVisible(System.IntPtr hWnd);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    public static extern bool ShowWindowAsync(System.IntPtr hWnd, int nCmdShow);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    public static extern bool ShowWindow(System.IntPtr hWnd, int nCmdShow);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(System.IntPtr hWnd);
"@

$script:WinByPid = @{}

function Update-WindowMap {
    $map = @{}
    $cb = [CsAI.Win+EnumProc] {
        param($h, $l)
        $p = [uint32]0
        [void][CsAI.Win]::GetWindowThreadProcessId($h, [ref]$p)
        if ($p -ne 0 -and -not $map.ContainsKey([int]$p)) { $map[[int]$p] = $h }
        return $true
    }
    [void][CsAI.Win]::EnumWindows($cb, [System.IntPtr]::Zero)
    $script:WinByPid = $map
}

function Show-ProcWindow([int]$procId, [bool]$show) {
    if (-not $script:WinByPid.ContainsKey($procId)) { return $false }
    $h = $script:WinByPid[$procId]
    if ($show) {
        [void][CsAI.Win]::ShowWindowAsync($h, 5)
        [void][CsAI.Win]::SetForegroundWindow($h)
    } else {
        [void][CsAI.Win]::ShowWindow($h, 0)
    }
    return $true
}

function Get-Instances {
    $rows = @()
    $procs = @{}
    foreach ($p in Get-Process -ErrorAction SilentlyContinue) { $procs[$p.Id] = $p }

    $wanted = "Name='powershell.exe' OR Name='python.exe' OR Name='srcds_win64.exe' OR Name='node.exe'"
    foreach ($ci in (Get-CimInstance Win32_Process -Filter $wanted -ErrorAction SilentlyContinue)) {
        $cl = $ci.CommandLine
        if (-not $cl) { continue }
        $role = $null
        $detail = ''
        if ($cl -match 'panel_gui\.ps1')      { $role = 'panel';      $detail = 'this window' }
        elseif ($cl -match '\+csai_actor') {
            $n = ([regex]::Match($cl, '\+csai_actor\s+(\d+)')).Groups[1].Value
            if ($n -eq '99') { $role = 'eval'; $detail = 'scoring run' }
            else { $role = ('actor ' + $n); $detail = 'collecting episodes' }
        }
        elseif ($cl -match 'daemon\.ps1' -and $cl -notmatch '-Stop') { $role = 'daemon'; $detail = 'supervisor' }
        elseif ($cl -match 'learn\.py')       { $role = 'learner';    $detail = 'ppo update' }
        elseif ($cl -match 'autofix\.py')     { $role = 'autofix';    $detail = 'plateau watch' }
        elseif ($cl -match 'curriculum\.sh')  { $role = 'curriculum'; $detail = 'wind-up steps' }
        elseif ($cl -match 'web.run\.ps1')    { $role = 'viewer';     $detail = 'replay site' }
        elseif ($ci.Name -eq 'node.exe')      { $role = 'viewer';     $detail = 'replay site' }
        if (-not $role) { continue }

        $pr = $procs[[int]$ci.ProcessId]
        $cpu = 0.0
        $mem = 0
        if ($pr) {
            $cpu = $pr.TotalProcessorTime.TotalSeconds
            $mem = [math]::Round($pr.WorkingSet64 / 1MB)
        }
        $rows += [pscustomobject]@{
            Role = $role; ProcId = [int]$ci.ProcessId; CpuSec = $cpu; Mem = $mem; Detail = $detail
        }
    }
    $order = { if ($_.Role -like 'actor*') { 1 } elseif ($_.Role -eq 'eval') { 2 } else { 0 } }
    return $rows | Sort-Object @{ Expression = $order }, Role
}

function Get-Power {
    if (Test-Path $PowerFile) {
        $v = (Get-Content $PowerFile -Raw -ErrorAction SilentlyContinue)
        if ($v) { return $v.Trim([char]0xFEFF + " `t`r`n").ToLower() }
    }
    return 'high'
}

function Get-Progress {
    $log = Join-Path $Data 'train_log.csv'
    if (-not (Test-Path $log)) { return $null }
    try {
        # Columns by name. The log has gained columns twice; fixed offsets would
        # read the wrong ones without any sign of it.
        $head = (Get-Content $log -TotalCount 1) -split ','
        $iGen = [array]::IndexOf($head, 'gen')
        $iRun = [array]::IndexOf($head, 'runs_from_start')
        $iFin = [array]::IndexOf($head, 'finished_from_start')
        $iMed = [array]::IndexOf($head, 'median_full_run_s')
        if ($iGen -lt 0 -or $iRun -lt 0 -or $iFin -lt 0 -or $iMed -lt 0) { return $null }
        $need = (@($iGen, $iRun, $iFin, $iMed) | Measure-Object -Maximum).Maximum + 1

        $tail = @(Get-Content $log -Tail 40 -ErrorAction SilentlyContinue)
        if ($tail.Count -lt 2) { return $null }
        $runs = 0.0; $fin = 0.0; $med = 0.0; $medN = 0; $gen = ''
        foreach ($line in $tail) {
            $c = $line.Split(',')
            if ($c.Count -lt $need) { continue }
            if ($c[$iRun] -eq '') { continue }
            $gen = $c[$iGen]
            $runs += [double]$c[$iRun]; $fin += [double]$c[$iFin]
            if ([double]$c[$iMed] -gt 0) { $med += [double]$c[$iMed]; $medN++ }
        }
        $rate = 0.0
        if ($runs -gt 0) { $rate = 100.0 * $fin / $runs }
        $mt = 0.0
        if ($medN -gt 0) { $mt = $med / $medN }
        return [pscustomobject]@{ Gen = $gen; Rate = $rate; Median = $mt }
    } catch { return $null }
}

$form = New-Object System.Windows.Forms.Form
$form.Text = 'CsAI'
$form.Size = New-Object System.Drawing.Size(940, 620)
$form.StartPosition = 'CenterScreen'
$form.MinimumSize = New-Object System.Drawing.Size(780, 500)

$status = New-Object System.Windows.Forms.Label
$status.Location = New-Object System.Drawing.Point(12, 10)
$status.Size = New-Object System.Drawing.Size(900, 40)
$status.Font = New-Object System.Drawing.Font('Segoe UI', 10)
$status.Anchor = 'Top,Left,Right'
$form.Controls.Add($status)

$list = New-Object System.Windows.Forms.ListView
$list.Location = New-Object System.Drawing.Point(12, 56)
$list.Size = New-Object System.Drawing.Size(660, 296)
$list.View = 'Details'
$list.FullRowSelect = $true
$list.HideSelection = $false
$list.Anchor = 'Top,Bottom,Left,Right'
[void]$list.Columns.Add('what', 110)
[void]$list.Columns.Add('pid', 60)
[void]$list.Columns.Add('cpu', 60)
[void]$list.Columns.Add('memory', 70)
[void]$list.Columns.Add('window', 70)
[void]$list.Columns.Add('doing', 260)
$form.Controls.Add($list)

$hint = New-Object System.Windows.Forms.Label
$hint.Text = 'double-click a row to show or hide that window'
$hint.Location = New-Object System.Drawing.Point(14, 356)
$hint.Size = New-Object System.Drawing.Size(400, 20)
$hint.ForeColor = [System.Drawing.Color]::Gray
$hint.Anchor = 'Bottom,Left'
$form.Controls.Add($hint)

function New-PanelButton($text, $x, $y, $w, $h) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text
    $b.Location = New-Object System.Drawing.Point($x, $y)
    $b.Size = New-Object System.Drawing.Size($w, $h)
    $b.Anchor = 'Top,Right'
    $form.Controls.Add($b)
    return $b
}

$bStart   = New-PanelButton 'start training' 686 56  110 30
$bStop    = New-PanelButton 'stop training'  802 56  110 30

$lblPow = New-Object System.Windows.Forms.Label
$lblPow.Text = 'power'
$lblPow.Location = New-Object System.Drawing.Point(686, 96)
$lblPow.Size = New-Object System.Drawing.Size(45, 22)
$lblPow.Anchor = 'Top,Right'
$form.Controls.Add($lblPow)

$cmbPow = New-Object System.Windows.Forms.ComboBox
$cmbPow.Location = New-Object System.Drawing.Point(733, 92)
$cmbPow.Size = New-Object System.Drawing.Size(179, 24)
$cmbPow.DropDownStyle = 'DropDownList'
$cmbPow.Anchor = 'Top,Right'
foreach ($l in @('idle', 'low', 'medium', 'high', 'max')) { [void]$cmbPow.Items.Add($l) }
$form.Controls.Add($cmbPow)

$bShow    = New-PanelButton 'show window'   686 128 110 30
$bHide    = New-PanelButton 'hide window'   802 128 110 30
$bHideAll = New-PanelButton 'hide them all' 686 164 226 28
$bViewer  = New-PanelButton 'replay viewer' 686 200 110 30
$bReport  = New-PanelButton 'report'        802 200 110 30
$bFolder  = New-PanelButton 'open folder'   686 236 226 28

$logPick = New-Object System.Windows.Forms.ComboBox
$logPick.Location = New-Object System.Drawing.Point(12, 378)
$logPick.Size = New-Object System.Drawing.Size(200, 24)
$logPick.DropDownStyle = 'DropDownList'
$logPick.Anchor = 'Bottom,Left'
foreach ($n in @('daemon.log', 'learner.log', 'learner.err.log', 'autofix.log')) { [void]$logPick.Items.Add($n) }
$logPick.SelectedIndex = 0
$form.Controls.Add($logPick)

$logBox = New-Object System.Windows.Forms.TextBox
$logBox.Location = New-Object System.Drawing.Point(12, 406)
$logBox.Size = New-Object System.Drawing.Size(900, 166)
$logBox.Multiline = $true
$logBox.ReadOnly = $true
$logBox.ScrollBars = 'Vertical'
$logBox.Font = New-Object System.Drawing.Font('Consolas', 9)
$logBox.Anchor = 'Bottom,Left,Right'
$form.Controls.Add($logBox)

function Get-DaemonArgs {
    # Kept in a file rather than baked in here, so the panel starts training with
    # whatever settings the run is actually using instead of a stale copy.
    if (Test-Path $ArgsFile) {
        $line = (Get-Content $ArgsFile -Raw -ErrorAction SilentlyContinue)
        if ($line) {
            $t = $line.Trim()
            if ($t) { return @($t.Split(' ') | Where-Object { $_ -ne '' }) }
        }
    }
    return @('-Map', 'surf_demise')
}

function Get-SelectedPid {
    if ($list.SelectedItems.Count -eq 0) { return -1 }
    return [int]$list.SelectedItems[0].SubItems[1].Text
}

$bStart.Add_Click({
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Root 'tools\daemon.ps1')) + (Get-DaemonArgs)
    Start-Process -FilePath 'powershell' -ArgumentList $a -WorkingDirectory $Root -WindowStyle Hidden | Out-Null
})

$bStop.Add_Click({
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Root 'tools\daemon.ps1'), '-Stop')
    Start-Process -FilePath 'powershell' -ArgumentList $a -WorkingDirectory $Root -WindowStyle Hidden | Out-Null
})

$cmbPow.Add_SelectedIndexChanged({
    if ($cmbPow.SelectedItem) {
        # The daemon re-reads this file every few seconds, so changing the level
        # needs nothing restarted and nothing trained is lost.
        Set-Content -Path $PowerFile -Value $cmbPow.SelectedItem -Encoding ascii
    }
})

$bShow.Add_Click({
    $p = Get-SelectedPid
    if ($p -gt 0) { Update-WindowMap; [void](Show-ProcWindow $p $true) }
})

$bHide.Add_Click({
    $p = Get-SelectedPid
    if ($p -gt 0) { Update-WindowMap; [void](Show-ProcWindow $p $false) }
})

$bHideAll.Add_Click({
    Update-WindowMap
    foreach ($i in $list.Items) {
        if ($i.SubItems[0].Text -ne 'panel') { [void](Show-ProcWindow ([int]$i.SubItems[1].Text) $false) }
    }
})

$list.Add_DoubleClick({
    $p = Get-SelectedPid
    if ($p -le 0) { return }
    Update-WindowMap
    $vis = $false
    if ($script:WinByPid.ContainsKey($p)) { $vis = [CsAI.Win]::IsWindowVisible($script:WinByPid[$p]) }
    [void](Show-ProcWindow $p (-not $vis))
})

$bViewer.Add_Click({
    Start-Process -FilePath 'powershell' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', (Join-Path $Root 'web\run.ps1')) -WorkingDirectory $Root -WindowStyle Hidden | Out-Null
    Start-Sleep -Milliseconds 1200
    Start-Process 'http://127.0.0.1:3000'
})

$bReport.Add_Click({
    Start-Process -FilePath 'python' -ArgumentList @((Join-Path $Root 'tools\report.py')) `
        -WorkingDirectory $Root -WindowStyle Hidden -Wait
    $r = Join-Path $Root 'reports\latest.md'
    if (Test-Path $r) { Start-Process $r }
})

$bFolder.Add_Click({ Start-Process $Root })

$script:PrevCpu = @{}
$script:PrevAt = Get-Date

$script:Tidied = @{}

function Update-Panel {
    Update-WindowMap
    $now = Get-Date
    $dt = ($now - $script:PrevAt).TotalSeconds
    if ($dt -le 0) { $dt = 1 }

    $rows = @(Get-Instances)
    $keep = @{}
    foreach ($i in $list.SelectedItems) { $keep[[int]$i.SubItems[1].Text] = $true }

    $list.BeginUpdate()
    $list.Items.Clear()
    foreach ($r in $rows) {
        $pct = 0.0
        if ($script:PrevCpu.ContainsKey($r.ProcId)) {
            $pct = 100.0 * ($r.CpuSec - $script:PrevCpu[$r.ProcId]) / $dt
            if ($pct -lt 0) { $pct = 0 }
        }
        $win = 'none'
        if ($script:WinByPid.ContainsKey($r.ProcId)) {
            if (-not $script:Tidied.ContainsKey($r.ProcId)) {
                $script:Tidied[$r.ProcId] = $true
                if ($r.Role -ne 'panel') { [void](Show-ProcWindow $r.ProcId $false) }
            }
            if ([CsAI.Win]::IsWindowVisible($script:WinByPid[$r.ProcId])) { $win = 'shown' } else { $win = 'hidden' }
        }
        $it = New-Object System.Windows.Forms.ListViewItem($r.Role)
        [void]$it.SubItems.Add([string]$r.ProcId)
        [void]$it.SubItems.Add(('{0:N0}%' -f $pct))
        [void]$it.SubItems.Add(('{0} MB' -f $r.Mem))
        [void]$it.SubItems.Add($win)
        [void]$it.SubItems.Add($r.Detail)
        if ($keep.ContainsKey($r.ProcId)) { $it.Selected = $true }
        [void]$list.Items.Add($it)
    }
    $list.EndUpdate()

    $c = @{}
    foreach ($r in $rows) { $c[$r.ProcId] = $r.CpuSec }
    $script:PrevCpu = $c
    $live = @{}
    foreach ($r in $rows) { $live[$r.ProcId] = $true }
    foreach ($k in @($script:Tidied.Keys)) { if (-not $live.ContainsKey($k)) { $script:Tidied.Remove($k) } }
    $script:PrevAt = $now

    $actors = @($rows | Where-Object { $_.Role -like 'actor*' }).Count
    $running = @($rows | Where-Object { $_.Role -eq 'daemon' }).Count -gt 0
    $head = 'training: '
    if ($running) { $head += 'running' } else { $head += 'stopped' }
    $head += ('   power: ' + (Get-Power) + '   game servers: ' + $actors)
    $pg = Get-Progress
    if ($pg) {
        $head += ("`r`ngeneration " + $pg.Gen +
                  ('   finishing {0:N0}% of runs' -f $pg.Rate) +
                  ('   median run {0:N2}s' -f $pg.Median) +
                  '   the time to beat is 39.05s')
    }
    $status.Text = $head

    $lf = Join-Path $Logs ([string]$logPick.SelectedItem)
    if (Test-Path $lf) {
        try {
            $t = (@(Get-Content $lf -Tail 60 -ErrorAction SilentlyContinue)) -join "`r`n"
            if ($logBox.Text -ne $t) {
                $logBox.Text = $t
                $logBox.SelectionStart = $logBox.Text.Length
                $logBox.ScrollToCaret()
            }
        } catch {}
    } else {
        $logBox.Text = 'nothing written to ' + $logPick.SelectedItem + ' yet'
    }
}

$cmbPow.SelectedItem = (Get-Power)

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 2000
$timer.Add_Tick({ try { Update-Panel } catch {} })
if (-not $NoAutoRefresh) { $timer.Start() }

if ($SelfTest) {
    # Everything except showing the form, so the parts that read the machine can
    # be checked without a window in the way.
    Update-Panel
    Write-Host $status.Text
    Write-Host ''
    foreach ($i in $list.Items) {
        Write-Host ('  {0,-12} {1,-7} {2,-6} {3,-8} {4,-7} {5}' -f
            $i.SubItems[0].Text, $i.SubItems[1].Text, $i.SubItems[2].Text,
            $i.SubItems[3].Text, $i.SubItems[4].Text, $i.SubItems[5].Text)
    }
    Write-Host ''
    Write-Host ('log tail: ' + $logBox.Text.Length + ' characters from ' + $logPick.SelectedItem)
    exit 0
}

$form.Add_Shown({ try { Update-Panel } catch {} })
$form.Add_FormClosed({ $timer.Stop() })
[void]$form.ShowDialog()
