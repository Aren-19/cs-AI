param([switch]$SelfTest,
      [string]$Start = '')   # comma separated slots to start on launch

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()
. (Join-Path $PSScriptRoot 'hidden.ps1')

$Root = Split-Path -Parent $PSScriptRoot
$Data = Join-Path $Root 'data'
$Logs = Join-Path $Root 'logs'
$Daemon = Join-Path $Root 'tools\daemon.ps1'

function Sfx([string]$slot) { if ($slot -eq 'main') { '' } else { "_$slot" } }

function Get-RealSlots {
    # main, plus one slot per data/daemon_args_<slot>.txt
    $out = @('main')
    Get-ChildItem (Join-Path $Data 'daemon_args_*.txt') -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.Name -match '^daemon_args_(.+)\.txt$') { $out += $Matches[1] }
    }
    return $out
}

function Get-SlotOf([string]$cl) {
    if ($cl -match '\+csai_slot\s+(\S+)') { return $Matches[1] }
    if ($cl -match '-Slot\s+(\S+)') { return $Matches[1] }
    if ($cl -match 'out_([A-Za-z0-9]+)') { return $Matches[1] }
    return 'main'
}

function Get-Instances {
    $rows = @()
    $procs = @{}
    foreach ($p in Get-Process -ErrorAction SilentlyContinue) { $procs[$p.Id] = $p }

    $wanted = "Name='powershell.exe' OR Name='python.exe' OR Name='srcds_win64.exe' OR Name='node.exe'"
    foreach ($ci in (Get-CimInstance Win32_Process -Filter $wanted -ErrorAction SilentlyContinue)) {
        $cl = $ci.CommandLine
        if (-not $cl) { continue }
        $slot = Get-SlotOf $cl
        $role = $null; $detail = ''; $log = ''
        if ($cl -match 'panel_gui\.ps1') { continue }
        elseif ($cl -match '\+csai_actor\s+(\d+)') {
            $n = $Matches[1]
            if ($n -eq '99') { $role = 'eval'; $detail = 'scoring the policy'; $log = "srcds:csai_${slot}_eval" }
            else { $role = "server $n"; $detail = 'playing episodes'; $log = "srcds:csai_${slot}_a$n" }
        }
        elseif ($ci.Name -eq 'srcds_win64.exe') { $role = 'server'; $detail = 'other'; $slot = '-' }
        elseif ($cl -match 'daemon\.ps1') {
            if ($cl -match '-Stop') { $role = 'stopping'; $detail = 'shutting the slot down' }
            else { $role = 'supervisor'; $detail = 'keeps the slot running' }
            $log = "file:daemon$(Sfx $slot).log"
        }
        elseif ($cl -match 'eval\.ps1')   { $role = 'eval run';  $detail = 'waiting for the eval server'; $log = "file:eval_$slot.log" }
        elseif ($cl -match 'learn\.py')   { $role = 'learner';   $detail = 'updating the policy'; $log = "file:learner$(Sfx $slot).log" }
        elseif ($cl -match 'report\.py')  { $role = 'report';    $detail = 'writing the report' }
        elseif ($cl -match 'webserve\.py' -or $cl -match 'web.run\.ps1' -or ($ci.Name -eq 'node.exe' -and $cl -match 'vite')) {
            $role = 'viewer'; $detail = 'replay site'; $slot = '-'
        }
        if (-not $role) { continue }

        $pr = $procs[[int]$ci.ProcessId]
        $cpu = 0.0; $mem = 0
        if ($pr) {
            try { $cpu = $pr.TotalProcessorTime.TotalSeconds } catch {}
            $mem = [math]::Round($pr.WorkingSet64 / 1MB)
        }
        $rows += [pscustomobject]@{
            Role = $role; ProcId = [int]$ci.ProcessId; CpuSec = $cpu; Mem = $mem
            Detail = $detail; SlotName = $slot; Log = $log
        }
    }
    $order = { if ($_.Role -like 'server*') { 2 } elseif ($_.Role -like 'eval*') { 3 } elseif ($_.Role -eq 'viewer') { 4 } else { 1 } }
    return @($rows | Sort-Object SlotName, @{ Expression = $order }, Role)
}

function Get-Power([string]$slot) {
    $pf = Join-Path $Data "power$(Sfx $slot).txt"
    if (Test-Path $pf) {
        $v = Get-Content $pf -Raw -ErrorAction SilentlyContinue
        if ($v) { return $v.Trim([char]0xFEFF + " `t`r`n").ToLower() }
    }
    return 'high'
}

function Get-SlotMaps([string]$slot) {
    $file = Join-Path $Data "daemon_args$(Sfx $slot).txt"
    $maps = @()
    if (Test-Path $file) {
        $t = @((Get-Content $file -Raw).Trim() -split '\s+')
        $i = [array]::IndexOf($t, '-Map')
        if ($i -ge 0 -and $i + 1 -lt $t.Count) { $maps = @($t[$i + 1] -split ',' | Where-Object { $_ }) }
    }
    if (-not $maps) { $maps = @('surf_demise') }
    return $maps
}

function Get-Best([string]$slot, [string]$map, [bool]$first) {
    $tag = if ($first) { Sfx $slot } else { "$(Sfx $slot).$map" }
    $f = Join-Path $Data "best$tag.txt"
    if (-not (Test-Path $f)) { return '' }
    $v = (Get-Content $f -Raw).Trim() -split '\s+'
    if ($v.Count -lt 4 -or [int]$v[1] -eq 0) { return '' }
    return (', best {0}/{1} at {2:N2}s' -f $v[1], $v[2], [double]$v[3])
}

# One summary per map: finish rate and median from the start, or the mean score.
function Get-Progress([string]$slot) {
    $maps = Get-SlotMaps $slot
    $log = Join-Path $Data "train_log$(Sfx $slot).csv"
    $out = ''
    $stats = @{}
    if (Test-Path $log) {
        try {
            $head = (Get-Content $log -TotalCount 1) -split ','
            $iGen = [array]::IndexOf($head, 'gen')
            $iRun = [array]::IndexOf($head, 'runs_from_start')
            $iFin = [array]::IndexOf($head, 'finished_from_start')
            $iMed = [array]::IndexOf($head, 'median_full_run_s')
            $iRet = [array]::IndexOf($head, 'mean_return')
            $iMap = [array]::IndexOf($head, 'map')
            $tail = @(Get-Content $log -Tail 80 -ErrorAction SilentlyContinue | Where-Object { $_ -notmatch '^gen,' })
            if ($iGen -ge 0 -and $tail.Count) { $out = 'gen ' + ($tail[-1] -split ',')[$iGen] }
            foreach ($line in $tail) {
                $c = $line.Split(',')
                $m = if ($iMap -ge 0 -and $c.Count -gt $iMap -and $c[$iMap]) { $c[$iMap] } else { $maps[0] }
                if (-not $stats.ContainsKey($m)) { $stats[$m] = @{ Runs = 0.0; Fin = 0.0; Med = 0.0; MedN = 0; Ret = 0.0; RetN = 0 } }
                $s = $stats[$m]
                if ($iRet -ge 0 -and $c.Count -gt $iRet -and $c[$iRet]) { $s.Ret += [double]$c[$iRet]; $s.RetN++ }
                if ($iRun -lt 0 -or $iMed -lt 0 -or $c.Count -le $iMed -or $c[$iRun] -eq '') { continue }
                $s.Runs += [double]$c[$iRun]; $s.Fin += [double]$c[$iFin]
                if ([double]$c[$iMed] -gt 0) { $s.Med += [double]$c[$iMed]; $s.MedN++ }
            }
        } catch {}
    }
    for ($k = 0; $k -lt $maps.Count; $k++) {
        $m = $maps[$k]
        $part = ($m -replace '^surf_', '') + ': '
        $s = $stats[$m]
        if ($s -and $s.Runs -gt 0 -and $s.Fin -gt 0) {
            $part += ('finishing {0:N0}%' -f (100.0 * $s.Fin / $s.Runs))
            if ($s.MedN) { $part += (', median {0:N2}s' -f ($s.Med / $s.MedN)) }
        } elseif ($s -and $s.RetN) {
            $part += ('mean score {0:N1}' -f ($s.Ret / $s.RetN))
        } else {
            $part += 'no runs yet'
        }
        $part += (Get-Best $slot $m ($k -eq 0))
        $out += '   |   ' + $part
    }
    return $out
}

function Read-Tail([string]$path, [int]$n = 60) {
    if (-not (Test-Path $path)) { return $null }
    try {
        $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
        try {
            if ($fs.Length -gt 65536) { [void]$fs.Seek(-65536, 'End') }
            $t = (New-Object IO.StreamReader($fs)).ReadToEnd()
        } finally { $fs.Close() }
        $lines = @($t -split "`r?`n" | Where-Object { $_ -ne '' })
        if ($lines.Count -gt $n) { $lines = $lines[-$n..-1] }
        return ($lines -join "`r`n")
    } catch { return $null }
}

function Resolve-Log([string]$key) {
    if ($key -like 'srcds:*') { return (Join-Path $SrcdsLogDir ($key.Substring(6) + '.log')) }
    if ($key -like 'file:*')  { return (Join-Path $Logs $key.Substring(5)) }
    return $null
}

# Background jobs the panel started and is waiting on, so nothing blocks the window.
$script:Pending = @()

function Start-Slot([string]$slot) {
    $file = Join-Path $Data "daemon_args$(Sfx $slot).txt"
    $sa = @('-Map', 'surf_demise')
    if (Test-Path $file) {
        $line = Get-Content $file -Raw -ErrorAction SilentlyContinue
        if ($line -and $line.Trim()) { $sa = @($line.Trim() -split '\s+') }
    }
    if ($slot -ne 'main' -and -not ($sa -contains '-Slot')) { $sa += @('-Slot', $slot) }
    $p = Start-HiddenPowerShell $Daemon $sa $Root
    if ($p) { $script:Pending += [pscustomobject]@{ Proc = $p; What = "starting $slot"; Until = (Get-Date).AddSeconds(20); Open = $null } }
}

function Stop-Slot([string]$slot) {
    $a = @('-Stop')
    if ($slot -ne 'main') { $a += @('-Slot', $slot) }
    $p = Start-HiddenPowerShell $Daemon $a $Root
    if ($p) { $script:Pending += [pscustomobject]@{ Proc = $p; What = "stopping $slot"; Until = $null; Open = $null } }
}

function Get-Busy {
    $now = Get-Date
    $left = @()
    foreach ($j in $script:Pending) {
        $done = if ($j.Until) { $now -ge $j.Until } else { $j.Proc.HasExited }
        if (-not $done) { $left += $j; continue }
        if ($j.Open -and ($j.Open -like 'http*' -or (Test-Path $j.Open))) { Start-Process $j.Open }
    }
    $script:Pending = $left
    return @($left | ForEach-Object { $_.What })
}

# ------------------------------------------------------------------- window ----

$form = New-Object System.Windows.Forms.Form
$form.Text = 'CsAI'
$form.Size = New-Object System.Drawing.Size(960, 640)
$form.StartPosition = 'CenterScreen'
$form.MinimumSize = New-Object System.Drawing.Size(800, 520)

$status = New-Object System.Windows.Forms.Label
$status.Location = New-Object System.Drawing.Point(12, 10)
$status.Size = New-Object System.Drawing.Size(920, 90)
$status.Font = New-Object System.Drawing.Font('Segoe UI', 10)
$status.Anchor = 'Top,Left,Right'
$form.Controls.Add($status)

$list = New-Object System.Windows.Forms.ListView
$list.Location = New-Object System.Drawing.Point(12, 104)
$list.Size = New-Object System.Drawing.Size(680, 258)
$list.View = 'Details'
$list.FullRowSelect = $true
$list.HideSelection = $false
$list.MultiSelect = $false
$list.Anchor = 'Top,Bottom,Left,Right'
[void]$list.Columns.Add('what', 100)
[void]$list.Columns.Add('slot', 70)
[void]$list.Columns.Add('pid', 60)
[void]$list.Columns.Add('cpu', 60)
[void]$list.Columns.Add('memory', 70)
[void]$list.Columns.Add('doing', 300)
$form.Controls.Add($list)

function New-PanelButton($text, $x, $y, $w, $h) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text
    $b.Location = New-Object System.Drawing.Point($x, $y)
    $b.Size = New-Object System.Drawing.Size($w, $h)
    $b.Anchor = 'Top,Right'
    $form.Controls.Add($b)
    return $b
}

function New-PanelLabel($text, $x, $y, $w) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text
    $l.Location = New-Object System.Drawing.Point($x, ($y + 4))
    $l.Size = New-Object System.Drawing.Size($w, 20)
    $l.Anchor = 'Top,Right'
    $form.Controls.Add($l)
    return $l
}

[void](New-PanelLabel 'slot' 706 104 45)
$cmbSlot = New-Object System.Windows.Forms.ComboBox
$cmbSlot.Location = New-Object System.Drawing.Point(753, 104)
$cmbSlot.Size = New-Object System.Drawing.Size(179, 24)
$cmbSlot.DropDownStyle = 'DropDownList'
$cmbSlot.Anchor = 'Top,Right'
$form.Controls.Add($cmbSlot)

$bStart = New-PanelButton 'start' 706 136 110 32
$bStop  = New-PanelButton 'stop'  822 136 110 32

[void](New-PanelLabel 'power' 706 178 45)
$cmbPow = New-Object System.Windows.Forms.ComboBox
$cmbPow.Location = New-Object System.Drawing.Point(753, 178)
$cmbPow.Size = New-Object System.Drawing.Size(179, 24)
$cmbPow.DropDownStyle = 'DropDownList'
$cmbPow.Anchor = 'Top,Right'
foreach ($l in @('idle', 'low', 'medium', 'high', 'max')) { [void]$cmbPow.Items.Add($l) }
$form.Controls.Add($cmbPow)

$bViewer = New-PanelButton 'replay viewer' 706 218 110 30
$bReport = New-PanelButton 'report'        822 218 110 30
$bFolder = New-PanelButton 'open folder'   706 254 226 28

$hint = New-Object System.Windows.Forms.Label
$hint.Text = 'pick a row to see its log'
$hint.Location = New-Object System.Drawing.Point(14, 368)
$hint.Size = New-Object System.Drawing.Size(200, 20)
$hint.ForeColor = [System.Drawing.Color]::Gray
$hint.Anchor = 'Bottom,Left'
$form.Controls.Add($hint)

$logPick = New-Object System.Windows.Forms.ComboBox
$logPick.Location = New-Object System.Drawing.Point(220, 366)
$logPick.Size = New-Object System.Drawing.Size(260, 24)
$logPick.DropDownStyle = 'DropDownList'
$logPick.Anchor = 'Bottom,Left'
$form.Controls.Add($logPick)

$logBox = New-Object System.Windows.Forms.TextBox
$logBox.Location = New-Object System.Drawing.Point(12, 396)
$logBox.Size = New-Object System.Drawing.Size(920, 190)
$logBox.Multiline = $true
$logBox.ReadOnly = $true
$logBox.WordWrap = $false
$logBox.ScrollBars = 'Both'
$logBox.Font = New-Object System.Drawing.Font('Consolas', 9)
$logBox.Anchor = 'Bottom,Left,Right'
$form.Controls.Add($logBox)

function Selected-Slot { if ($cmbSlot.SelectedItem) { [string]$cmbSlot.SelectedItem } else { 'all' } }
function Slots-ToActOn { $s = Selected-Slot; if ($s -eq 'all') { Get-RealSlots } else { @($s) } }

function Update-LogChoices([object[]]$rows) {
    $keys = New-Object System.Collections.Generic.List[string]
    foreach ($sl in (Get-RealSlots)) {
        foreach ($k in @("file:daemon$(Sfx $sl).log", "file:learner$(Sfx $sl).log", "file:eval_$sl.log")) { $keys.Add($k) }
    }
    foreach ($r in $rows) { if ($r.Log -and -not $keys.Contains($r.Log)) { $keys.Add($r.Log) } }
    $cur = [string]$logPick.SelectedItem
    $have = @($logPick.Items | ForEach-Object { [string]$_ })
    if (($have -join '|') -ne ($keys -join '|')) {
        $logPick.BeginUpdate()
        $logPick.Items.Clear()
        foreach ($k in $keys) { [void]$logPick.Items.Add($k) }
        $logPick.EndUpdate()
        $i = $logPick.Items.IndexOf($cur)
        $logPick.SelectedIndex = if ($i -ge 0) { $i } else { 0 }
    }
}

function Update-LogBox {
    $key = [string]$logPick.SelectedItem
    $path = Resolve-Log $key
    $t = if ($path) { Read-Tail $path } else { $null }
    if ($null -eq $t) { $t = "nothing written to $key yet" }
    if ($logBox.Text -ne $t) {
        $logBox.Text = $t
        $logBox.SelectionStart = $logBox.Text.Length
        $logBox.ScrollToCaret()
    }
}

$script:PrevCpu = @{}
$script:PrevAt = Get-Date
$script:Rows = @()
$script:SlotSwitching = $false
$script:Refreshing = $false

function Update-Panel {
    $now = Get-Date
    $dt = ($now - $script:PrevAt).TotalSeconds
    if ($dt -le 0) { $dt = 1 }

    $rows = Get-Instances
    $script:Rows = $rows
    $keep = -1
    if ($list.SelectedItems.Count) { $keep = [int]$list.SelectedItems[0].SubItems[2].Text }

    $script:Refreshing = $true
    $list.BeginUpdate()
    $list.Items.Clear()
    foreach ($r in $rows) {
        $pct = 0.0
        if ($script:PrevCpu.ContainsKey($r.ProcId)) {
            $pct = [math]::Max(0.0, 100.0 * ($r.CpuSec - $script:PrevCpu[$r.ProcId]) / $dt)
        }
        $it = New-Object System.Windows.Forms.ListViewItem($r.Role)
        [void]$it.SubItems.Add($r.SlotName)
        [void]$it.SubItems.Add([string]$r.ProcId)
        [void]$it.SubItems.Add(('{0:N0}%' -f $pct))
        [void]$it.SubItems.Add(('{0} MB' -f $r.Mem))
        [void]$it.SubItems.Add($r.Detail)
        $it.Tag = $r.Log
        if ($r.ProcId -eq $keep) { $it.Selected = $true }
        [void]$list.Items.Add($it)
    }
    $list.EndUpdate()
    $script:Refreshing = $false

    $c = @{}
    foreach ($r in $rows) { $c[$r.ProcId] = $r.CpuSec }
    $script:PrevCpu = $c
    $script:PrevAt = $now

    $lines = @()
    foreach ($sl in (Get-RealSlots)) {
        $sup = @($rows | Where-Object { $_.Role -eq 'supervisor' -and $_.SlotName -eq $sl }).Count
        $srv = @($rows | Where-Object { $_.Role -like 'server *' -and $_.SlotName -eq $sl }).Count
        $state = if ($sup) { "running, $srv server(s)" }
                 elseif ($srv) { "NOT SUPERVISED, $srv server(s) left - press stop" }
                 else { 'stopped' }
        $line = "{0}: {1}   power {2}" -f $sl, $state, (Get-Power $sl)
        $pg = Get-Progress $sl
        if ($pg) { $line += "`r`n      " + ($pg -replace '^gen (\d+)   \|   ', 'gen $1:   ') }
        $lines += $line
    }
    $busy = Get-Busy
    if ($busy.Count) { $lines += ($busy -join ', ') + '...' }
    $status.Text = ($lines -join "`r`n")
    $bStart.Enabled = ($busy.Count -eq 0)
    $bStop.Enabled = -not ($busy | Where-Object { $_ -like 'stopping*' })

    Update-LogChoices $rows
    Update-LogBox
}

$bStart.Add_Click({
    foreach ($sl in (Slots-ToActOn)) { Start-Slot $sl }
    Update-Panel
})

$bStop.Add_Click({
    foreach ($sl in (Slots-ToActOn)) { Stop-Slot $sl }
    Update-Panel
})

$cmbPow.Add_SelectedIndexChanged({
    if ($cmbPow.SelectedItem -and -not $script:SlotSwitching) {
        # The supervisor reads this every few seconds; nothing restarts.
        foreach ($sl in (Slots-ToActOn)) {
            Set-Content -Path (Join-Path $Data "power$(Sfx $sl).txt") -Value $cmbPow.SelectedItem -Encoding ascii
        }
        Update-Panel
    }
})

$cmbSlot.Add_SelectedIndexChanged({
    $script:SlotSwitching = $true
    $s = Selected-Slot
    $cmbPow.SelectedItem = Get-Power $(if ($s -eq 'all') { 'main' } else { $s })
    $script:SlotSwitching = $false
})

$list.Add_SelectedIndexChanged({
    if (-not $script:Refreshing -and $list.SelectedItems.Count -and $list.SelectedItems[0].Tag) {
        $i = $logPick.Items.IndexOf([string]$list.SelectedItems[0].Tag)
        if ($i -ge 0 -and $logPick.SelectedIndex -ne $i) { $logPick.SelectedIndex = $i }
    }
})

$logPick.Add_SelectedIndexChanged({ Update-LogBox })

$bViewer.Add_Click({
    # Opened from here: a browser started by a hidden process would be hidden too.
    $p = Start-HiddenPowerShell (Join-Path $Root 'web\run.ps1') @('-NoBrowser') $Root
    if ($p) {
        $script:Pending += [pscustomobject]@{ Proc = $p; What = 'starting the viewer'; Until = $null
                                              Open = 'http://127.0.0.1:3000/' }
    }
})

$bReport.Add_Click({
    $p = Start-Hidden 'python.exe' @((Join-Path $Root 'tools\report.py')) $Root
    if ($p) {
        $script:Pending += [pscustomobject]@{ Proc = $p; What = 'writing the report'; Until = $null
                                              Open = (Join-Path $Root 'reports\latest.md') }
    }
})

$bFolder.Add_Click({ Start-Process $Root })

foreach ($sl in (@('all') + (Get-RealSlots))) { [void]$cmbSlot.Items.Add($sl) }
$script:SlotSwitching = $true
$cmbSlot.SelectedIndex = 0
$cmbPow.SelectedItem = Get-Power 'main'
$script:SlotSwitching = $false

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 2000
$timer.Add_Tick({ try { Update-Panel } catch {} })

if ($SelfTest) {
    Update-Panel
    Write-Host $status.Text
    Write-Host ''
    foreach ($i in $list.Items) {
        Write-Host ('  {0,-12} {1,-8} {2,-7} {3,-6} {4,-8} {5}' -f
            $i.SubItems[0].Text, $i.SubItems[1].Text, $i.SubItems[2].Text,
            $i.SubItems[3].Text, $i.SubItems[4].Text, $i.SubItems[5].Text)
    }
    Write-Host ''
    Write-Host ('logs: ' + (@($logPick.Items) -join ', '))
    Write-Host ('log tail: ' + $logBox.Text.Length + ' characters from ' + $logPick.SelectedItem)
    exit 0
}

$form.Add_Shown({
    try { Update-Panel } catch {}
    if ($Start) {
        foreach ($sl in ($Start -split ',')) {
            $sl = $sl.Trim()
            if ($sl -and (Get-RealSlots) -contains $sl) { Start-Slot $sl }
        }
    }
    $timer.Start()
})

# Closing asks first, so training never keeps running unnoticed.
$form.Add_FormClosing({
    param($sender, $e)
    $rows = Get-Instances
    $training = @($rows | Where-Object { $_.SlotName -ne '-' -and $_.Role -notin @('stopping', 'report') })
    $viewer = @($rows | Where-Object { $_.Role -eq 'viewer' })
    if (-not $training.Count -and -not $viewer.Count) { return }

    $what = @()
    if ($training.Count) { $what += 'training' }
    if ($viewer.Count) { $what += 'the replay viewer' }
    $msg = "Stop $($what -join ' and ') before closing?`r`n`r`nYes stops it all. No closes the panel and leaves it running."
    $ans = [System.Windows.Forms.MessageBox]::Show($form, $msg, 'CsAI', 'YesNoCancel', 'Question')
    if ($ans -eq 'Cancel') { $e.Cancel = $true; return }
    if ($ans -eq 'Yes') {
        if ($training.Count) { foreach ($sl in (Get-RealSlots)) { Stop-Slot $sl } }
        if ($viewer.Count) { [void](Start-HiddenPowerShell (Join-Path $Root 'web\run.ps1') @('-Stop') $Root) }
    }
})
$form.Add_FormClosed({ $timer.Stop() })
[void]$form.ShowDialog()
