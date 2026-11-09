# desk-perf.ps1 - old-vs-new performance measurement.
#
# Old (before) = the running installed app (read-only sampling; the only input
# posted to it is the drag simulation, which ends where it started).
# New (after)  = a copy under build\testrun\perf launched with
#                DESK_TEST_INSTANCE=perf.
#
# Measurements:
#   * idle: 10 s of CPU plus WS / private / threads / GDI / USER,
#   * drag: posted WM_LBUTTONDOWN on a card title, WM_MOUSEMOVE at ~60 Hz for
#     10 s (moVing +/-24 px and returning), WM_LBUTTONUP at the original point,
#   * terminal cost: continuous PowerShell output for 10 s (posted WM_CHAR),
#     CPU delta and the latency until the first output frame appears (marker
#     command with a known delay), plus idle-vs-output comparison.
#
# Evidence: build\qa\perf\<RunId>\perf.json (+ board.png)
#
# Example:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\desk-perf.ps1 -RunId perf

param(
    [string]$Exe = '',
    [string]$RunId = '',
    [int]$Seconds = 10,
    [int]$LivePid = 0,
    [switch]$NoSign
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'desk-testlib.ps1')

if (-not $Exe) { $Exe = Join-Path $root 'build\bin\desk.exe' }
$TestRoot = Join-Path $root 'build\testrun'
if (-not $RunId) { $RunId = Get-Date -Format 'yyyyMMdd-HHmmss' }
$evidenceDir = Join-Path (Join-Path $root 'build\qa\perf') $RunId
New-Item -ItemType Directory -Path $evidenceDir -Force | Out-Null
$perf = [ordered]@{
    runId = $RunId
    at = (Get-Date).ToString('s')
    seconds = $Seconds
    exe = $Exe
    exeHash = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash
    old = [ordered]@{}
    new = [ordered]@{}
    notes = @()
}
$result = New-DeskResult
$result.Name = 'perf-old-vs-new'

function Measure-DragCpu {
    # Posts a real drag gesture to the board window: title press, ~60 Hz moves
    # for Seconds, return to the original point, release.
    param([IntPtr]$Hwnd, [int]$ClientX, [int]$ClientY, [int]$Seconds, [int]$Amplitude = 24)
    [void][DeskTestNative]::PostMessage($Hwnd, 0x0201, [IntPtr]1, [DeskTestNative]::MakeLParam($ClientX, $ClientY))
    Start-Sleep -Milliseconds 60
    $process = Get-Process -Id $script:targetPid -ErrorAction SilentlyContinue
    if ($process) { $process.Refresh(); $cpu0 = $process.TotalProcessorTime.TotalMilliseconds }
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $moves = 0
    $direction = 1
    while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
        $offset = $Amplitude * $direction
        [void][DeskTestNative]::PostMessage($Hwnd, 0x0200, [IntPtr]1, [DeskTestNative]::MakeLParam(($ClientX + $offset), ($ClientY + ($offset / 3))))
        $moves += 1
        $direction = -$direction
        Start-Sleep -Milliseconds 15
    }
    # Return to the exact press point before releasing so the saved layout does
    # not change (the board resolves the release position from these points).
    [void][DeskTestNative]::PostMessage($Hwnd, 0x0200, [IntPtr]1, [DeskTestNative]::MakeLParam($ClientX, $ClientY))
    [void][DeskTestNative]::PostMessage($Hwnd, 0x0202, [IntPtr]0, [DeskTestNative]::MakeLParam($ClientX, $ClientY))
    Start-Sleep -Milliseconds 250
    if ($process) { $process.Refresh(); $cpu1 = $process.TotalProcessorTime.TotalMilliseconds }
    $elapsed = $watch.Elapsed.TotalSeconds
    $delta = 0
    if ($process) { $delta = $cpu1 - $cpu0 }
    return [pscustomobject]@{
        Seconds = [math]::Round($elapsed, 2)
        Moves = $moves
        MoveHz = [math]::Round($moves / $elapsed, 1)
        CpuMs = [math]::Round($delta, 1)
        CpuPct = [math]::Round(100.0 * $delta / ($elapsed * 1000.0), 2)
    }
}

function Measure-HoverCpu {
    # Mouse moves without a button: event-driven input handling should do no
    # repaint at all while the pointer stays over the same (empty) target.
    param([IntPtr]$Hwnd, [int]$ClientX, [int]$ClientY, [int]$Seconds)
    $process = Get-Process -Id $script:targetPid -ErrorAction SilentlyContinue
    if (-not $process) { return $null }
    $process.Refresh()
    $cpu0 = $process.TotalProcessorTime.TotalMilliseconds
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $moves = 0
    while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
        $jitter = $moves % 3
        [void][DeskTestNative]::PostMessage($Hwnd, 0x0200, [IntPtr]0, [DeskTestNative]::MakeLParam(($ClientX + $jitter), $ClientY))
        $moves += 1
        Start-Sleep -Milliseconds 15
    }
    $elapsed = $watch.Elapsed.TotalSeconds
    $process.Refresh()
    $cpu1 = $process.TotalProcessorTime.TotalMilliseconds
    return [pscustomobject]@{
        Seconds = [math]::Round($elapsed, 2)
        Moves = $moves
        CpuMs = [math]::Round($cpu1 - $cpu0, 1)
        CpuPct = [math]::Round(100.0 * ($cpu1 - $cpu0) / ($elapsed * 1000.0), 2)
    }
}

function Measure-OutputLatency {
    # Sends a command that sleeps a known time and then prints a marker; the
    # first console frame that differs from the baseline is the marker frame.
    param([IntPtr]$Hwnd, [object]$ConsoleRect, [int]$KnownDelayMs = 1000)
    Send-DeskText -Hwnd $Hwnd -Text ('Start-Sleep -Milliseconds {0}; "MARKER"' -f $KnownDelayMs)
    Start-Sleep -Milliseconds 150
    Send-DeskEnter -Hwnd $Hwnd
    Start-Sleep -Milliseconds 550
    $baseline = [DeskTestImage]::PrintBitmap($Hwnd, $script:boardW, $script:boardH)
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $appeared = -1
    try {
        while ($watch.ElapsedMilliseconds -lt ($KnownDelayMs + 3000)) {
            $shot = [DeskTestImage]::PrintBitmap($Hwnd, $script:boardW, $script:boardH)
            $diff = [DeskTestImage]::DiffFraction($baseline, $shot, $ConsoleRect.X, $ConsoleRect.Y, $ConsoleRect.W, $ConsoleRect.H, 35)
            $shot.Dispose()
            if ($diff -gt 0.0004) { $appeared = $watch.ElapsedMilliseconds; break }
            Start-Sleep -Milliseconds 10
        }
    } finally { $baseline.Dispose() }
    $latency = -1
    if ($appeared -ge 0) { $latency = $appeared - ($KnownDelayMs - 550) }
    return [pscustomobject]@{ SampleMs = $appeared; LatencyMs = [math]::Round($latency, 0); KnownDelayMs = $KnownDelayMs }
}

function Measure-OutputCpu {
    # Continuous output for Seconds from the PowerShell card.
    param([IntPtr]$Hwnd, [int]$Seconds)
    $command = '$end=(Get-Date).AddSeconds(' + $Seconds + '); while((Get-Date) -lt $end){ 1..4|%{"X$_"}; Start-Sleep -Milliseconds 20 }'
    $process = Get-Process -Id $script:targetPid -ErrorAction SilentlyContinue
    if (-not $process) { return $null }
    Send-DeskText -Hwnd $Hwnd -Text $command
    Start-Sleep -Milliseconds 200
    Send-DeskEnter -Hwnd $Hwnd
    Start-Sleep -Milliseconds 150
    $process.Refresh()
    $cpu0 = $process.TotalProcessorTime.TotalMilliseconds
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Seconds $Seconds
    $process.Refresh()
    $cpu1 = $process.TotalProcessorTime.TotalMilliseconds
    $elapsed = $watch.Elapsed.TotalSeconds
    return [pscustomobject]@{
        Seconds = [math]::Round($elapsed, 2)
        CpuMs = [math]::Round($cpu1 - $cpu0, 1)
        CpuPct = [math]::Round(100.0 * ($cpu1 - $cpu0) / ($elapsed * 1000.0), 2)
    }
}

# ---------------------------------------------------------------------------
# OLD: the running installed app (read-only apart from the drag gesture)
# ---------------------------------------------------------------------------
$livePath = ''
$live = $null
if ($LivePid -gt 0) {
    $live = Get-Process -Id $LivePid -ErrorAction SilentlyContinue
} else {
    $live = @(Get-Process -Name desk -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and ($_.Path -like '*\Programs\Desktop Desk\desk.exe') }) | Select-Object -First 1
}
if ($live -and $live.Path) { $livePath = $live.Path }
if (-not $live) {
    Add-DeskNote $result 'installed desk.exe is not running; old-side numbers unavailable this run'
} else {
    $script:targetPid = $live.Id
    $perf.old.pid = $live.Id
    $perf.old.path = $livePath
    $perf.old.startTime = $live.StartTime.ToString('s')
    $liveStats = Get-DeskProcessStats -ProcessId $live.Id
    $perf.old.idleStats = $liveStats
    $liveIdle = Measure-DeskCpu -ProcessId $live.Id -Seconds $Seconds
    $perf.old.idleCpu = $liveIdle
    Write-Host ("[perf] old idle: ws=$($liveStats.WSMB)MB private=$($liveStats.PrivateMB)MB threads=$($liveStats.Threads) gdi=$($liveStats.GDI) user=$($liveStats.USER) cpu=$($liveIdle.CpuMs)ms/$($liveIdle.Seconds)s")

    $liveWindows = @(Get-DeskWindowList -ProcessId $live.Id | Where-Object { $_.Class -eq 'wg.desk' -and $_.Visible })
    if ($liveWindows.Count -gt 0) {
        $liveWindow = $liveWindows[0]
        $liveIniPath = Join-Path (Split-Path -Parent $livePath) 'desk.ini'
        $liveIni = $null
        $liveHashBefore = ''
        if (Test-Path -LiteralPath $liveIniPath) {
            $liveHashBefore = (Get-FileHash -LiteralPath $liveIniPath -Algorithm SHA256).Hash
            try { $liveIni = Read-DeskIniFile -Path $liveIniPath } catch { $liveIni = $null }
        }
        $dragX = 150
        $dragY = 20
        if ($liveIni -and $liveIni.Contains('card.ps.x') -and $liveIni.Contains('card.ps.y')) {
            $dragX = (Get-DeskIniInt $liveIni 'card.ps.x' 40) + 40
            $dragY = (Get-DeskIniInt $liveIni 'card.ps.y' 200) + 15
            $perf.old.dragCard = 'ps'
        } elseif ($liveIni -and $liveIni.Contains('card.account.x')) {
            $dragX = (Get-DeskIniInt $liveIni 'card.account.x' 40) + 40
            $dragY = (Get-DeskIniInt $liveIni 'card.account.y' 32) + 15
            $perf.old.dragCard = 'account'
        }
        $perf.old.dragPoint = @{ x = $dragX; y = $dragY }
        $liveDrag = Measure-DragCpu -Hwnd $liveWindow.Hwnd -ClientX $dragX -ClientY $dragY -Seconds $Seconds
        $perf.old.dragCpu = $liveDrag
        Write-Host ("[perf] old drag: {0} moves in {1}s, cpu={2}ms ({3}%)" -f $liveDrag.Moves, $liveDrag.Seconds, $liveDrag.CpuMs, $liveDrag.CpuPct)
        # Empty board area, no button: nothing should repaint.
        $hoverX = [math]::Max(200, [int]($liveWindow.W / 2))
        $hoverY = [math]::Max(200, [int]($liveWindow.H - 120))
        $liveHover = Measure-HoverCpu -Hwnd $liveWindow.Hwnd -ClientX $hoverX -ClientY $hoverY -Seconds $Seconds
        $perf.old.hoverCpu = $liveHover
        Write-Host ("[perf] old hover-only moves: {0} cpu={1}ms ({2}%)" -f $liveHover.Moves, $liveHover.CpuMs, $liveHover.CpuPct)
        Start-Sleep -Milliseconds 600
        if ($liveHashBefore) {
            $liveHashAfter = (Get-FileHash -LiteralPath $liveIniPath -Algorithm SHA256).Hash
            $perf.old.liveIniHashBefore = $liveHashBefore
            $perf.old.liveIniHashAfter = $liveHashAfter
            $perf.old.liveIniUnchanged = ($liveHashBefore -eq $liveHashAfter)
            [void](Add-DeskCheck $result 'live desk.ini content unchanged by the drag probe' ($liveHashBefore -eq $liveHashAfter))
        }
        Add-DeskNote $result 'old-side PowerShell output cost was not measured: typing into the live terminal would disturb the running session'
    } else {
        Add-DeskNote $result 'installed board window not found; old drag not measured'
    }
}

# ---------------------------------------------------------------------------
# NEW: isolated copy under build\testrun\perf
# ---------------------------------------------------------------------------
$scenarioDir = Join-Path $TestRoot 'perf'
$leftover = @(Stop-AllDeskTestProcesses -Root $TestRoot)
if ($leftover.Count -gt 0) { Write-Host ("stopped leftover testrun processes: " + ($leftover -join ',')) }
[void](New-DeskTestDir -Path $scenarioDir -Root $TestRoot)
$copy = Join-Path $scenarioDir 'desk.exe'
[void][System.IO.File]::Copy($Exe, $copy, $true)
if (-not $NoSign) { & (Join-Path $PSScriptRoot 'sign-local.ps1') -Path $copy | Out-Host }
Write-DeskIniFile -Path (Join-Path $scenarioDir 'desk.ini') -Values ([ordered]@{
    'board.grid'            = 1
    'board.h'               = 1310
    'board.solid'           = 0
    'board.version'         = 2
    'board.w'               = 1280
    'board.x'               = 1280
    'board.y'               = 0
    'card.account.expanded' = 0
    'card.account.visible'  = 1
    'card.account.x'        = 14
    'card.account.y'        = 32
    'card.ps.expanded'      = 1
    'card.ps.visible'       = 1
    'card.ps.x'             = 14
    'card.ps.y'             = 400
    'card.servers.expanded' = 0
    'card.servers.visible'  = 1
    'card.servers.x'        = 14
    'card.servers.y'        = 200
})
$runKeyBefore = Get-DeskRunKeySnapshot
$newPid = Start-DeskTestInstance -ExePath $copy -WorkingDirectory $scenarioDir -Suffix 'perf'
$script:targetPid = $newPid
$perf.new.pid = $newPid
$window = $null
$deadline = (Get-Date).AddSeconds(25)
while ((Get-Date) -lt $deadline -and -not $window) {
    if (-not (Test-DeskTestAlive -ProcessId $newPid)) { break }
    $candidates = @(Get-DeskWindowList -ProcessId $newPid | Where-Object { $_.Visible -and ($_.Class -eq 'wg.desk-perf' -or $_.Class -eq 'wg.desk') })
    if ($candidates.Count -gt 0) { $window = $candidates[0] } else { Start-Sleep -Milliseconds 100 }
}
[void](Add-DeskCheck $result 'new instance started' ($null -ne $window) ("pid=$newPid"))
if (-not $window) {
    [void](Close-DeskBoard -ProcessId $newPid -Hwnd ([IntPtr]::Zero) -Root $TestRoot)
    [void](Save-DeskResult -Result $result -Path (Join-Path $evidenceDir 'result.json'))
    exit 1
}
$script:boardW = $window.W
$script:boardH = $window.H
[void](Wait-IniStable -Path (Join-Path $scenarioDir 'desk.ini') -TimeoutSeconds 6)
Start-Sleep -Milliseconds 1200

$ini = Read-DeskIniFile -Path (Join-Path $scenarioDir 'desk.ini')
$psX = 14; $psY = 400
if ($ini) {
    $psX = Get-DeskIniInt $ini 'card.ps.x' 14
    $psY = Get-DeskIniInt $ini 'card.ps.y' 400
}
$shot = Save-DeskWindowShot -Hwnd $window.Hwnd -X $window.X -Y $window.Y -W $window.W -H $window.H -Path (Join-Path $evidenceDir 'new-board.png')
$result.Evidence.Add("screenshot=$($shot.Path) mode=$($shot.Mode)")

$newStats = Get-DeskProcessStats -ProcessId $newPid
$perf.new.idleStats = $newStats
$newIdle = Measure-DeskCpu -ProcessId $newPid -Seconds $Seconds
$perf.new.idleCpu = $newIdle
Write-Host ("[perf] new idle: ws=$($newStats.WSMB)MB private=$($newStats.PrivateMB)MB threads=$($newStats.Threads) gdi=$($newStats.GDI) user=$($newStats.USER) cpu=$($newIdle.CpuMs)ms/$($newIdle.Seconds)s")

$newDrag = Measure-DragCpu -Hwnd $window.Hwnd -ClientX ($psX + 40) -ClientY ($psY + 15) -Seconds $Seconds
$perf.new.dragCpu = $newDrag
Write-Host ("[perf] new drag: {0} moves in {1}s, cpu={2}ms ({3}%)" -f $newDrag.Moves, $newDrag.Seconds, $newDrag.CpuMs, $newDrag.CpuPct)
[void](Wait-IniStable -Path (Join-Path $scenarioDir 'desk.ini') -TimeoutSeconds 6)
$iniAfterDrag = Read-DeskIniFile -Path (Join-Path $scenarioDir 'desk.ini')
$dragRestored = $false
if ($iniAfterDrag -and $ini) {
    $dragRestored = ((Get-DeskIniInt $iniAfterDrag 'card.ps.x' -1) -eq $psX) -and ((Get-DeskIniInt $iniAfterDrag 'card.ps.y' -1) -eq $psY)
}
$perf.new.dragRestoredPosition = $dragRestored
[void](Add-DeskCheck $result 'drag probe ends on the original card position' $dragRestored)

$newHover = Measure-HoverCpu -Hwnd $window.Hwnd -ClientX 700 -ClientY 1200 -Seconds $Seconds
$perf.new.hoverCpu = $newHover
Write-Host ("[perf] new hover-only moves: {0} cpu={1}ms ({2}%)" -f $newHover.Moves, $newHover.CpuMs, $newHover.CpuPct)

# PowerShell card: focus by clicking the body, then measure output cost.
$clickX = $psX + 200
$clickY = $psY + 38 + 60
[void](Send-DeskClick -Hwnd $window.Hwnd -X $clickX -Y $clickY)
Start-Sleep -Milliseconds 400
$consoleRect = [pscustomobject]@{ X = $psX + 4; Y = $psY + 42; W = 560; H = 336 }
$latency = Measure-OutputLatency -Hwnd $window.Hwnd -ConsoleRect $consoleRect -KnownDelayMs 1000
$perf.new.outputLatency = $latency
Write-Host ("[perf] new terminal output latency: {0}ms (sample at {1}ms after a {2}ms delayed marker)" -f $latency.LatencyMs, $latency.SampleMs, $latency.KnownDelayMs)
[void](Add-DeskCheck $result 'terminal output is painted promptly' ($latency.LatencyMs -ge 0 -and $latency.LatencyMs -lt 250) ("{0} ms after the command produced output" -f $latency.LatencyMs))

Start-Sleep -Milliseconds 500
$outputCpu = Measure-OutputCpu -Hwnd $window.Hwnd -Seconds $Seconds
$perf.new.outputCpu = $outputCpu
Write-Host ("[perf] new terminal 10s output: cpu={0}ms ({1}%)" -f $outputCpu.CpuMs, $outputCpu.CpuPct)
$perf.new.outputIdleDeltaMs = [math]::Round($outputCpu.CpuMs - $newIdle.CpuMs, 1)
Start-Sleep -Milliseconds 800
$shot = Save-DeskWindowShot -Hwnd $window.Hwnd -X $window.X -Y $window.Y -W $window.W -H $window.H -Path (Join-Path $evidenceDir 'new-terminal-output.png')
$result.Evidence.Add("terminal screenshot=$($shot.Path)")

$close = Close-DeskBoard -ProcessId $newPid -Hwnd $window.Hwnd -Root $TestRoot
[void](Add-DeskCheck $result 'new instance closed gracefully' $close.Graceful)
$runKeyAfter = Get-DeskRunKeySnapshot
$runKeyChanges = Compare-DeskRunKey -Before $runKeyBefore -After $runKeyAfter
[void](Add-DeskCheck $result 'HKCU Run autostart unchanged' ($runKeyChanges.Count -eq 0) ($runKeyChanges -join '; '))

# ---------------------------------------------------------------------------
# size-matched comparison: new build on the old 760x800 board
# ---------------------------------------------------------------------------
$smallDir = Join-Path $TestRoot 'perf760'
[void](New-DeskTestDir -Path $smallDir -Root $TestRoot)
$smallCopy = Join-Path $smallDir 'desk.exe'
[void][System.IO.File]::Copy($Exe, $smallCopy, $true)
if (-not $NoSign) { & (Join-Path $PSScriptRoot 'sign-local.ps1') -Path $smallCopy | Out-Host }
Write-DeskIniFile -Path (Join-Path $smallDir 'desk.ini') -Values ([ordered]@{
    'board.grid'            = 1
    'board.h'               = 800
    'board.solid'           = 0
    'board.version'         = 2
    'board.w'               = 760
    'board.x'               = 1760
    'board.y'               = 64
    'card.account.expanded' = 0
    'card.account.visible'  = 1
    'card.account.x'        = 40
    'card.account.y'        = 32
    'card.ps.expanded'      = 1
    'card.ps.visible'       = 1
    'card.ps.x'             = 40
    'card.ps.y'             = 200
    'card.servers.expanded' = 0
    'card.servers.visible'  = 1
    'card.servers.x'        = 392
    'card.servers.y'        = 32
})
$smallPid = Start-DeskTestInstance -ExePath $smallCopy -WorkingDirectory $smallDir -Suffix 'perf760'
$script:targetPid = $smallPid
$smallWindow = $null
$deadline = (Get-Date).AddSeconds(25)
while ((Get-Date) -lt $deadline -and -not $smallWindow) {
    if (-not (Test-DeskTestAlive -ProcessId $smallPid)) { break }
    $candidates = @(Get-DeskWindowList -ProcessId $smallPid | Where-Object { $_.Visible -and ($_.Class -eq 'wg.desk-perf760' -or $_.Class -eq 'wg.desk') })
    if ($candidates.Count -gt 0) { $smallWindow = $candidates[0] } else { Start-Sleep -Milliseconds 100 }
}
$perf.new760 = [ordered]@{ pid = $smallPid }
if ($smallWindow) {
    $script:boardW = $smallWindow.W
    $script:boardH = $smallWindow.H
    [void](Wait-IniStable -Path (Join-Path $smallDir 'desk.ini') -TimeoutSeconds 6)
    Start-Sleep -Milliseconds 1200
    $smallStats = Get-DeskProcessStats -ProcessId $smallPid
    $perf.new760.idleStats = $smallStats
    $perf.new760.idleCpu = Measure-DeskCpu -ProcessId $smallPid -Seconds $Seconds
    Write-Host ("[perf] new-760 idle: ws=$($smallStats.WSMB)MB private=$($smallStats.PrivateMB)MB threads=$($smallStats.Threads) gdi=$($smallStats.GDI) user=$($smallStats.USER) cpu=$($perf.new760.idleCpu.CpuMs)ms")
    $smallIni = Read-DeskIniFile -Path (Join-Path $smallDir 'desk.ini')
    $smallPsX = 40; $smallPsY = 200
    if ($smallIni) {
        $smallPsX = Get-DeskIniInt $smallIni 'card.ps.x' 40
        $smallPsY = Get-DeskIniInt $smallIni 'card.ps.y' 200
    }
    $perf.new760.dragCpu = Measure-DragCpu -Hwnd $smallWindow.Hwnd -ClientX ($smallPsX + 40) -ClientY ($smallPsY + 15) -Seconds $Seconds
    Write-Host ("[perf] new-760 drag: {0} moves cpu={1}ms ({2}%)" -f $perf.new760.dragCpu.Moves, $perf.new760.dragCpu.CpuMs, $perf.new760.dragCpu.CpuPct)
    $perf.new760.hoverCpu = Measure-HoverCpu -Hwnd $smallWindow.Hwnd -ClientX 400 -ClientY 700 -Seconds $Seconds
    Write-Host ("[perf] new-760 hover-only moves: cpu={0}ms ({1}%)" -f $perf.new760.hoverCpu.CpuMs, $perf.new760.hoverCpu.CpuPct)
    $smallClose = Close-DeskBoard -ProcessId $smallPid -Hwnd $smallWindow.Hwnd -Root $TestRoot
    [void](Add-DeskCheck $result 'size-matched instance closed gracefully' $smallClose.Graceful)
} else {
    Add-DeskNote $result 'size-matched 760x800 instance did not start'
    [void](Close-DeskBoard -ProcessId $smallPid -Hwnd ([IntPtr]::Zero) -Root $TestRoot)
}

# ---------------------------------------------------------------------------
# write evidence
# ---------------------------------------------------------------------------
$perf.summary = [ordered]@{
    oldIdleCpuMs = $perf.old.idleCpu.CpuMs
    oldHoverCpuMs = $perf.old.hoverCpu.CpuMs
    oldDragCpuMs = $perf.old.dragCpu.CpuMs
    newIdleCpuMs = $perf.new.idleCpu.CpuMs
    newHoverCpuMs = $perf.new.hoverCpu.CpuMs
    newDragCpuMs = $perf.new.dragCpu.CpuMs
    new760IdleCpuMs = $perf.new760.idleCpu.CpuMs
    new760HoverCpuMs = $perf.new760.hoverCpu.CpuMs
    new760DragCpuMs = $perf.new760.dragCpu.CpuMs
    newOutputCpuMs = $perf.new.outputCpu.CpuMs
    newOutputLatencyMs = $perf.new.outputLatency.LatencyMs
    oldIniUnchanged = $perf.old.liveIniUnchanged
}
[System.IO.File]::WriteAllText((Join-Path $evidenceDir 'perf.json'), ($perf | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
[void](Save-DeskResult -Result $result -Path (Join-Path $evidenceDir 'result.json'))
Write-Host ("perf evidence: {0}" -f $evidenceDir)
$failed = @($result.Checks | Where-Object { -not $_.Pass })
foreach ($check in $failed) { Write-Host ("FAIL: {0}  {1}" -f $check.Name, $check.Detail) }
if ($failed.Count -gt 0) { exit 1 }
exit 0
