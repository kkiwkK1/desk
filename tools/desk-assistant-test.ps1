# desk-assistant-test.ps1 - isolated M1 assistant acceptance harness.
#
# Stages:
#   offline  fake Pi contract, no network. Full assistant-only GUI checks.
#   real     real Pi channel (needs -AuthorizedNetworkProbe). One small Chinese
#            question, Esc cancel, citation/cost/child/idle checks, plus an
#            abort -> recovery ask, a natural idle stop and a wake that must
#            resume the same private Pi session. Full GUI acceptance; probe is not.
#   probe    the same isolated real channel but only one minimal question. The
#            result is written separately and is NOT full GUI acceptance.
#
# Safety:
#   * every run copies build\bin\desk.exe under build\qa\assistant-m1\<run>\app
#     and sets its own DESK_TEST_INSTANCE / DESK_TEST_CONFIG_DIR / fixture DSH;
#   * the script only stops the PID it started plus descendants it recorded
#     before close. A descendant is killed only after re-proving the same PID,
#     creation time and executable image path (so a reused PID or unreadable
#     image is never touched). Unverifiable/failed cleanup is reported, and the
#     real node.exe Pi child may be outside the run directory but is still our
#     recorded subtree. It never enumerates or kills other desk/pi;
#   * screenshots are PrintWindow only. When a layered window prints blank the
#     own-canvas test hook is used. This script never falls back to screen BitBlt;
#   * no key values, command lines, full environment or real DSH content is
#     read or written. The live HKCU Run values and the metadata fingerprint
#     (names/sizes/mtimes only) of every *production* config target (installed
#     exe dir, repo build\bin, a reliably derived main-worktree build\bin, and
#     %APPDATA%\Desktop Desk when present) are compared before/after without
#     copying file contents. A target that does not exist is recorded as missing
#     and can never by itself prove "production config unchanged";
#   * offline child processes are a positive control: the copied fake Pi must be
#     observed alive with PID > 0 and that exact image path, and the recorded
#     identity is what the cleanup later re-proves. enabled=0 is the opposite
#     control: it must own no window and no child subtree at all. The real stage
#     samples the recorded Node/Pi child (working set + CPU over an explicit
#     interval) before any cancel/close, and reports sampling failure instead of
#     reading an empty set as 0 MB;
#   * the real stage inherits the real USERPROFILE/LOCALAPPDATA/TEMP so the
#     trusted node/pi.cmd launcher, the official provider extension and the
#     key_ref resolve; only the desk/Pi config is isolated and Pi's agent dir,
#     pi-work and session file stay under DESK_TEST_CONFIG_DIR. That is a
#     process-environment boundary, not an OS sandbox: the trusted Pi/provider
#     may still write its own install temp locks and caches elsewhere.
#
# FAKE_PI_LOG records command types only (get_state/prompt/abort); it never
# contains prompt text. The script asserts types/order, not payload text.
#
# The own-canvas hook (implemented in src/widget/board.cpp) is:
#   message WM_APP+11 (0x800B), wParam 0 or 1, lParam ignored.
#   Files: <DESK_TEST_CONFIG_DIR>\assistant-test-frame-0.bmp / -1.bmp.
#   Synchronous SendMessage returns 1 on success. The hook exists only for a
#   valid DESK_TEST_INSTANCE with a valid DESK_TEST_CONFIG_DIR.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\desk-assistant-test.ps1 -Stage offline
#   powershell -ExecutionPolicy Bypass -File tools\desk-assistant-test.ps1 -Stage real -AuthorizedNetworkProbe
#   powershell -ExecutionPolicy Bypass -File tools\desk-assistant-test.ps1 -Stage probe -AuthorizedNetworkProbe
#
# Optional:
#   -PhysicalHotkeyProbe  press Ctrl+Alt+F10 with keybd_event to exercise the
#                         real RegisterHotKey path. It is an equivalent test of
#                         the separate test chord only; the default Ctrl+Alt+A
#                         is never pressed by this script. Default test runs
#                         register no global hotkeys; this switch sets
#                         DESK_TEST_REGISTER_ASSISTANT_HOTKEY=1, which the app
#                         accepts only for a valid test instance + config and
#                         then registers only the assistant chord.
#
# MEMORY card coverage uses only this PID's popup menu (MSAA), the framework
# wg.Prompt window (WM_CHAR + Enter) and the #32770 confirm buttons (IDOK /
# IDCANCEL). The script never edits assistant-facts.jsonl to fake UI state.
#
# This file is intentionally not run by the implementation task. The Lead
# decides when to build, sign and execute a stage.

[CmdletBinding()]
param(
    [ValidateSet('offline', 'real', 'probe')]
    [string]$Stage = 'offline',
    [switch]$AuthorizedNetworkProbe,
    [switch]$PhysicalHotkeyProbe,
    [string]$ExePath = '',
    [string]$Provider = 'command-code',
    [string]$Model = 'deepseek/deepseek-v4.1-flash-fast',
    [string]$KeyRef = 'COMMANDCODE_API_KEY_2',
    [int]$TimeoutSeconds = 90
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'desk-testlib.ps1')

$script:CanvasMessage = 0x800B
$script:HotkeyMessage = 0x0312
$script:HotkeyAssistantId = 5
$script:RepoRoot = Split-Path -Parent $PSScriptRoot
$script:BuildRoot = Join-Path $script:RepoRoot 'build'
$script:QaRoot = Join-Path $script:BuildRoot 'qa\assistant-m1'
$script:LastRun = $null
$script:RuntimeDependencies = @()

function Write-Utf8File {
    param([string]$Path, [string]$Text)
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

function Write-JsonFile {
    param([string]$Path, [object]$Value)
    Write-Utf8File -Path $Path -Text ($Value | ConvertTo-Json -Depth 12)
}

function Read-Utf8File {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    try {
        return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
    } catch {
        return ''
    }
}

function Read-JsonLines {
    param([string]$Path)
    $text = Read-Utf8File -Path $Path
    $items = New-Object System.Collections.Generic.List[object]
    foreach ($line in ($text -split "`n")) {
        $line = $line.Trim()
        if (-not $line) { continue }
        try { $items.Add(($line | ConvertFrom-Json)) } catch { }
    }
    # Return object[] rather than List[object]: some PS5 policy/serialization
    # paths choke on @($genericList) at the end of a pipeline.
    return $items.ToArray()
}

function Wait-FilePredicate {
    param([string]$Path, [scriptblock]$Predicate, [int]$TimeoutSeconds = 20)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $latest = ''
    while ((Get-Date) -lt $deadline) {
        $latest = Read-Utf8File -Path $Path
        if (& $Predicate $latest) { return $latest }
        Start-Sleep -Milliseconds 200
    }
    return $latest
}

function Get-FileSnapshotFingerprint {
    # Names/sizes/mtimes only. Never reads file contents.
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return '<missing>' }
    $base = [System.IO.Path]::GetFullPath($Path)
    $items = @(Get-ChildItem -LiteralPath $base -File -Recurse -Force -ErrorAction SilentlyContinue |
        Sort-Object FullName)
    $parts = New-Object System.Collections.Generic.List[string]
    foreach ($item in $items) {
        $full = [System.IO.Path]::GetFullPath($item.FullName)
        $rel = $full.Substring($base.Length).TrimStart('\')
        $parts.Add(('{0}|{1}|{2}' -f $rel, $item.Length, $item.LastWriteTimeUtc.Ticks))
    }
    return ($parts.ToArray() -join ';')
}

function Get-MainWorktreeRoot {
    # Worktrees in this repo live at <main>\_worktrees\<name>. Report the main
    # root only when it really is the primary worktree: there ".git" is a
    # directory, while a linked worktree carries a ".git" file. Read-only.
    param([string]$RepoRoot)
    if (-not $RepoRoot) { return '' }
    $parent = Split-Path -Parent $RepoRoot
    if (-not $parent) { return '' }
    if ((Split-Path -Leaf $parent) -ne '_worktrees') { return '' }
    $main = Split-Path -Parent $parent
    if (-not $main) { return '' }
    if (-not (Test-Path -LiteralPath (Join-Path $main '.git') -PathType Container)) { return '' }
    return [System.IO.Path]::GetFullPath($main)
}

function Get-ProductionConfigTargets {
    # Directories a production (non-test) desk.exe would really use for its
    # config: appenv::ConfigDir prefers the writable exe directory (the
    # installer's %LOCALAPPDATA%\Programs\Desktop Desk) and falls back to
    # %APPDATA%\Desktop Desk. build\bin covers the built exe, and the main
    # worktree's build\bin the other session's copy. This run's own build\qa
    # tree is never a target and no target content is read.
    $candidates = New-Object System.Collections.Generic.List[object]
    if ($env:LOCALAPPDATA) {
        $candidates.Add([pscustomobject]@{
            Label = 'installed exe dir (assistant)'
            Path  = (Join-Path $env:LOCALAPPDATA 'Programs\Desktop Desk Assistant')
        })
        $candidates.Add([pscustomobject]@{
            Label = 'installed exe dir'
            Path  = (Join-Path $env:LOCALAPPDATA 'Programs\Desktop Desk')
        })
    }
    $candidates.Add([pscustomobject]@{
        Label = 'repo build\bin'
        Path  = (Join-Path $script:BuildRoot 'bin')
    })
    $mainRoot = Get-MainWorktreeRoot -RepoRoot $script:RepoRoot
    if ($mainRoot) {
        $candidates.Add([pscustomobject]@{
            Label = 'main worktree build\bin'
            Path  = (Join-Path $mainRoot 'build\bin')
        })
    }
    if ($env:APPDATA) {
        $candidates.Add([pscustomobject]@{
            Label = 'APPDATA fallback'
            Path  = (Join-Path $env:APPDATA 'Desktop Desk')
        })
    }
    $targets = New-Object System.Collections.Generic.List[object]
    $seen = @{}
    foreach ($candidate in $candidates) {
        $full = [System.IO.Path]::GetFullPath($candidate.Path)
        $key = $full.ToLowerInvariant()
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true
        $targets.Add([pscustomobject]@{ Label = $candidate.Label; Path = $full })
    }
    return $targets.ToArray()
}

function Get-LiveDeskProcessDirs {
    # Directories of running desk.exe images (metadata only). A production
    # instance writes its own config while a QA run is in flight, so a target it
    # owns cannot be attributed to this run. Nothing here is messaged, stopped
    # or enumerated for windows.
    $directories = New-Object System.Collections.Generic.List[object]
    foreach ($process in @(Get-CimInstance Win32_Process -Filter "Name='desk.exe'" -ErrorAction SilentlyContinue)) {
        $path = ''
        try { $path = [string]$process.ExecutablePath } catch { }
        if (-not $path) { continue }
        $directories.Add([pscustomobject]@{
            ProcessId = [int]$process.ProcessId
            Directory = (Split-Path -Parent $path)
        })
    }
    return $directories.ToArray()
}

function Get-ProductionConfigSnapshot {
    # Metadata only: existence, entry count and the names/sizes/mtimes
    # fingerprint. Contents are never read, so no live config value can leak
    # into the evidence.
    $snapshot = New-Object System.Collections.Generic.List[object]
    $liveOwners = @(Get-LiveDeskProcessDirs)
    foreach ($target in (Get-ProductionConfigTargets)) {
        $exists = Test-Path -LiteralPath $target.Path -PathType Container
        $entries = 0
        if ($exists) {
            $entries = @(Get-ChildItem -LiteralPath $target.Path -File -Recurse -Force `
                -ErrorAction SilentlyContinue).Count
        }
        $liveOwner = 0
        foreach ($live in $liveOwners) {
            if ([string]::Equals([string]$live.Directory, [string]$target.Path,
                    [System.StringComparison]::OrdinalIgnoreCase)) {
                $liveOwner = [int]$live.ProcessId
                break
            }
        }
        $snapshot.Add([pscustomobject]@{
            Label       = $target.Label
            Path        = $target.Path
            Exists      = $exists
            Entries     = $entries
            LiveOwner   = $liveOwner
            Fingerprint = (Get-FileSnapshotFingerprint -Path $target.Path)
        })
    }
    return $snapshot.ToArray()
}

function Add-ProductionConfigChecks {
    # "Production config is unchanged" needs at least one existing non-empty
    # target: a snapshot full of missing directories compares equal to itself,
    # which is exactly the evidence vacuum this check must refuse.
    param([object]$Result, [object[]]$Before, [object[]]$After, [string]$Prefix)
    # A target owned by a running production desk.exe is recorded (metadata only)
    # but excluded from the "unchanged" assertion: the live app writes its own
    # config, so a change there is not evidence about this QA run.
    $assertable = @($Before | Where-Object { -not $_.LiveOwner })
    $observed = @($assertable | Where-Object { $_.Exists -and $_.Entries -gt 0 })
    $summary = New-Object System.Collections.Generic.List[string]
    $changed = New-Object System.Collections.Generic.List[string]
    $liveOwned = New-Object System.Collections.Generic.List[string]
    foreach ($item in $Before) {
        $match = @($After | Where-Object { $_.Path -eq $item.Path })
        $same = ($match.Count -eq 1 -and $match[0].Fingerprint -eq $item.Fingerprint -and
                 $match[0].Exists -eq $item.Exists)
        if (-not $same -and -not $item.LiveOwner) { $changed.Add($item.Label) }
        $state = 'no'
        if (-not $same) { $state = 'yes' }
        if ($item.LiveOwner) {
            $state += ' (live production pid ' + $item.LiveOwner + ', not asserted)'
            $liveOwned.Add($item.Label)
        }
        $summary.Add(('{0} exists={1} entries={2} changed={3}' -f $item.Label, $item.Exists,
            $item.Entries, $state))
    }
    Add-DeskNote -Result $Result -Note ('{0} production config targets: {1}' -f $Prefix,
        ($summary.ToArray() -join '; '))
    if ($liveOwned.Count -gt 0) {
        $liveNote = '{0} targets owned by a running production desk.exe (metadata recorded, ' +
            'their writes are not attributed to this run): {1}'
        [void](Add-DeskNote $Result ($liveNote -f $Prefix, ($liveOwned.ToArray() -join ', ')))
    }
    $missing = @($assertable | Where-Object { -not $_.Exists -or $_.Entries -le 0 } |
        ForEach-Object { '{0}=missing-or-empty' -f $_.Label })
    [void](Add-DeskCheck $Result ($Prefix + ' production config target exists for comparison') `
        ($observed.Count -gt 0) ($missing -join ', '))
    [void](Add-DeskCheck $Result ($Prefix + ' production config metadata is unchanged') `
        ($observed.Count -gt 0 -and $changed.Count -eq 0) ($changed.ToArray() -join ', '))
}

function Get-CreationTicks {
    param($Value)
    if ($null -eq $Value) { return $null }
    if ($Value -is [datetime]) { return $Value.ToUniversalTime().Ticks }
    try { return ([datetime]$Value).ToUniversalTime().Ticks } catch { return $null }
}

function Get-CimProcessIdentity {
    param([int]$ProcessId)
    $process = Get-CimInstance Win32_Process -Filter "ProcessId=$ProcessId" -ErrorAction SilentlyContinue
    if (-not $process) { return $null }
    $path = ''
    try { $path = [string]$process.ExecutablePath } catch { }
    $ticks = Get-CreationTicks $process.CreationDate
    if ($null -eq $ticks) {
        try {
            $gp = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
            if ($gp) { $ticks = $gp.StartTime.ToUniversalTime().Ticks }
        } catch { }
    }
    return [pscustomobject]@{
        ProcessId = [int]$process.ProcessId
        ParentProcessId = [int]$process.ParentProcessId
        Name = [string]$process.Name
        CreationTicks = $ticks
        ExecutablePath = $path
    }
}

function Get-ProcessDescendants {
    # BFS over ParentProcessId only; metadata only. Never reads command line or
    # environment. CreationTime + executable path are recorded so a later cleanup
    # can prove it is still the same process, not a reused PID.
    param([int]$RootProcessId)
    $all = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Select-Object ProcessId, ParentProcessId, Name, CreationDate, ExecutablePath)
    $result = New-Object System.Collections.Generic.List[object]
    $frontier = New-Object System.Collections.Generic.List[int]
    $frontier.Add($RootProcessId)
    $seen = @{}
    while ($frontier.Count -gt 0) {
        $parent = $frontier[0]
        $frontier.RemoveAt(0)
        foreach ($process in $all) {
            if ([int]$process.ParentProcessId -ne $parent) { continue }
            $id = [int]$process.ProcessId
            if ($seen.ContainsKey($id)) { continue }
            $seen[$id] = $true
            $path = ''
            try { $path = [string]$process.ExecutablePath } catch { }
            $result.Add([pscustomobject]@{
                ProcessId = $id
                ParentProcessId = [int]$process.ParentProcessId
                Name = [string]$process.Name
                CreationTicks = (Get-CreationTicks $process.CreationDate)
                ExecutablePath = $path
            })
            $frontier.Add($id)
        }
    }
    return $result.ToArray()
}

function Test-RecordedProcess {
    # Same PID is not enough: require the recorded creation time and the same
    # executable image path. A reused PID or an unreadable image path is never
    # killed.
    param([object]$Recorded)
    $current = Get-CimProcessIdentity -ProcessId ([int]$Recorded.ProcessId)
    if (-not $current) { return [pscustomobject]@{ State = 'gone'; Current = $null; Reason = 'process exited' } }
    if ($null -eq $Recorded.CreationTicks -or $null -eq $current.CreationTicks) {
        return [pscustomobject]@{ State = 'unverified'; Current = $current; Reason = 'missing creation time' }
    }
    if ([long]$Recorded.CreationTicks -ne [long]$current.CreationTicks) {
        return [pscustomobject]@{ State = 'reused'; Current = $current; Reason = 'pid creation time changed' }
    }
    $oldPath = [string]$Recorded.ExecutablePath
    $newPath = [string]$current.ExecutablePath
    if (-not $oldPath -or -not $newPath) {
        return [pscustomobject]@{ State = 'unverified'; Current = $current; Reason = 'executable path unavailable' }
    }
    $fullOld = [System.IO.Path]::GetFullPath($oldPath)
    $fullNew = [System.IO.Path]::GetFullPath($newPath)
    if (-not [string]::Equals($fullOld, $fullNew, [System.StringComparison]::OrdinalIgnoreCase)) {
        return [pscustomobject]@{ State = 'reused'; Current = $current; Reason = 'executable image changed' }
    }
    return [pscustomobject]@{ State = 'same'; Current = $current; Reason = 'creation time and image match' }
}

function Stop-RunDescendants {
    # Only the recorded descendants of the PID this script launched. Kill by PID
    # after re-proving creation time + same image; never by process name, never
    # an unverified process. AppDir is deliberately not used: the real Pi may be
    # node.exe outside the run directory, but it is still our recorded child.
    param([object[]]$Descendants)
    $result = [pscustomobject]@{
        Cleaned = @()
        Remaining = @()
        Gone = @()
        Reused = @()
        Unproven = @()
        Failed = @()
    }
    $cleaned = New-Object System.Collections.Generic.List[object]
    foreach ($child in $Descendants) {
        $state = Test-RecordedProcess -Recorded $child
        if ($state.State -eq 'gone') { $result.Gone += $child; continue }
        if ($state.State -eq 'reused') {
            $result.Reused += [pscustomobject]@{ ProcessId = $child.ProcessId; Name = $child.Name; Reason = $state.Reason }
            continue
        }
        if ($state.State -eq 'unverified') {
            $result.Unproven += [pscustomobject]@{ ProcessId = $child.ProcessId; Name = $child.Name; Reason = $state.Reason }
            continue
        }
        try {
            Stop-Process -Id ([int]$child.ProcessId) -Force -ErrorAction Stop
            $cleaned.Add($child)
        } catch {
            $result.Failed += [pscustomobject]@{ ProcessId = $child.ProcessId; Name = $child.Name; Reason = $_.Exception.Message }
        }
    }
    if ($cleaned.Count -gt 0) { Start-Sleep -Milliseconds 500 }
    $remaining = New-Object System.Collections.Generic.List[object]
    foreach ($child in $cleaned) {
        $state = Test-RecordedProcess -Recorded $child
        if ($state.State -eq 'same' -or $state.State -eq 'unverified') { $remaining.Add($child) }
    }
    $result.Cleaned = $cleaned.ToArray()
    $result.Remaining = $remaining.ToArray()
    return $result
}

function Add-CleanupChecks {
    param([object]$Result, [object]$Cleanup, [string]$Prefix)
    $remainingNames = @($Cleanup.Remaining | ForEach-Object { [string]$_.Name } | Select-Object -Unique)
    $unprovenNames = @($Cleanup.Unproven | ForEach-Object { '{0}:{1} ({2})' -f $_.ProcessId, $_.Name, $_.Reason })
    $failedNames = @($Cleanup.Failed | ForEach-Object { '{0}:{1} ({2})' -f $_.ProcessId, $_.Name, $_.Reason })
    [void](Add-DeskCheck $Result ($Prefix + ' no Pi child survives the board') `
        ($Cleanup.Remaining.Count -eq 0) ($remainingNames -join ','))
    [void](Add-DeskCheck $Result ($Prefix + ' cleanup identity is verified') `
        ($Cleanup.Unproven.Count -eq 0 -and $Cleanup.Failed.Count -eq 0) `
        (($unprovenNames + $failedNames) -join '; '))
    [void](Add-DeskNote $Result ('{0} cleanup: cleaned={1} gone={2} reused={3} unproven={4} failed={5}' -f `
        $Prefix, $Cleanup.Cleaned.Count, $Cleanup.Gone.Count, $Cleanup.Reused.Count, `
        $Cleanup.Unproven.Count, $Cleanup.Failed.Count))
    if ($Cleanup.Reused.Count -gt 0) {
        [void](Add-DeskNote $Result ('{0} reused PIDs were left untouched: {1}' -f $Prefix, `
            (($Cleanup.Reused | ForEach-Object { '{0}:{1} ({2})' -f $_.ProcessId, $_.Name, $_.Reason }) -join '; ')))
    }
}

function Test-SameImagePath {
    param([string]$Left, [string]$Right)
    if (-not $Left -or -not $Right) { return $false }
    try {
        return [string]::Equals([System.IO.Path]::GetFullPath($Left), [System.IO.Path]::GetFullPath($Right),
            [System.StringComparison]::OrdinalIgnoreCase)
    } catch {
        return $false
    }
}

function Format-ProcessRecord {
    param([object]$Record)
    if ($null -eq $Record) { return '<none observed>' }
    return ('pid={0} name={1} created={2} image={3}' -f $Record.ProcessId, $Record.Name,
        $Record.CreationTicks, $Record.ExecutablePath)
}

function Merge-ProcessRecords {
    # Union of recorded identities deduplicated by PID + creation time, so the
    # cleanup set also covers a child that was seen alive but exited before the
    # board closed.
    param([object[]]$First, [object[]]$Second)
    $merged = New-Object System.Collections.Generic.List[object]
    $seen = @{}
    foreach ($record in (@($First) + @($Second))) {
        if ($null -eq $record) { continue }
        $key = '{0}|{1}' -f $record.ProcessId, $record.CreationTicks
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true
        $merged.Add($record)
    }
    return $merged.ToArray()
}

function Get-BoardChildProcesses {
    param([int]$RootProcessId)
    if ($RootProcessId -le 0) { return @() }
    return @(Get-ProcessDescendants -RootProcessId $RootProcessId)
}

function Wait-BoardChildImage {
    # Positive control: the board under test must really own a live child whose
    # image path is the one we copied. An empty descendant set is not evidence
    # of "nothing left over", so this records the identity that the cleanup has
    # to re-prove (PID + creation time + image) before it can stop anything.
    param([int]$RootProcessId, [string]$ImagePath, [int]$TimeoutSeconds = 15)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $seen = @()
    while ((Get-Date) -lt $deadline) {
        $seen = @(Get-BoardChildProcesses -RootProcessId $RootProcessId)
        $match = @($seen | Where-Object { Test-SameImagePath -Left $_.ExecutablePath -Right $ImagePath })
        if ($match.Count -gt 0) {
            return [pscustomobject]@{ Found = $true; Child = $match[0]; Subtree = $seen }
        }
        if ($RootProcessId -gt 0 -and -not (Test-DeskTestAlive -ProcessId $RootProcessId)) { break }
        Start-Sleep -Milliseconds 300
    }
    return [pscustomobject]@{ Found = $false; Child = $null; Subtree = $seen }
}

function Find-ProcessByImagePath {
    # Read-only, system-wide metadata scan for an image that belongs to this run
    # (the copied fake Pi). Used only for the enabled=0 negative control; a hit
    # is reported, never killed (killing stays limited to recorded descendants).
    param([string]$ImagePath)
    $found = New-Object System.Collections.Generic.List[object]
    if (-not $ImagePath) { return $found.ToArray() }
    $all = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Select-Object ProcessId, ParentProcessId, Name, CreationDate, ExecutablePath)
    foreach ($process in $all) {
        $path = ''
        try { $path = [string]$process.ExecutablePath } catch { }
        if (-not (Test-SameImagePath -Left $path -Right $ImagePath)) { continue }
        $found.Add([pscustomobject]@{
            ProcessId       = [int]$process.ProcessId
            ParentProcessId = [int]$process.ParentProcessId
            Name            = [string]$process.Name
            CreationTicks   = (Get-CreationTicks $process.CreationDate)
            ExecutablePath  = $path
        })
    }
    return $found.ToArray()
}

function Watch-DeskProcessWindows {
    # Every visible window this PID owns while it lives, including #32770 error
    # modals. enabled=0 uses this so a modal cannot be filtered away by the
    # board class name and then pass as "no window".
    param([int]$ProcessId, [int]$TimeoutSeconds = 6)
    if ($ProcessId -le 0) { return @() }   # 0 would mean "every window"
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $windows = New-Object System.Collections.Generic.List[object]
    $seen = @{}
    while ((Get-Date) -lt $deadline) {
        foreach ($window in @(Get-DeskWindowList -ProcessId $ProcessId)) {
            if (-not $window.Visible) { continue }
            $key = '{0}' -f $window.Hwnd
            if ($seen.ContainsKey($key)) { continue }
            $seen[$key] = $true
            $windows.Add($window)
        }
        if (-not (Test-DeskTestAlive -ProcessId $ProcessId)) { break }
        Start-Sleep -Milliseconds 150
    }
    return $windows.ToArray()
}

function Measure-ProcessSet {
    # Working set (MB) and CPU (ms) for a recorded process set over an explicit
    # wall-clock interval; CpuPct treats one logical core as 100%. A process
    # that is not provably the same one, or that exits inside the window, is
    # reported as SamplingFailed with a reason instead of a silent 0 MB.
    param([object[]]$Records, [int]$Seconds = 3)
    $samples = New-Object System.Collections.Generic.List[object]
    foreach ($record in @($Records)) {
        if ($null -eq $record) { continue }
        $failure = ''
        $state = Test-RecordedProcess -Recorded $record
        if ($state.State -ne 'same') { $failure = 'identity: ' + $state.Reason }
        $proc = $null
        $cpu0 = 0.0
        $ws0 = 0.0
        if (-not $failure) {
            $proc = Get-Process -Id ([int]$record.ProcessId) -ErrorAction SilentlyContinue
            if (-not $proc) {
                $failure = 'process vanished before sampling'
            } else {
                try {
                    $proc.Refresh()
                    $cpu0 = $proc.TotalProcessorTime.TotalMilliseconds
                    $ws0 = $proc.WorkingSet64 / 1MB
                } catch {
                    $failure = 'stat read failed: ' + $_.Exception.Message
                }
            }
        }
        if ($failure) {
            $samples.Add([pscustomobject]@{
                ProcessId = $record.ProcessId; Name = $record.Name; SamplingFailed = $true
                Reason = $failure; WSMB = $null; WSMBEnd = $null
                CpuMs = $null; CpuPct = $null; Seconds = 0
            })
            continue
        }
        Start-Sleep -Seconds $Seconds
        $stateEnd = Test-RecordedProcess -Recorded $record
        $procEnd = Get-Process -Id ([int]$record.ProcessId) -ErrorAction SilentlyContinue
        if (-not $procEnd -or $stateEnd.State -ne 'same') {
            $reason = 'exited inside the sample window'
            if ($procEnd) { $reason = 'identity: ' + $stateEnd.Reason }
            $samples.Add([pscustomobject]@{
                ProcessId = $record.ProcessId; Name = $record.Name; SamplingFailed = $true
                Reason = ('{0} (ws at start {1:F1}MB)' -f $reason, $ws0)
                WSMB = [math]::Round($ws0, 1); WSMBEnd = $null
                CpuMs = $null; CpuPct = $null; Seconds = 0
            })
            continue
        }
        $cpu1 = 0.0
        $ws1 = 0.0
        try {
            $procEnd.Refresh()
            $cpu1 = $procEnd.TotalProcessorTime.TotalMilliseconds
            $ws1 = $procEnd.WorkingSet64 / 1MB
        } catch {
            $samples.Add([pscustomobject]@{
                ProcessId = $record.ProcessId; Name = $record.Name; SamplingFailed = $true
                Reason = 'stat read failed after sampling: ' + $_.Exception.Message
                WSMB = [math]::Round($ws0, 1); WSMBEnd = $null
                CpuMs = $null; CpuPct = $null; Seconds = 0
            })
            continue
        }
        $delta = $cpu1 - $cpu0
        $samples.Add([pscustomobject]@{
            ProcessId = $record.ProcessId; Name = $record.Name; SamplingFailed = $false
            Reason = ''
            WSMB    = [math]::Round($ws0, 1)
            WSMBEnd = [math]::Round($ws1, 1)
            CpuMs   = [math]::Round($delta, 1)
            CpuPct  = [math]::Round(100.0 * $delta / ($Seconds * 1000.0), 2)
            Seconds = $Seconds
        })
    }
    return $samples.ToArray()
}

function Find-ByteSequence {
    param([byte[]]$Haystack, [byte[]]$Needle)
    if ($null -eq $Haystack -or $null -eq $Needle -or $Needle.Length -eq 0) { return $false }
    $limit = $Haystack.Length - $Needle.Length
    for ($i = 0; $i -le $limit; $i++) {
        $match = $true
        for ($j = 0; $j -lt $Needle.Length; $j++) {
            if ($Haystack[$i + $j] -ne $Needle[$j]) { $match = $false; break }
        }
        if ($match) { return $true }
    }
    return $false
}

function Test-DeskTestHook {
    param([string]$Path, [string]$Needle)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    # The hook names are wide string literals in C++, so search both UTF-16LE
    # and UTF-8; a plain ASCII byte search would never match L"DESK_TEST_...".
    if (Find-ByteSequence -Haystack $bytes -Needle ([System.Text.Encoding]::Unicode.GetBytes($Needle))) { return $true }
    if (Find-ByteSequence -Haystack $bytes -Needle ([System.Text.Encoding]::UTF8.GetBytes($Needle))) { return $true }
    return $false
}

function Initialize-DeskAssistantLaunch {
    # Local copy of the detached launch that keeps the process handle, so a board
    # that never shows its window can still be asked for its real exit code, and
    # that can capture the child's stdout/stderr into evidence files. desk-testlib
    # keeps its own untouched DeskLauncher.
    if ('DeskAssistantLaunch' -as [type]) { return }
    Add-Type -ReferencedAssemblies 'System' -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class DeskAssistantLaunch {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2;
        public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION {
        public IntPtr hProcess, hThread;
        public int dwProcessId, dwThreadId;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct SECURITY_ATTRIBUTES {
        public int nLength;
        public IntPtr lpSecurityDescriptor;
        public int bInheritHandle;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcess(string lpApplicationName, string lpCommandLine,
        IntPtr lpProcessAttributes, IntPtr lpThreadAttributes, bool bInheritHandles,
        uint dwCreationFlags, IntPtr lpEnvironment, string lpCurrentDirectory,
        ref STARTUPINFO si, out PROCESS_INFORMATION pi);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFileW(string name, uint desiredAccess, uint shareMode,
        ref SECURITY_ATTRIBUTES attributes, uint creationDisposition, uint flagsAndAttributes,
        IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);

    [DllImport("kernel32.dll")]
    static extern IntPtr GetStdHandle(int which);

    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);

    static readonly Dictionary<int, IntPtr> Tracked = new Dictionary<int, IntPtr>();
    static readonly object TrackedLock = new object();

    static IntPtr BuildEnvironment(string[] names, string[] values) {
        SortedDictionary<string, string> map = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (System.Collections.DictionaryEntry entry in Environment.GetEnvironmentVariables()) {
            string key = entry.Key as string;
            if (key == null) continue;
            map[key] = entry.Value as string ?? "";
        }
        for (int i = 0; i < names.Length && i < values.Length; ++i) {
            if (string.IsNullOrEmpty(names[i])) continue;
            map[names[i]] = values[i] ?? "";
        }
        string block = "";
        foreach (KeyValuePair<string, string> pair in map) block += pair.Key + "=" + pair.Value + "\0";
        block += "\0";
        byte[] bytes = Encoding.Unicode.GetBytes(block);
        IntPtr memory = Marshal.AllocHGlobal(bytes.Length);
        Marshal.Copy(bytes, 0, memory, bytes.Length);
        return memory;
    }

    public static int StartObserved(string exe, string workingDirectory, string[] names,
                                    string[] values, string stdoutPath, string stderrPath) {
        STARTUPINFO si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        PROCESS_INFORMATION pi = new PROCESS_INFORMATION();
        // DETACHED_PROCESS (no inherited console) + CREATE_UNICODE_ENVIRONMENT.
        const uint flags = 0x00000008u | 0x00000400u | 0x08000000u;
        SECURITY_ATTRIBUTES sa = new SECURITY_ATTRIBUTES();
        sa.nLength = Marshal.SizeOf(typeof(SECURITY_ATTRIBUTES));
        sa.bInheritHandle = 1;
        IntPtr outHandle = IntPtr.Zero;
        IntPtr errHandle = IntPtr.Zero;
        bool redirect = false;
        try {
            try {
                if (!string.IsNullOrEmpty(stdoutPath))
                    outHandle = CreateFileW(stdoutPath, 0x40000000u, 3u, ref sa, 2u, 0x80u, IntPtr.Zero);
                if (!string.IsNullOrEmpty(stderrPath))
                    errHandle = CreateFileW(stderrPath, 0x40000000u, 3u, ref sa, 2u, 0x80u, IntPtr.Zero);
                redirect = outHandle != IntPtr.Zero || errHandle != IntPtr.Zero;
            } catch {
                redirect = false;   // diagnostics are best effort, never fatal
            }
            if (redirect) {
                si.dwFlags = 0x00000100;   // STARTF_USESTDHANDLES
                si.hStdInput = GetStdHandle(-10);
                si.hStdOutput = outHandle != IntPtr.Zero ? outHandle : errHandle;
                si.hStdError = errHandle != IntPtr.Zero ? errHandle : outHandle;
            }
            IntPtr env = BuildEnvironment(names, values);
            try {
                string commandLine = "\"" + exe + "\"";
                if (!CreateProcess(exe, commandLine, IntPtr.Zero, IntPtr.Zero, redirect, flags, env,
                                   workingDirectory, ref si, out pi)) {
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                }
            } finally {
                Marshal.FreeHGlobal(env);
            }
        } finally {
            if (outHandle != IntPtr.Zero) CloseHandle(outHandle);
            if (errHandle != IntPtr.Zero) CloseHandle(errHandle);
        }
        CloseHandle(pi.hThread);
        lock (TrackedLock) { Tracked[pi.dwProcessId] = pi.hProcess; }
        return pi.dwProcessId;
    }

    public static bool TryGetExit(int pid, out bool exited, out int exitCode) {
        exited = false;
        exitCode = -1;
        IntPtr handle;
        lock (TrackedLock) {
            if (!Tracked.TryGetValue(pid, out handle)) return false;
        }
        uint code;
        if (!GetExitCodeProcess(handle, out code)) return false;
        exitCode = unchecked((int)code);
        exited = code != 259u;   // STILL_ACTIVE
        return true;
    }

    public static void ForgetTracked(int pid) {
        IntPtr handle;
        lock (TrackedLock) {
            if (!Tracked.TryGetValue(pid, out handle)) return;
            Tracked.Remove(pid);
            CloseHandle(handle);
        }
    }
}
'@ -ErrorAction Stop
}

function Get-ExeStaticImports {
    # Minimal read-only PE import-table walk (x86/x64). Returns the imported
    # module names plus a reason when the image or table cannot be read. No
    # process is started and no file is written.
    param([string]$Path)
    $result = [pscustomobject]@{ Imports = @(); Error = '' }
    $stream = $null
    $reader = $null
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        $reader = New-Object System.IO.BinaryReader($stream)
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        $stream.Position = $peOffset + 4
        [void]$reader.ReadUInt16()
        $sectionCount = $reader.ReadUInt16()
        $stream.Position = $peOffset + 20
        $optionalSize = $reader.ReadUInt16()
        $optionalStart = $peOffset + 24
        $stream.Position = $optionalStart
        $magic = $reader.ReadUInt16()
        $importDirOffset = $optionalStart + 96
        if ($magic -eq 0x20B) { $importDirOffset = $optionalStart + 112 }
        $stream.Position = $importDirOffset + 8
        $importRva = $reader.ReadInt32()
        if ($importRva -le 0) { return $result }
        $sections = New-Object System.Collections.Generic.List[object]
        $sectionStart = $optionalStart + $optionalSize
        for ($i = 0; $i -lt $sectionCount; $i++) {
            $stream.Position = $sectionStart + $i * 40 + 8
            $virtualSize = $reader.ReadInt32()
            $virtualAddress = $reader.ReadInt32()
            [void]$reader.ReadInt32()
            $rawPointer = $reader.ReadInt32()
            $sections.Add([pscustomobject]@{
                VA = $virtualAddress
                VSize = [math]::Max($virtualSize, 1)
                Raw = $rawPointer
            })
        }
        $toOffset = {
            param([int]$rva)
            foreach ($section in $sections) {
                if ($rva -ge $section.VA -and $rva -lt ($section.VA + $section.VSize)) {
                    return $section.Raw + ($rva - $section.VA)
                }
            }
            return -1
        }
        $names = New-Object System.Collections.Generic.List[string]
        $descriptor = & $toOffset $importRva
        for ($index = 0; $descriptor -ge 0 -and $index -lt 256; $index++) {
            $stream.Position = $descriptor + $index * 20
            [void]$reader.ReadInt32()
            [void]$reader.ReadInt32()
            [void]$reader.ReadInt32()
            $nameRva = $reader.ReadInt32()
            if ($nameRva -le 0) { break }
            $nameOffset = & $toOffset $nameRva
            if ($nameOffset -lt 0) { break }
            $stream.Position = $nameOffset
            $name = New-Object System.Text.StringBuilder
            for ($k = 0; $k -lt 260; $k++) {
                $byte = $reader.ReadByte()
                if ($byte -eq 0) { break }
                [void]$name.Append([char]$byte)
            }
            $names.Add($name.ToString())
        }
        $result.Imports = $names.ToArray()
    } catch {
        $result.Error = $_.Exception.Message
    } finally {
        if ($reader) { $reader.Close() }
        if ($stream) { $stream.Dispose() }
    }
    return $result
}

function Copy-ExeRuntimeDependencies {
    # The QA copy must be self-contained: every non-API-set import that exists
    # next to the source exe (a same-build MinGW runtime such as libgcc_s_*.dll,
    # libstdc++-6.dll, libwinpthread-1.dll) is copied beside the copy. PATH is
    # never extended and nothing is installed; a missing dependency is reported
    # instead of failing the launch with a bare 0xC0000135.
    param([string]$SourceExe, [string]$DestinationDirectory)
    $sourceDirectory = Split-Path -Parent $SourceExe
    $scan = Get-ExeStaticImports -Path $SourceExe
    $rows = New-Object System.Collections.Generic.List[object]
    foreach ($name in @($scan.Imports)) {
        $isApiSet = ($name -match '^(?i)(api-ms-|ext-ms-)')
        $inSource = $false
        $inDestination = $false
        $inSystem = $false
        $copied = $false
        try { $inSource = Test-Path -LiteralPath (Join-Path $sourceDirectory $name) -PathType Leaf } catch { }
        try { $inDestination = Test-Path -LiteralPath (Join-Path $DestinationDirectory $name) -PathType Leaf } catch { }
        if ($env:SystemRoot) {
            try { $inSystem = Test-Path -LiteralPath (Join-Path $env:SystemRoot ('System32\' + $name)) -PathType Leaf } catch { }
        }
        if ($inSource -and -not $inDestination) {
            try {
                Copy-Item -LiteralPath (Join-Path $sourceDirectory $name) `
                    -Destination (Join-Path $DestinationDirectory $name) -Force -ErrorAction Stop
                $inDestination = $true
                $copied = $true
            } catch { }
        }
        $rows.Add([pscustomobject]@{
            Name        = $name
            IsApiSet    = $isApiSet
            SourceDir   = $inSource
            Copied      = $copied
            Destination = $inDestination
            System32    = $inSystem
            Resolved    = ($isApiSet -or $inDestination -or $inSystem)
        })
    }
    return $rows.ToArray()
}

function Add-RuntimeDependencyChecks {
    # Pre-flight evidence for the copied image: which imports resolved from the
    # app dir, from System32 or as API sets, and whether anything is missing.
    param([object]$Result, [string]$Prefix)
    $rows = @($script:RuntimeDependencies)
    if ($rows.Count -eq 0) {
        [void](Add-DeskNote $Result ($Prefix + ' runtime dependency scan produced no rows'))
        return
    }
    $copied = @($rows | Where-Object { $_.Copied } | ForEach-Object { $_.Name } | Select-Object -Unique)
    $unresolved = @($rows | Where-Object { -not $_.Resolved } | ForEach-Object { $_.Name } | Select-Object -Unique)
    $summary = @($rows | ForEach-Object {
        $state = 'unresolved'
        if ($_.Copied) { $state = 'copied' }
        elseif ($_.Destination) { $state = 'app-dir' }
        elseif ($_.System32) { $state = 'system32' }
        elseif ($_.IsApiSet) { $state = 'api-set' }
        '{0}={1}' -f $_.Name, $state
    }) -join '; '
    [void](Add-DeskNote $Result ('{0} runtime imports: {1}' -f $Prefix, $summary))
    [void](Add-DeskCheck $Result ($Prefix + ' board runtime dependencies resolve') `
        ($unresolved.Count -eq 0) ($unresolved -join ','))
    if ($copied.Count -gt 0) {
        [void](Add-DeskNote $Result ('{0} same-build runtime DLLs copied into the QA app dir: {1}' -f `
            $Prefix, ($copied -join ',')))
    }
}

function Save-WindowDiagnostic {
    # Non-assertive evidence for a board that did not produce the expected
    # window: every window this PID owns (any class, any visibility), the size
    # of the desktop window set, process liveness and the real exit code of the
    # copy we launched, plus stopwatch vs wall-clock elapsed (a mismatch means
    # the machine clock stepped). Metadata only; no screenshot, no messages.
    param(
        [object]$Run,
        [int]$ProcessId,
        [string]$ClassName,
        [double]$ElapsedSeconds,
        [datetime]$WallStart,
        [datetime]$WallEnd,
        [string]$Path
    )
    $windows = @()
    $desktopCount = 0
    try { $windows = @(Get-DeskWindowList -ProcessId $ProcessId) } catch { }
    try { $desktopCount = @(Get-DeskWindowList).Count } catch { }
    $exited = $null
    $exitCode = $null
    try {
        $trackedExited = $false
        $trackedCode = 0
        if ([DeskAssistantLaunch]::TryGetExit($ProcessId, [ref]$trackedExited, [ref]$trackedCode)) {
            $exited = $trackedExited
            $exitCode = $trackedCode
        }
    } catch { }
    $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    $record = [pscustomobject]@{
        processId      = $ProcessId
        expectedClass  = $ClassName
        seenClasses    = @($windows | ForEach-Object { [string]$_.Class } | Select-Object -Unique)
        windowCount    = $windows.Count
        desktopWindows = $desktopCount
        windows        = @($windows | ForEach-Object {
            [pscustomobject]@{ Class = $_.Class; Title = $_.Title; Visible = $_.Visible
                               X = $_.X; Y = $_.Y; W = $_.W; H = $_.H }
        })
        processAlive   = ($null -ne $process)
        exitKnown      = ($null -ne $exited)
        exited         = $exited
        exitCode       = $exitCode
        elapsedSeconds = [math]::Round($ElapsedSeconds, 3)
        wallSeconds    = [math]::Round(($WallEnd - $WallStart).TotalSeconds, 3)
        wallStart      = $WallStart.ToString('HH:mm:ss.fff')
        wallEnd        = $WallEnd.ToString('HH:mm:ss.fff')
        imagePath      = (Get-DeskTestPath -ProcessId $ProcessId)
        isolation      = [pscustomobject]@{ class = [string]$Run.ClassName; appDir = [string]$Run.AppDir }
    }
    if ($Path) { Write-JsonFile -Path $Path -Value $record }
    return $record
}

function New-RunLayout {
    param([string]$Label, [string]$SuffixPrefix)
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $dir = Join-Path $script:QaRoot ('{0}-{1}' -f $Label, $stamp)
    $counter = 1
    while (Test-Path -LiteralPath $dir) {
        $dir = Join-Path $script:QaRoot ('{0}-{1}-{2}' -f $Label, $stamp, $counter)
        $counter++
    }
    $layout = [pscustomobject]@{
        Dir             = $dir
        AppDir          = Join-Path $dir 'app'
        ConfigDir        = Join-Path $dir 'config'
        AbortConfigDir   = Join-Path $dir 'config-abort'
        DisabledConfigDir = Join-Path $dir 'config-disabled'
        FixtureDir      = Join-Path $dir 'fixture'
        DshRoot         = Join-Path $dir 'fixture\dsh'
        ProjectDir      = Join-Path $dir 'fixture\project'
        DisplayFile     = Join-Path $dir 'fixture\display.txt'
        FakeExe         = Join-Path $dir 'app\fake-pi.exe'
        FakeLog         = Join-Path $dir 'evidence\fake-pi.log'
        AbortFakeLog    = Join-Path $dir 'evidence\fake-pi-abort.log'
        DisabledFakeLog = Join-Path $dir 'evidence\fake-pi-disabled.log'
        AppData         = Join-Path $dir 'profile\appdata'
        LocalAppData    = Join-Path $dir 'profile\localappdata'
        Temp            = Join-Path $dir 'profile\temp'
        UserProfile     = Join-Path $dir 'profile\user'
        EvidenceDir     = Join-Path $dir 'evidence'
        Suffix          = ('asq{0}' -f ((Get-Random -Minimum 10000 -Maximum 99999)))
        AbortSuffix     = ('asq{0}' -f ((Get-Random -Minimum 10000 -Maximum 99999)))
        DisabledSuffix  = ('asq{0}' -f ((Get-Random -Minimum 10000 -Maximum 99999)))
        ClassName       = ''
        Exe             = ''
    }
    foreach ($path in @($layout.Dir, $layout.AppDir, $layout.ConfigDir, $layout.AbortConfigDir,
                        $layout.DisabledConfigDir, $layout.DshRoot, $layout.ProjectDir, $layout.AppData,
                        $layout.LocalAppData, $layout.Temp, $layout.UserProfile,
                        $layout.EvidenceDir)) {
        New-Item -ItemType Directory -Path $path -Force | Out-Null
    }
    $layout.ClassName = 'wg.desk-' + $layout.Suffix
    $layout.Exe = Join-Path $layout.AppDir 'desk.exe'
    return $layout
}

function Write-DshFixture {
    param([object]$Run)
    $workspace = [ordered]@{
        tables = [ordered]@{
            workspaces = [ordered]@{
                'fixture-project-0001' = [ordered]@{
                    title     = 'M1 fixture project'
                    path      = $Run.ProjectDir
                    updatedAt = '2026-11-08T00:00:00Z'
                }
            }
        }
    }
    $session = [ordered]@{
        identity = [ordered]@{ cwd = $Run.ProjectDir }
        rows     = [ordered]@{
            title = [ordered]@{ val = 'Fixture session' }
            turnOutline = [ordered]@{
                val = [ordered]@{
                    turns = @(
                        [ordered]@{ response = 'M1 fixture conclusion: 中文来源结论。' }
                    )
                }
            }
        }
    }
    $storages = Join-Path $Run.DshRoot 'storages'
    $sessions = Join-Path $storages 'session_projcache\sessions'
    New-Item -ItemType Directory -Path $sessions -Force | Out-Null
    Write-JsonFile -Path (Join-Path $storages 'workspace.json') -Value $workspace
    # The file stem becomes the source id: fixture -> dsh-session-fixture.
    Write-JsonFile -Path (Join-Path $sessions 'fixture.json') -Value $session
    $lines = @('work=0,0,2560,1392', 'dpi=96')
    Write-Utf8File -Path $Run.DisplayFile -Text (($lines -join "`n") + "`n")
}

function Write-AssistantConfig {
    param(
        [string]$ConfigDir,
        [bool]$Fake,
        [string]$FakeExe = '',
        [string]$Mode = 'qa',
        [int]$Enabled = 1
    )
    $values = [ordered]@{
        enabled            = [string]$Enabled
        hotkey             = 'Ctrl+Alt+F10'
        idle_stop_minutes  = '1'
    }
    if ($Fake) {
        $values['pi_command'] = $FakeExe
        $values['provider'] = ''
        $values['model'] = ''
        $values['key_ref'] = ''
    } else {
        $values['pi_command'] = ''
        $values['provider'] = $Provider
        $values['model'] = $Model
        $values['key_ref'] = $KeyRef
    }
    # Write assistant.ini here instead of desk-testlib's Write-DeskIniFile: that
    # helper prepends a BOM character *and* writes with Encoding.Unicode (which
    # emits its own BOM), so the file starts with two BOMs. wg::Store::Open strips
    # only one, which left the first key as "\uFEFFenabled" - the fixture's
    # enabled=0 was therefore invisible and the app kept its enabled=1 default
    # (the enabled=0 negative control launched a full board). One BOM, key intact.
    $ini = New-Object System.Text.StringBuilder
    foreach ($key in $values.Keys) { [void]$ini.Append("$key=$($values[$key])" + [char]10) }
    [System.IO.File]::WriteAllText((Join-Path $ConfigDir 'assistant.ini'), $ini.ToString(),
        [System.Text.Encoding]::Unicode)
    if ($Fake) {
        $fakeEnv = @(
            ('FAKE_PI_MODE={0}' -f $Mode),
            ('FAKE_PI_LOG={0}' -f $(if ($Mode -eq 'abort') { $script:LastRun.AbortFakeLog } else { $script:LastRun.FakeLog }))
        )
        Write-Utf8File -Path (Join-Path $ConfigDir 'fake.env') -Text (($fakeEnv -join "`n") + "`n")
    }
}

function Start-BoardProcess {
    param([object]$Run, [hashtable]$Environment)
    $names = [string[]]@($Environment.Keys)
    $values = [string[]]::new($names.Count)
    for ($i = 0; $i -lt $names.Count; $i++) { $values[$i] = [string]$Environment[$names[$i]] }
    Initialize-DeskAssistantLaunch
    # One evidence log pair per instance (the offline suite starts three), so the
    # stdout/stderr of a board that dies before its window is still readable.
    $tag = 'board'
    if ($Environment.ContainsKey('DESK_TEST_INSTANCE')) {
        $tag = 'board-' + ([string]$Environment['DESK_TEST_INSTANCE'])
    }
    $stdoutPath = Join-Path $Run.EvidenceDir ($tag + '.stdout.txt')
    $stderrPath = Join-Path $Run.EvidenceDir ($tag + '.stderr.txt')
    return [DeskAssistantLaunch]::StartObserved($Run.Exe, $Run.AppDir, $names, $values,
        $stdoutPath, $stderrPath)
}

function Wait-BoardHwnd {
    # Wait-DeskWindow hands back a single-element array that PowerShell unrolls to
    # one window object, and .Count on a Windows PowerShell 5.1 PSObject is empty
    # (not 1). The old "$window -and $window.Count -gt 0" guard was therefore
    # false for a real, visible board window: this function returned $null within
    # milliseconds and the suite reported "no window" although the board was
    # running. @() keeps the array contract, and a failed wait now leaves a
    # diagnostic file instead of a bare "no window".
    # ClassName overrides Run.ClassName: the abort instance runs under its own
    # AbortSuffix, so waiting for the main board class made a live abort window
    # look like "no window" and skipped every Esc check behind it.
    param([object]$Run, [int]$ProcessId, [int]$TimeoutSeconds = 25, [string]$DiagnosticPath = '',
          [string]$ClassName = '')
    $expectedClass = if ($ClassName) { $ClassName } else { [string]$Run.ClassName }
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $wallStart = Get-Date
    $windows = @(Wait-DeskWindow -ProcessId $ProcessId -ClassName $expectedClass -TimeoutSeconds $TimeoutSeconds)
    $watch.Stop()
    $wallEnd = Get-Date
    if ($windows.Count -gt 0) { return $windows[0] }
    if ($DiagnosticPath -and $ProcessId -gt 0) {
        try {
            [void](Save-WindowDiagnostic -Run $Run -ProcessId $ProcessId -ClassName $expectedClass `
                -ElapsedSeconds $watch.Elapsed.TotalSeconds -WallStart $wallStart -WallEnd $wallEnd `
                -Path $DiagnosticPath)
        } catch { }
    }
    return $null
}

function Close-BoardProcess {
    param([object]$Run, [int]$ProcessId, [IntPtr]$Hwnd)
    $closed = [pscustomobject]@{ Graceful = $false; Killed = $false; PathVerified = $false }
    if ($ProcessId -le 0) { return $closed }
    $path = Get-DeskTestPath -ProcessId $ProcessId
    if ($path) {
        $full = [System.IO.Path]::GetFullPath($path)
        $root = [System.IO.Path]::GetFullPath($Run.AppDir)
        if ($full.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
            $closed.PathVerified = $true
        }
    }
    if (-not $closed.PathVerified) {
        return $closed
    }
    $result = Close-DeskBoard -ProcessId $ProcessId -Hwnd $Hwnd -Root $Run.AppDir -TimeoutMs 12000
    $closed.Graceful = [bool]$result.Graceful
    $closed.Killed = [bool]$result.Killed
    return $closed
}

function New-OfflineEnvironment {
    param([object]$Run, [string]$ConfigDir, [string]$Suffix, [string]$FakeLog, [string]$Mode,
          [bool]$RegisterAssistantHotkey = $false)
    return @{
        DESK_TEST_INSTANCE        = $Suffix
        DESK_TEST_CONFIG_DIR      = $ConfigDir
        DESK_TEST_DSH_ROOT        = $Run.DshRoot
        DESK_TEST_ASSISTANT_ONLY  = '1'
        DESK_TEST_DISPLAY_FILE    = $Run.DisplayFile
        APPDATA                   = $Run.AppData
        LOCALAPPDATA              = $Run.LocalAppData
        TEMP                      = $Run.Temp
        TMP                       = $Run.Temp
        USERPROFILE               = $Run.UserProfile
        HOME                      = $Run.UserProfile
        FAKE_PI_MODE              = $Mode
        FAKE_PI_LOG               = $FakeLog
        FAKE_PI_RELEASE           = ''
        DESK_TEST_REGISTER_ASSISTANT_HOTKEY = $(if ($RegisterAssistantHotkey) { '1' } else { '' })
    }
}

function New-RealEnvironment {
    param([object]$Run, [string]$Suffix, [bool]$RegisterAssistantHotkey = $false)
    # Deliberate boundary (not an OS sandbox): USERPROFILE/LOCALAPPDATA/TEMP/
    # APPDATA/HOME are inherited so the trusted `node`/`pi.cmd` launcher, the
    # official provider extension and the DSH key_ref resolve; only the desk
    # config, DSH root and display fixture are isolated here. Engine already
    # keeps Pi's private agent dir, pi-work and the session file under
    # DESK_TEST_CONFIG_DIR and passes the key only in the child environment, so
    # this function must never carry a credential value itself.
    return @{
        DESK_TEST_INSTANCE       = $Suffix
        DESK_TEST_CONFIG_DIR     = $Run.ConfigDir
        DESK_TEST_DSH_ROOT       = $Run.DshRoot
        DESK_TEST_ASSISTANT_ONLY = '1'
        DESK_TEST_DISPLAY_FILE   = $Run.DisplayFile
        # Never let an inherited fake-Pi environment leak into a real run.
        FAKE_PI_MODE             = ''
        FAKE_PI_LOG              = ''
        FAKE_PI_RELEASE          = ''
        DESK_TEST_REGISTER_ASSISTANT_HOTKEY = $(if ($RegisterAssistantHotkey) { '1' } else { '' })
    }
}

function Get-AssistantRoot {
    param([object]$Run)
    $ini = Read-DeskIniFile -Path (Join-Path $Run.ConfigDir 'desk.ini')
    if ($null -eq $ini) { return $null }
    if (-not $ini.Contains('card.assistant.x') -or -not $ini.Contains('card.assistant.y')) { return $null }
    return [pscustomobject]@{
        X = [int]$ini['card.assistant.x']
        Y = [int]$ini['card.assistant.y']
        W = 568
        H = 500
    }
}

function Save-PrintWindowEvidence {
    # PrintWindow only. There is no screen-capture fallback in this script.
    param([IntPtr]$Hwnd, [int]$Width, [int]$Height, [string]$Path)
    $bitmap = [DeskTestImage]::PrintBitmap($Hwnd, $Width, $Height)
    if ($null -eq $bitmap) {
        return [pscustomobject]@{ Present = $false; Blank = $true; Stats = 'null'; Path = '' }
    }
    try {
        $blank = [DeskTestImage]::IsBlank($bitmap)
        $stats = [DeskTestImage]::Stats($bitmap)
        if (-not $blank) {
            $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
        }
        return [pscustomobject]@{ Present = $true; Blank = $blank; Stats = $stats; Path = $Path }
    } finally {
        $bitmap.Dispose()
    }
}

function Get-OwnCanvasFrame {
    # Synchronous own-canvas export. Returns an open Bitmap; the caller disposes.
    param([IntPtr]$Hwnd, [object]$Run, [string]$ConfigDir, [int]$Slot, [string]$Label)
    $frame = Join-Path $ConfigDir ('assistant-test-frame-{0}.bmp' -f $Slot)
    Remove-Item -LiteralPath $frame -Force -ErrorAction SilentlyContinue
    $result = [DeskTestNative]::SendMessage($Hwnd, $script:CanvasMessage, [IntPtr]$Slot, [IntPtr]::Zero)
    if ($result.ToInt64() -ne 1) {
        return [pscustomobject]@{ Bitmap = $null; Path = $frame; Stats = ''; Ink = 0; Export = $false }
    }
    if (-not (Test-Path -LiteralPath $frame)) {
        return [pscustomobject]@{ Bitmap = $null; Path = $frame; Stats = ''; Ink = 0; Export = $false }
    }
    try {
        $bitmap = [System.Drawing.Bitmap]::FromFile($frame)
    } catch {
        return [pscustomobject]@{ Bitmap = $null; Path = $frame; Stats = ''; Ink = 0; Export = $false }
    }
    $stats = [DeskTestImage]::Stats($bitmap)
    $ink = [DeskTestImage]::CountInkPixels($bitmap, 0, $bitmap.Height, 0, $bitmap.Width, 40)
    $copy = New-Object System.Drawing.Bitmap($bitmap)
    $bitmap.Dispose()
    $copy.Save((Join-Path $Run.EvidenceDir ($Label + '.png')),
        [System.Drawing.Imaging.ImageFormat]::Png)
    return [pscustomobject]@{ Bitmap = $copy; Path = $frame; Stats = $stats; Ink = $ink; Export = $true }
}

function Send-DeskEscape {
    param([IntPtr]$Hwnd)
    [void][DeskTestNative]::PostKey($Hwnd, 0x1B)   # VK_ESCAPE
}

function Send-AssistantQuestion {
    param([IntPtr]$Hwnd, [string]$Text)
    Send-DeskText -Hwnd $Hwnd -Text $Text
    Start-Sleep -Milliseconds 150
    Send-DeskEnter -Hwnd $Hwnd
}

function Send-PhysicalCtrlAltF10 {
    # Only used behind the explicit -PhysicalHotkeyProbe gate. This is the
    # independent probe chord; the default Ctrl+Alt+A is never pressed.
    if (-not ('DeskHotkeyInput' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class DeskHotkeyInput {
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    static void Down(byte vk) { keybd_event(vk, 0, 0, UIntPtr.Zero); }
    static void Up(byte vk) { keybd_event(vk, 0, 2, UIntPtr.Zero); }
    public static void CtrlAltF10() {
        Down(0x11); Down(0x12); Down(0x79);
        System.Threading.Thread.Sleep(60);
        Up(0x79); Up(0x12); Up(0x11);
    }
}
'@
    }
    [DeskHotkeyInput]::CtrlAltF10()
}

function Get-FactState {
    param([string]$Path, [string]$Needle)
    $items = Read-JsonLines -Path $Path
    $active = 0
    $seen = 0
    foreach ($item in $items) {
        if ([string]$item.text -notlike ('*' + $Needle + '*')) { continue }
        $seen++
        if ([string]$item.status -ne 'deleted') { $active++ }
    }
    return [pscustomobject]@{ Seen = $seen; Active = $active }
}

function Get-FactById {
    param([string]$Path, [string]$Id)
    foreach ($item in (Read-JsonLines -Path $Path)) {
        if ([string]$item.id -eq $Id) { return $item }
    }
    return $null
}

function Get-MemoryCard {
    param([object]$Run)
    $ini = Read-DeskIniFile -Path (Join-Path $Run.ConfigDir 'desk.ini')
    if ($null -eq $ini -or -not $ini.Contains('card.memory.x')) { return $null }
    return [pscustomobject]@{
        X = [int]$ini['card.memory.x']
        Y = [int]$ini['card.memory.y']
        W = 340
        Expanded = (Get-DeskIniInt $ini 'card.memory.expanded' 0)
        Visible = (Get-DeskIniInt $ini 'card.memory.visible' 1)
    }
}

function Set-AssistantCollapsed {
    # Test fixture for the summon check: a fresh config starts with the assistant
    # card expanded, so summoning it changes nothing on screen. Collapse it via
    # the title-bar details toggle (same geometry the memory card uses), and prove
    # the collapsed state in the board's own desk.ini, so the summon has a real
    # state transition to observe. Only this run's isolated desk.ini is touched.
    param([object]$Run, [IntPtr]$Hwnd)
    $ini = Read-DeskIniFile -Path (Join-Path $Run.ConfigDir 'desk.ini')
    if ($null -eq $ini -or -not $ini.Contains('card.assistant.x')) { return $false }
    if ((Get-DeskIniInt $ini 'card.assistant.expanded' 1) -eq 0) { return $true }
    [void](Send-DeskClick -Hwnd $Hwnd -X ([int]$ini['card.assistant.x'] + 568 - 18) `
        -Y ([int]$ini['card.assistant.y'] + 15))
    $deskIni = Join-Path $Run.ConfigDir 'desk.ini'
    $after = Wait-DeskIniCondition -Path $deskIni -TimeoutSeconds 3 -Predicate {
        param($values) (Get-DeskIniInt $values 'card.assistant.expanded' 1) -eq 0
    }
    return ($null -ne $after -and (Get-DeskIniInt $after 'card.assistant.expanded' 1) -eq 0)
}

function Set-MemoryExpanded {
    param([object]$Run, [IntPtr]$Hwnd)
    $card = Get-MemoryCard -Run $Run
    if ($null -eq $card -or $card.Visible -ne 1) { return $false }
    if ($card.Expanded -eq 1) { return $true }
    # The title bar's right edge is the details toggle (board.cpp).
    [void](Send-DeskClick -Hwnd $Hwnd -X ($card.X + $card.W - 18) -Y ($card.Y + 15))
    $deskIni = Join-Path $Run.ConfigDir 'desk.ini'
    $after = Wait-DeskIniCondition -Path $deskIni -TimeoutSeconds 3 -Predicate {
        param($values) (Get-DeskIniInt $values 'card.memory.expanded' 0) -eq 1
    }
    return ($null -ne $after -and (Get-DeskIniInt $after 'card.memory.expanded' 0) -eq 1)
}

function Get-ActiveFactRows {
    param([string]$Path)
    $items = Read-JsonLines -Path $Path
    $active = New-Object System.Collections.Generic.List[object]
    foreach ($item in $items) {
        if ([string]$item.status -eq 'deleted') { continue }
        if (-not ([string]$item.text)) { continue }
        $active.Add($item)
    }
    # Mirrors MemoryStore::Facts(): pinned first, then updated/created desc,
    # then id ascending. Do not assume the engine's fixture import is empty.
    return @($active.ToArray() | Sort-Object -Property @(
        @{ Expression = { if ([bool]$_.pinned) { 0 } else { 1 } } },
        @{ Expression = { -[long]$_.updated } },
        @{ Expression = { -[long]$_.created } },
        @{ Expression = { [string]$_.id } }
    ))
}

function Get-FactRowIndex {
    param([string]$Path, [string]$Id = '', [string]$Needle = '')
    $rows = @(Get-ActiveFactRows -Path $Path)
    for ($i = 0; $i -lt $rows.Count; $i++) {
        if ($Id -and [string]$rows[$i].id -eq $Id) { return $i }
        if (-not $Id -and $Needle -and ([string]$rows[$i].text).Contains($Needle)) { return $i }
    }
    return -1
}

function Select-MemoryFactRow {
    param([object]$Run, [IntPtr]$Hwnd, [int]$RowIndex)
    if ($RowIndex -lt 0 -or $RowIndex -ge 17) { return $false }
    $card = Get-MemoryCard -Run $Run
    if ($null -eq $card -or $card.Visible -ne 1) { return $false }
    # Memory body starts at card.y + header 30 + top pad 8; row pitch 32.
    $x = $card.X + 100
    $y = $card.Y + 38 + 4 + $RowIndex * 32 + 15
    # Never leak Send-DeskClick's bool into this function's return value: a
    # two-value return would arrive as Object[] and blow up a caller's [bool]
    # parameter (this is what aborted the 20261108-232801 run before it could
    # write a result file).
    [void](Send-DeskClick -Hwnd $Hwnd -X $x -Y $y)
    Start-Sleep -Milliseconds 300
    return $true
}

function Wait-ProcessWindowClass {
    param([int]$ProcessId, [string]$ClassName, [int]$TimeoutSeconds = 5)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $window = @(Get-DeskWindowList -ProcessId $ProcessId | Where-Object {
            $_.Visible -and $_.Class -eq $ClassName
        })
        if ($window.Count -gt 0) { return $window[0] }
        Start-Sleep -Milliseconds 100
    }
    return $null
}

function Invoke-PopupMenuItem {
    # Right-click this card, then activate the matching item in the popup menu
    # through MSAA. Only popup windows owned by our PID are considered.
    param([int]$ProcessId, [IntPtr]$Hwnd, [int]$X, [int]$Y, [string]$Pattern)
    [void][DeskTestNative]::PostMessage($Hwnd, 0x0204, [IntPtr]1, [DeskTestNative]::MakeLParam($X, $Y))
    [void][DeskTestNative]::PostMessage($Hwnd, 0x0205, [IntPtr]::Zero, [DeskTestNative]::MakeLParam($X, $Y))
    $menu = $null
    $deadline = (Get-Date).AddSeconds(3)
    while ((Get-Date) -lt $deadline -and -not $menu) {
        $menu = @(Get-DeskWindowList -ProcessId $ProcessId | Where-Object {
            $_.Class -eq '#32768'
        }) | Select-Object -First 1
        if (-not $menu) { Start-Sleep -Milliseconds 100 }
    }
    if (-not $menu) { return $false }
    if (-not ('AssistantMenu' -as [type])) {
        # Menu commands can open a modal wg.Prompt / MessageBox on the board UI
        # thread. Invoke MSAA on a background thread so the caller can drive the
        # modal while accDoDefaultAction is still blocked inside the command.
        $accAssembly = [System.Reflection.Assembly]::LoadWithPartialName('Accessibility')
        if (-not $accAssembly) { return $false }
        Add-Type -ReferencedAssemblies $accAssembly.Location -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Threading;
using Accessibility;
public static class AssistantMenu {
    [DllImport("oleacc.dll")]
    static extern int AccessibleObjectFromWindow(IntPtr hwnd, uint id, ref Guid iid,
        [MarshalAs(UnmanagedType.IUnknown)] out object obj);
    public static bool Invoke(IntPtr menu, string pattern) {
        bool[] found = new bool[1];
        ManualResetEvent ready = new ManualResetEvent(false);
        Thread thread = new Thread(delegate() {
            try {
                Guid iid = new Guid("618736E0-3C3D-11CF-810C-00AA00389B71");
                object obj = null;
                if (AccessibleObjectFromWindow(menu, 4294967292u, ref iid, out obj) != 0 || obj == null) return;
                IAccessible acc = (IAccessible)obj;
                int count = acc.accChildCount;
                for (int child = 1; child <= count; ++child) {
                    string name = "";
                    try { name = acc.get_accName(child); } catch { continue; }
                    if (!Glob(pattern, name)) continue;
                    found[0] = true;
                    ready.Set();
                    try { acc.accDoDefaultAction(child); } catch { }
                    return;
                }
            } catch {
            } finally {
                ready.Set();
            }
        });
        thread.IsBackground = true;
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        ready.WaitOne(3000);
        return found[0];
    }
    static bool Glob(string pattern, string text) {
        if (pattern.EndsWith("*"))
            return text.StartsWith(pattern.Substring(0, pattern.Length - 1), StringComparison.OrdinalIgnoreCase);
        return string.Equals(pattern, text, StringComparison.OrdinalIgnoreCase);
    }
}
'@
    }
    try {
        if ([AssistantMenu]::Invoke($menu.Hwnd, $Pattern)) { return $true }
    } catch {
    }
    # Never leave a modal popup open if the item was not found.
    [void][DeskTestNative]::PostKey($menu.Hwnd, 0x1B)
    return $false
}

function Set-PromptTextAndSubmit {
    param([int]$ProcessId, [string]$Initial, [string]$Text)
    $prompt = Wait-ProcessWindowClass -ProcessId $ProcessId -ClassName 'wg.Prompt' -TimeoutSeconds 5
    if (-not $prompt) { return $false }
    for ($i = 0; $i -lt $Initial.Length; $i++) {
        [void][DeskTestNative]::PostKey($prompt.Hwnd, 0x08)   # VK_BACK
        Start-Sleep -Milliseconds 10
    }
    Send-DeskText -Hwnd $prompt.Hwnd -Text $Text
    Start-Sleep -Milliseconds 150
    Send-DeskEnter -Hwnd $prompt.Hwnd
    Start-Sleep -Milliseconds 500
    return $true
}

function Get-MemoryCardRoi {
    # ROI of the MEMORY card inside an own-canvas frame. desk.ini card.*.x/y are
    # board-canvas coordinates, i.e. the same space as the exported frame, so the
    # card rect maps 1:1. The height covers the header plus the visible fact rows
    # (rows start at +42 with a 32px pitch, see Select-MemoryFactRow) and is
    # clamped to the frame. Returns $null when the geometry is not available, so
    # the caller reports "ROI unavailable" instead of guessing coordinates.
    param([object]$Run, [object]$Frame)
    if ($null -eq $Frame -or $null -eq $Frame.Bitmap) { return $null }
    $card = Get-MemoryCard -Run $Run
    if ($null -eq $card -or $card.Visible -ne 1) { return $null }
    $factRows = @(Get-ActiveFactRows -Path (Join-Path $Run.ConfigDir 'assistant-facts.jsonl')).Count
    $rows = [math]::Max($factRows, 1)
    if ($rows -gt 17) { $rows = 17 }
    $x = [int]$card.X
    $y = [int]$card.Y
    if ($x -lt 0 -or $y -lt 0) { return $null }
    if ($x -ge $Frame.Bitmap.Width -or $y -ge $Frame.Bitmap.Height) { return $null }
    $w = [math]::Min([int]$card.W, $Frame.Bitmap.Width - $x)
    $h = [math]::Min((42 + 32 * $rows + 20), $Frame.Bitmap.Height - $y)
    if ($w -lt 80 -or $h -lt 40) { return $null }
    return [pscustomobject]@{ X = $x; Y = $y; W = $w; H = $h; Rows = $rows }
}

function Invoke-DialogButton {
    param([IntPtr]$Dialog, [int]$ButtonId)
    if (-not ('AssistantWin32' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class AssistantWin32 {
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr dialog, int id);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
}
'@
    }
    $button = [AssistantWin32]::GetDlgItem($Dialog, $ButtonId)
    if ($button -ne [IntPtr]::Zero) {
        [void][AssistantWin32]::SendMessage($button, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) # BM_CLICK
    } else {
        [void][AssistantWin32]::SendMessage($Dialog, 0x0111, [IntPtr]$ButtonId, [IntPtr]::Zero) # WM_COMMAND
    }
}

function Restart-OfflineBoard {
    param([object]$Run, [int]$ProcessId, [IntPtr]$Hwnd)
    $descendants = @()
    if ($ProcessId -gt 0) { $descendants = @(Get-ProcessDescendants -RootProcessId $ProcessId) }
    $closed = Close-BoardProcess -Run $Run -ProcessId $ProcessId -Hwnd $Hwnd
    Start-Sleep -Milliseconds 800
    $cleanup = Stop-RunDescendants -Descendants $descendants
    $newPid = Start-BoardProcess -Run $Run -Environment (New-OfflineEnvironment `
        -Run $Run -ConfigDir $Run.ConfigDir -Suffix $Run.Suffix -FakeLog $Run.FakeLog -Mode 'qa')
    $newWindow = Wait-BoardHwnd -Run $Run -ProcessId $newPid
    $newHwnd = [IntPtr]::Zero
    if ($newWindow) {
        $newHwnd = $newWindow.Hwnd
        # Re-focus the assistant for the local memory commands that follow a
        # restart. Global test hotkeys stay off (this is the WM_HOTKEY path).
        [void][DeskTestNative]::PostMessage($newHwnd, $script:HotkeyMessage,
            [IntPtr]$script:HotkeyAssistantId, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 600
        $deskIni = Join-Path $Run.ConfigDir 'desk.ini'
        [void](Wait-IniStable -Path $deskIni -TimeoutSeconds 3)
    }
    return [pscustomobject]@{
        Pid = $newPid
        Hwnd = $newHwnd
        Window = $newWindow
        Closed = $closed
        Cleanup = $cleanup
        Survivors = @($cleanup.Remaining)
    }
}

function Invoke-OfflineSuite {
    param([object]$Run)
    $result = New-DeskResult
    $result.Name = 'assistant-offline'
    Add-RuntimeDependencyChecks -Result $result -Prefix 'offline'
    $regBefore = Get-DeskRunKeySnapshot
    $prodBefore = Get-ProductionConfigSnapshot

    $pid1 = 0
    $hwnd1 = [IntPtr]::Zero
    $fakeChildRecord = $null
    try {
        $pid1 = Start-BoardProcess -Run $Run -Environment (New-OfflineEnvironment `
            -Run $Run -ConfigDir $Run.ConfigDir -Suffix $Run.Suffix -FakeLog $Run.FakeLog `
            -Mode 'qa' -RegisterAssistantHotkey ([bool]$PhysicalHotkeyProbe))
        $windowDiagnostic = Join-Path $Run.EvidenceDir 'window-diagnostic.json'
        $window = Wait-BoardHwnd -Run $Run -ProcessId $pid1 -DiagnosticPath $windowDiagnostic
        if (-not $window) {
            [void](Add-DeskNote $result ('offline window diagnostic: ' + (Read-Utf8File -Path $windowDiagnostic)))
            [void](Add-DeskCheck $result 'offline board window starts' $false 'no window')
            return $result
        }
        $hwnd1 = $window.Hwnd
        [void](Add-DeskCheck $result 'offline board window starts' `
            ($window.Class -eq $Run.ClassName) $window.Class)
        [void](Add-DeskCheck $result 'board image is the copied test image' `
            ((Get-DeskTestPath -ProcessId $pid1) -like ($Run.AppDir + '*')))
        $startRunChanges = @(Compare-DeskRunKey -Before $regBefore -After (Get-DeskRunKeySnapshot))
        [void](Add-DeskCheck $result 'HKCU Run is unchanged at start' ($startRunChanges.Count -eq 0) `
            ($startRunChanges -join '; '))

        $deskIni = Join-Path $Run.ConfigDir 'desk.ini'
        $ini = Wait-DeskIniCondition -Path $deskIni -TimeoutSeconds 10 -Predicate {
            param($values)
            $values.Contains('card.assistant.visible') -and $values.Contains('card.memory.visible')
        }
        [void](Add-DeskCheck $result 'assistant and memory cards are registered' `
            ($null -ne $ini -and $ini.Contains('card.assistant.visible') -and $ini.Contains('card.memory.visible')))
        if ($ini) {
            $hasOther = $false
            foreach ($key in $ini.Keys) {
                if ($key -like 'card.account.*' -or $key -like 'card.servers.*' -or $key -like 'card.ps.*') {
                    $hasOther = $true
                }
            }
            [void](Add-DeskCheck $result 'DESK_TEST_ASSISTANT_ONLY hides Account/Servers/PS' (-not $hasOther))
        }

        $shot = Save-PrintWindowEvidence -Hwnd $hwnd1 -Width $window.W -Height $window.H `
            -Path (Join-Path $Run.EvidenceDir 'printwindow-start.png')
        [void](Add-DeskNote $result ('PrintWindow start: present={0} blank={1} {2}' -f `
            $shot.Present, $shot.Blank, $shot.Stats))
        if ($shot.Blank) {
            [void](Add-DeskNote $result 'PrintWindow is blank for this layered window; own-canvas frames are the pixel evidence')
        }

        $pre = Get-OwnCanvasFrame -Hwnd $hwnd1 -Run $Run -ConfigDir $Run.ConfigDir -Slot 0 -Label 'canvas-pre'
        [void](Add-DeskCheck $result 'own-canvas export returns a non-empty bitmap' `
            ($pre.Export -and $null -ne $pre.Bitmap -and $pre.Ink -gt 50) $pre.Stats)

        # Summon path. A fresh config starts with the assistant card expanded, so a
        # summon would be a visual no-op. The fixture collapses the card first and
        # proves that collapsed state in the board's own desk.ini; only then is the
        # "before" frame taken, so the frame difference measures the expansion
        # instead of nothing. Default test instances register no global hotkeys, so
        # the quiet path posts the same WM_HOTKEY the system would deliver. The
        # optional physical path is gated by DESK_TEST_REGISTER_ASSISTANT_HOTKEY and
        # uses only Ctrl+Alt+F10.
        $collapsedFixture = Set-AssistantCollapsed -Run $Run -Hwnd $hwnd1
        [void](Add-DeskCheck $result 'assistant collapsed fixture is applied before summon' $collapsedFixture)
        $preSummon = Get-OwnCanvasFrame -Hwnd $hwnd1 -Run $Run -ConfigDir $Run.ConfigDir -Slot 0 -Label 'canvas-collapsed'
        if ($PhysicalHotkeyProbe) {
            Send-PhysicalCtrlAltF10
            [void](Add-DeskNote $result 'physical probe used Ctrl+Alt+F10 only; default Ctrl+Alt+A was not pressed')
        } else {
            [void][DeskTestNative]::PostMessage($hwnd1, $script:HotkeyMessage,
                [IntPtr]$script:HotkeyAssistantId, [IntPtr]::Zero)
        }
        Start-Sleep -Milliseconds 600
        $summonState = Wait-DeskIniCondition -Path (Join-Path $Run.ConfigDir 'desk.ini') -TimeoutSeconds 3 -Predicate {
            param($values) (Get-DeskIniInt $values 'card.assistant.expanded' 0) -eq 1
        }
        $summonExpanded = ($null -ne $summonState -and (Get-DeskIniInt $summonState 'card.assistant.expanded' 0) -eq 1)
        [void](Add-DeskCheck $result 'summon expands the assistant card (persisted state)' `
            ($collapsedFixture -and $summonExpanded) ('expanded=' + [int]$summonExpanded))
        $summon = Get-OwnCanvasFrame -Hwnd $hwnd1 -Run $Run -ConfigDir $Run.ConfigDir -Slot 1 -Label 'canvas-summon'
        $summonDiff = 0.0
        if ($preSummon.Bitmap -and $summon.Bitmap) {
            $summonDiff = [DeskTestImage]::DiffFraction($preSummon.Bitmap, $summon.Bitmap, 0, 0,
                [math]::Min($preSummon.Bitmap.Width, $summon.Bitmap.Width),
                [math]::Min($preSummon.Bitmap.Height, $summon.Bitmap.Height), 20)
        }
        [void](Add-DeskCheck $result 'summon message expands/focuses the assistant' `
            ($summon.Export -and $summonDiff -gt 0.001) ('diff={0:F4}' -f $summonDiff))
        if ($summon.Bitmap) { $summon.Bitmap.Dispose(); $summon.Bitmap = $null }
        if ($preSummon.Bitmap) { $preSummon.Bitmap.Dispose(); $preSummon.Bitmap = $null }

        $question = '请用中文回答：M1 离线验收的问题'
        Send-AssistantQuestion -Hwnd $hwnd1 -Text $question
        $episodePath = Join-Path $Run.ConfigDir 'assistant-episodes.jsonl'
        [void](Wait-FilePredicate -Path $episodePath -TimeoutSeconds 35 -Predicate {
            param($text)
            $text.Contains('"question"')
        })
        $episodes = @(Read-JsonLines -Path $episodePath)
        $episode = $null
        if ($episodes.Count -gt 0) { $episode = $episodes[$episodes.Count - 1] }
        [void](Add-DeskCheck $result 'Chinese question produces an episode' ($null -ne $episode))
        if ($episode) {
            [void](Add-DeskCheck $result 'episode records the Chinese question' `
                ([string]$episode.question -like ('*' + $question + '*')))
            [void](Add-DeskCheck $result 'streamed answer contains Chinese text' `
                ([string]$episode.answer -match '[\u4e00-\u9fff]'))
            [void](Add-DeskCheck $result 'citation resolves to the DSH fixture' `
                (([string]$episode.sources).Contains('dsh-session-fixture')))
        }

        # Positive child control: the fake Pi must be a live child of *this*
        # board, with PID > 0 and the image we copied into the run. Its identity
        # is recorded here so the cleanup below re-proves the same process
        # instead of treating an empty descendant set as evidence.
        $fakeChild = Wait-BoardChildImage -RootProcessId $pid1 -ImagePath $Run.FakeExe -TimeoutSeconds 15
        if ($fakeChild.Found) { $fakeChildRecord = $fakeChild.Child }
        [void](Add-DeskCheck $result 'fake Pi runs as a live child of this board' `
            ($null -ne $fakeChildRecord -and $fakeChildRecord.ProcessId -gt 0) `
            ('observed: ' + (Format-ProcessRecord -Record $fakeChildRecord)))
        [void](Add-DeskNote $result ('fake Pi subtree before close: {0}' -f `
            (($fakeChild.Subtree | ForEach-Object { '{0}:{1}' -f $_.ProcessId, $_.Name }) -join ',')))

        $answer = Get-OwnCanvasFrame -Hwnd $hwnd1 -Run $Run -ConfigDir $Run.ConfigDir -Slot 1 -Label 'canvas-answer'
        $answerDiff = 0.0
        if ($pre.Bitmap -and $answer.Bitmap) {
            $answerDiff = [DeskTestImage]::DiffFraction($pre.Bitmap, $answer.Bitmap, 0, 0,
                [math]::Min($pre.Bitmap.Width, $answer.Bitmap.Width),
                [math]::Min($pre.Bitmap.Height, $answer.Bitmap.Height), 20)
        }
        [void](Add-DeskCheck $result 'own-canvas frames show the streamed answer' `
            ($answer.Export -and $answerDiff -gt 0.001) ('diff={0:F4}' -f $answerDiff))

        # FAKE_PI_LOG records command *types* only (never prompt text or payload).
        $fakeLog = Read-Utf8File -Path $Run.FakeLog
        [void](Add-DeskCheck $result 'fake RPC received the prompt command' `
            ($fakeLog.ToLowerInvariant().Contains('prompt')))
        [void](Add-DeskCheck $result 'fake RPC did not receive an abort for a completed ask' `
            (-not $fakeLog.ToLowerInvariant().Contains('abort')))

        $costPath = Join-Path $Run.ConfigDir 'assistant-cost.ini'
        $cost = Read-DeskIniFile -Path $costPath
        $tokens = 0
        if ($cost) {
            foreach ($key in $cost.Keys) {
                if ($key -like 'day.*.tokens') { $tokens += [int]$cost[$key] }
            }
        }
        [void](Add-DeskCheck $result 'cost file records tokens for the answer' ($tokens -gt 0) "tokens=$tokens")

        # Local deterministic commands must not reach Pi. The engine imports
        # fixture project facts at startup, so every row lookup below is by our
        # own id/text, never by assuming an empty initial list.
        $factsPath = Join-Path $Run.ConfigDir 'assistant-facts.jsonl'
        $logBeforeLocal = (Read-Utf8File -Path $Run.FakeLog).Length
        $tempText = '临时暗号-' + (Get-Random -Minimum 10000 -Maximum 99999)
        Send-AssistantQuestion -Hwnd $hwnd1 -Text ('记住：' + $tempText)
        $localFact = Wait-FilePredicate -Path $factsPath -TimeoutSeconds 12 -Predicate {
            param($text) $text.Contains($tempText)
        }
        [void](Add-DeskCheck $result 'local 记住 writes a fact' ($localFact.Contains($tempText)))
        Send-AssistantQuestion -Hwnd $hwnd1 -Text ('忘掉：' + $tempText)
        [void](Wait-FilePredicate -Path $factsPath -TimeoutSeconds 10 -Predicate {
            param($text) $text.Contains($tempText) -and $text.Contains('"deleted"')
        })
        $forgotten = Get-FactState -Path $factsPath -Needle $tempText
        [void](Add-DeskCheck $result 'local 忘掉 soft-deletes the fact' `
            ($forgotten.Seen -gt 0 -and $forgotten.Active -eq 0))

        # Keep the local clear command covered before the menu-driven CRUD test.
        $localClearMarker = '本地清空暗号-' + (Get-Random -Minimum 10000 -Maximum 99999)
        Send-AssistantQuestion -Hwnd $hwnd1 -Text ('记住：' + $localClearMarker)
        [void](Wait-FilePredicate -Path $factsPath -TimeoutSeconds 12 -Predicate {
            param($text) $text.Contains($localClearMarker)
        })
        Send-AssistantQuestion -Hwnd $hwnd1 -Text '清空记忆'
        [void](Wait-FilePredicate -Path $factsPath -TimeoutSeconds 10 -Predicate {
            param($text) -not $text.Trim()
        })
        [void](Wait-FilePredicate -Path $episodePath -TimeoutSeconds 10 -Predicate {
            param($text) -not $text.Trim()
        })
        $cleared = @(Read-JsonLines -Path $factsPath)
        $clearedEpisodes = @(Read-JsonLines -Path $episodePath)
        [void](Add-DeskCheck $result 'local 清空记忆 clears facts and episodes' `
            ($cleared.Count -eq 0 -and $clearedEpisodes.Count -eq 0))

        # Create the one fact the MEMORY menu will pin/edit/delete/clear.
        $primaryText = '菜单验收暗号-' + (Get-Random -Minimum 10000 -Maximum 99999)
        Send-AssistantQuestion -Hwnd $hwnd1 -Text ('记住：' + $primaryText)
        $localPrimary = Wait-FilePredicate -Path $factsPath -TimeoutSeconds 12 -Predicate {
            param($text) $text.Contains($primaryText)
        }
        $primaryRows = @(Get-ActiveFactRows -Path $factsPath)
        $primaryId = ''
        foreach ($item in $primaryRows) {
            if (([string]$item.text).Contains($primaryText)) { $primaryId = [string]$item.id; break }
        }
        [void](Add-DeskCheck $result 'local primary fact exists for the menu test' ($localPrimary.Contains($primaryText) -and $primaryId))
        $logAfterLocal = (Read-Utf8File -Path $Run.FakeLog).Length
        [void](Add-DeskCheck $result 'local memory commands do not call Pi' ($logAfterLocal -eq $logBeforeLocal))

        # MEMORY card geometry and expansion. Only our own popup windows are
        # addressed; no facts file is edited by the script.
        [void](Wait-IniStable -Path (Join-Path $Run.ConfigDir 'desk.ini') -TimeoutSeconds 3)
        $memoryExpanded = Set-MemoryExpanded -Run $Run -Hwnd $hwnd1
        [void](Add-DeskCheck $result 'MEMORY card expands for row selection' $memoryExpanded)
        $memStart = Get-OwnCanvasFrame -Hwnd $hwnd1 -Run $Run -ConfigDir $Run.ConfigDir -Slot 0 -Label 'memory-start'
        [void](Add-DeskCheck $result 'MEMORY starting own-canvas frame is non-empty' `
            ($memStart.Export -and $null -ne $memStart.Bitmap -and $memStart.Ink -gt 50) $memStart.Stats)

        $row = Get-FactRowIndex -Path $factsPath -Id $primaryId
        [void](Add-DeskCheck $result 'primary fact row is selectable' ($row -ge 0 -and $row -lt 17) ('row=' + $row))
        $selected = Select-MemoryFactRow -Run $Run -Hwnd $hwnd1 -RowIndex $row
        [void](Add-DeskCheck $result 'primary fact row click selects it' $selected)
        $pinInvoked = Invoke-PopupMenuItem -ProcessId $pid1 -Hwnd $hwnd1 `
            -X ((Get-MemoryCard -Run $Run).X + 80) -Y ((Get-MemoryCard -Run $Run).Y + 15) -Pattern 'Pin fact*'
        [void](Add-DeskCheck $result 'MEMORY Pin menu item activates through MSAA' $pinInvoked)
        [void](Wait-FilePredicate -Path $factsPath -TimeoutSeconds 10 -Predicate {
            param($text) $text.Contains('"id":"' + $primaryId + '"') -and $text.Contains('"pinned":true')
        })
        $pinnedRecord = Get-FactById -Path $factsPath -Id $primaryId
        [void](Add-DeskCheck $result 'Pin fact persists pinned=true' ($null -ne $pinnedRecord -and [bool]$pinnedRecord.pinned))
        $memPin = Get-OwnCanvasFrame -Hwnd $hwnd1 -Run $Run -ConfigDir $Run.ConfigDir -Slot 1 -Label 'memory-pin'
        # Pass/fail is judged on the MEMORY card ROI (the row marker/order is a
        # card-local change); the full-board fraction stays as context only. The
        # 0.0005 threshold is unchanged - the board is 1280x1600, so the same
        # card-local change is a much larger fraction of the card area.
        $pinRoi = Get-MemoryCardRoi -Run $Run -Frame $memPin
        $pinRoiDiff = 0.0
        if ($null -ne $pinRoi -and $memStart.Bitmap -and $memPin.Bitmap) {
            $pinRoiDiff = [DeskTestImage]::DiffFraction($memStart.Bitmap, $memPin.Bitmap,
                $pinRoi.X, $pinRoi.Y, $pinRoi.W, $pinRoi.H, 20)
        }
        $pinDiff = 0.0
        if ($memStart.Bitmap -and $memPin.Bitmap) {
            $pinDiff = [DeskTestImage]::DiffFraction($memStart.Bitmap, $memPin.Bitmap, 0, 0,
                [math]::Min($memStart.Bitmap.Width, $memPin.Bitmap.Width),
                [math]::Min($memStart.Bitmap.Height, $memPin.Bitmap.Height), 20)
        }
        $pinDetail = 'ROI unavailable'
        if ($null -ne $pinRoi) {
            $pinArea = [int]$pinRoi.W * [int]$pinRoi.H
            $pinDetail = 'card roi={0}x{1}@({2},{3}) rows={4} changedPx~{5} of {6} diff={7:F4}' -f `
                $pinRoi.W, $pinRoi.H, $pinRoi.X, $pinRoi.Y, $pinRoi.Rows,
                [int][math]::Round($pinRoiDiff * $pinArea), $pinArea, $pinRoiDiff
        }
        [void](Add-DeskCheck $result 'Pin changes the MEMORY card ROI' `
            ($null -ne $pinRoi -and $pinRoiDiff -gt 0.0005) $pinDetail)
        [void](Add-DeskNote $result ('Pin full-board frame diff (context only): {0:F4}' -f $pinDiff))
        if ($memPin.Bitmap) { $memPin.Bitmap.Dispose(); $memPin.Bitmap = $null }

        $row = Get-FactRowIndex -Path $factsPath -Id $primaryId
        $selected = Select-MemoryFactRow -Run $Run -Hwnd $hwnd1 -RowIndex $row
        [void](Add-DeskCheck $result 'edited fact row is selectable' ($row -ge 0 -and $selected))
        $editInvoked = Invoke-PopupMenuItem -ProcessId $pid1 -Hwnd $hwnd1 `
            -X ((Get-MemoryCard -Run $Run).X + 80) -Y ((Get-MemoryCard -Run $Run).Y + 15) -Pattern 'Edit fact*'
        [void](Add-DeskCheck $result 'MEMORY Edit menu item activates through MSAA' $editInvoked)
        # The edited text changes the FIRST glyphs of the row (the row renders a
        # prefix and truncates the tail), so the visible change is at the start
        # the user reads, not in a clipped tail.
        $editedText = ('可见替换-{0}' -f (Get-Random -Minimum 10000 -Maximum 99999))
        $promptOk = Set-PromptTextAndSubmit -ProcessId $pid1 -Initial $primaryText -Text $editedText
        [void](Add-DeskCheck $result 'EDIT prompt accepts text and Enter' $promptOk)
        [void](Wait-FilePredicate -Path $factsPath -TimeoutSeconds 10 -Predicate {
            param($text) $text.Contains($editedText)
        })
        $editedRecord = Get-FactById -Path $factsPath -Id $primaryId
        [void](Add-DeskCheck $result 'Edit fact keeps the id and writes the new text' `
            ($null -ne $editedRecord -and [string]$editedRecord.text -eq $editedText))
        $memEdit = Get-OwnCanvasFrame -Hwnd $hwnd1 -Run $Run -ConfigDir $Run.ConfigDir -Slot 1 -Label 'memory-edit'
        $editRoi = Get-MemoryCardRoi -Run $Run -Frame $memEdit
        $editRoiDiff = 0.0
        if ($null -ne $editRoi -and $memStart.Bitmap -and $memEdit.Bitmap) {
            $editRoiDiff = [DeskTestImage]::DiffFraction($memStart.Bitmap, $memEdit.Bitmap,
                $editRoi.X, $editRoi.Y, $editRoi.W, $editRoi.H, 20)
        }
        $editDiff = 0.0
        if ($memStart.Bitmap -and $memEdit.Bitmap) {
            $editDiff = [DeskTestImage]::DiffFraction($memStart.Bitmap, $memEdit.Bitmap, 0, 0,
                [math]::Min($memStart.Bitmap.Width, $memEdit.Bitmap.Width),
                [math]::Min($memStart.Bitmap.Height, $memEdit.Bitmap.Height), 20)
        }
        $editDetail = 'ROI unavailable'
        if ($null -ne $editRoi) {
            $editArea = [int]$editRoi.W * [int]$editRoi.H
            $editDetail = 'card roi={0}x{1}@({2},{3}) rows={4} changedPx~{5} of {6} diff={7:F4}' -f `
                $editRoi.W, $editRoi.H, $editRoi.X, $editRoi.Y, $editRoi.Rows,
                [int][math]::Round($editRoiDiff * $editArea), $editArea, $editRoiDiff
        }
        [void](Add-DeskCheck $result 'Edit changes the MEMORY card ROI' `
            ($null -ne $editRoi -and $editRoiDiff -gt 0.0005) $editDetail)
        [void](Add-DeskNote $result ('Edit full-board frame diff (context only): {0:F4}' -f $editDiff))
        if ($memEdit.Bitmap) { $memEdit.Bitmap.Dispose(); $memEdit.Bitmap = $null }
        if ($memStart.Bitmap) { $memStart.Bitmap.Dispose(); $memStart.Bitmap = $null }

        # Restart with the same isolated config: the pinned/edited fact must
        # still be there before the menu delete/clear checks continue.
        $restart = Restart-OfflineBoard -Run $Run -ProcessId $pid1 -Hwnd $hwnd1
        [void](Add-DeskCheck $result 'MEMORY restart closes the first instance' `
            ($restart.Closed.Graceful -or $restart.Closed.Killed))
        Add-CleanupChecks -Result $result -Cleanup $restart.Cleanup -Prefix 'MEMORY restart'
        [void](Add-DeskCheck $result 'MEMORY restart starts a new isolated window' ($restart.Hwnd -ne [IntPtr]::Zero))
        if ($restart.Hwnd -ne [IntPtr]::Zero) {
            $pid1 = $restart.Pid
            $hwnd1 = $restart.Hwnd
            $window = $restart.Window
            $persisted = Get-FactById -Path $factsPath -Id $primaryId
            [void](Add-DeskCheck $result 'pinned Edit fact survives restart' `
                ($null -ne $persisted -and [string]$persisted.text -eq $editedText -and [bool]$persisted.pinned))
            [void](Add-DeskCheck $result 'MEMORY expansion survives restart' `
                (Set-MemoryExpanded -Run $Run -Hwnd $hwnd1))
            $memRestart = Get-OwnCanvasFrame -Hwnd $hwnd1 -Run $Run -ConfigDir $Run.ConfigDir -Slot 0 -Label 'memory-restart'
            [void](Add-DeskCheck $result 'restarted MEMORY frame is non-empty' `
                ($memRestart.Export -and $null -ne $memRestart.Bitmap -and $memRestart.Ink -gt 50) $memRestart.Stats)
            if ($memRestart.Bitmap) { $memRestart.Bitmap.Dispose(); $memRestart.Bitmap = $null }

            $row = Get-FactRowIndex -Path $factsPath -Id $primaryId
            [void](Add-DeskCheck $result 'persisted fact row is selectable after restart' ($row -ge 0 -and (Select-MemoryFactRow -Run $Run -Hwnd $hwnd1 -RowIndex $row)))
            $beforeDelete = Get-OwnCanvasFrame -Hwnd $hwnd1 -Run $Run -ConfigDir $Run.ConfigDir -Slot 0 -Label 'memory-before-delete'
            $deleteInvoked = Invoke-PopupMenuItem -ProcessId $pid1 -Hwnd $hwnd1 `
                -X ((Get-MemoryCard -Run $Run).X + 80) -Y ((Get-MemoryCard -Run $Run).Y + 15) -Pattern 'Delete fact*'
            [void](Add-DeskCheck $result 'MEMORY Delete menu item activates through MSAA' $deleteInvoked)
            [void](Wait-FilePredicate -Path $factsPath -TimeoutSeconds 10 -Predicate {
                param($text) $text.Contains('"id":"' + $primaryId + '"') -and $text.Contains('"status":"deleted"')
            })
            $deletedRecord = Get-FactById -Path $factsPath -Id $primaryId
            [void](Add-DeskCheck $result 'Delete fact writes a deleted tombstone' `
                ($null -ne $deletedRecord -and [string]$deletedRecord.status -eq 'deleted'))
            $afterDelete = Get-OwnCanvasFrame -Hwnd $hwnd1 -Run $Run -ConfigDir $Run.ConfigDir -Slot 1 -Label 'memory-after-delete'
            $deleteDiff = 0.0
            if ($beforeDelete.Bitmap -and $afterDelete.Bitmap) {
                $deleteDiff = [DeskTestImage]::DiffFraction($beforeDelete.Bitmap, $afterDelete.Bitmap, 0, 0,
                    [math]::Min($beforeDelete.Bitmap.Width, $afterDelete.Bitmap.Width),
                    [math]::Min($beforeDelete.Bitmap.Height, $afterDelete.Bitmap.Height), 20)
            }
            [void](Add-DeskCheck $result 'Delete changes the own-canvas frame' ($deleteDiff -gt 0.0005) ('diff={0:F4}' -f $deleteDiff))
            if ($afterDelete.Bitmap) { $afterDelete.Bitmap.Dispose() }
            if ($beforeDelete.Bitmap) { $beforeDelete.Bitmap.Dispose() }

            # Clear all: first Cancel (facts must survive), then OK.
            # Everything since the restart was mouse-driven (rows, pin/delete
            # popups). A click on a card that does not claim the keyboard blurs
            # the assistant, and WM_CHAR is forwarded only to the focused card,
            # so the local 记住 below would otherwise type into nothing and the
            # Cancel check would find no fact. The WM_HOTKEY path is a toggle
            # (assistantSummoned_ is still true here, so it would blur instead),
            # so re-focus with a left click in the assistant card body: the
            # AssistantCard claims the keyboard for every body point.
            $focusIni = Read-DeskIniFile -Path (Join-Path $Run.ConfigDir 'desk.ini')
            if ($null -ne $focusIni -and $focusIni.Contains('card.assistant.x')) {
                [void](Send-DeskClick -Hwnd $hwnd1 -X ([int]$focusIni['card.assistant.x'] + 40) `
                    -Y ([int]$focusIni['card.assistant.y'] + 60))
                Start-Sleep -Milliseconds 400
            }
            $clearMarker = '待清空暗号-' + (Get-Random -Minimum 10000 -Maximum 99999)
            Send-AssistantQuestion -Hwnd $hwnd1 -Text ('记住：' + $clearMarker)
            $clearMarkerText = Wait-FilePredicate -Path $factsPath -TimeoutSeconds 12 -Predicate {
                param($text) $text.Contains($clearMarker)
            }
            [void](Add-DeskCheck $result 'Clear all fixture fact exists before Cancel' `
                $clearMarkerText.Contains($clearMarker))
            $clearCancelInvoked = Invoke-PopupMenuItem -ProcessId $pid1 -Hwnd $hwnd1 `
                -X ((Get-MemoryCard -Run $Run).X + 80) -Y ((Get-MemoryCard -Run $Run).Y + 15) -Pattern 'Clear all memory*'
            $cancelDialog = Wait-ProcessWindowClass -ProcessId $pid1 -ClassName '#32770' -TimeoutSeconds 5
            [void](Add-DeskCheck $result 'Clear all opens the confirm dialog' ($clearCancelInvoked -and $null -ne $cancelDialog))
            # Wait-ProcessWindowClass returns a window record, not a handle.
            if ($cancelDialog) { Invoke-DialogButton -Dialog $cancelDialog.Hwnd -ButtonId 2 }   # IDCANCEL
            Start-Sleep -Milliseconds 900
            $markerAfterCancel = Get-FactState -Path $factsPath -Needle $clearMarker
            [void](Add-DeskCheck $result 'Clear all Cancel leaves facts intact' ($markerAfterCancel.Active -gt 0))

            $beforeClear = Get-OwnCanvasFrame -Hwnd $hwnd1 -Run $Run -ConfigDir $Run.ConfigDir -Slot 0 -Label 'memory-before-clear'
            $clearOkInvoked = Invoke-PopupMenuItem -ProcessId $pid1 -Hwnd $hwnd1 `
                -X ((Get-MemoryCard -Run $Run).X + 80) -Y ((Get-MemoryCard -Run $Run).Y + 15) -Pattern 'Clear all memory*'
            $okDialog = Wait-ProcessWindowClass -ProcessId $pid1 -ClassName '#32770' -TimeoutSeconds 5
            [void](Add-DeskCheck $result 'Clear all reopens the confirm dialog' ($clearOkInvoked -and $null -ne $okDialog))
            if ($okDialog) { Invoke-DialogButton -Dialog $okDialog.Hwnd -ButtonId 1 }   # IDOK
            [void](Wait-FilePredicate -Path $factsPath -TimeoutSeconds 10 -Predicate {
                param($text) -not $text.Trim()
            })
            [void](Wait-FilePredicate -Path $episodePath -TimeoutSeconds 10 -Predicate {
                param($text) -not $text.Trim()
            })
            $clearedFacts = @(Read-JsonLines -Path $factsPath)
            $clearedEpisodes = @(Read-JsonLines -Path $episodePath)
            [void](Add-DeskCheck $result 'Clear all OK empties facts and episodes' `
                ($clearedFacts.Count -eq 0 -and $clearedEpisodes.Count -eq 0))
            $afterClear = Get-OwnCanvasFrame -Hwnd $hwnd1 -Run $Run -ConfigDir $Run.ConfigDir -Slot 1 -Label 'memory-after-clear'
            $clearDiff = 0.0
            if ($beforeClear.Bitmap -and $afterClear.Bitmap) {
                $clearDiff = [DeskTestImage]::DiffFraction($beforeClear.Bitmap, $afterClear.Bitmap, 0, 0,
                    [math]::Min($beforeClear.Bitmap.Width, $afterClear.Bitmap.Width),
                    [math]::Min($beforeClear.Bitmap.Height, $afterClear.Bitmap.Height), 20)
            }
            [void](Add-DeskCheck $result 'Clear all changes the own-canvas frame' ($clearDiff -gt 0.0005) ('diff={0:F4}' -f $clearDiff))
            if ($afterClear.Bitmap) { $afterClear.Bitmap.Dispose() }
            if ($beforeClear.Bitmap) { $beforeClear.Bitmap.Dispose() }

            # Final restart: an explicit clear must also survive a process restart
            # (the import checkpoint prevents fixture project facts from returning).
            $restart2 = Restart-OfflineBoard -Run $Run -ProcessId $pid1 -Hwnd $hwnd1
            [void](Add-DeskCheck $result 'MEMORY clear restart starts a new window' ($restart2.Hwnd -ne [IntPtr]::Zero))
            Add-CleanupChecks -Result $result -Cleanup $restart2.Cleanup -Prefix 'MEMORY clear restart'
            if ($restart2.Hwnd -ne [IntPtr]::Zero) {
                $pid1 = $restart2.Pid
                $hwnd1 = $restart2.Hwnd
                $window = $restart2.Window
                $finalFacts = @(Read-JsonLines -Path $factsPath)
                $finalEpisodes = @(Read-JsonLines -Path $episodePath)
                [void](Add-DeskCheck $result 'Clear all persists across restart' `
                    ($finalFacts.Count -eq 0 -and $finalEpisodes.Count -eq 0))
                $finalFrame = Get-OwnCanvasFrame -Hwnd $hwnd1 -Run $Run -ConfigDir $Run.ConfigDir -Slot 0 -Label 'memory-final'
                [void](Add-DeskCheck $result 'post-clear restart own-canvas frame is non-empty' `
                    ($finalFrame.Export -and $null -ne $finalFrame.Bitmap -and $finalFrame.Ink -gt 50) $finalFrame.Stats)
                if ($finalFrame.Bitmap) { $finalFrame.Bitmap.Dispose() }
            }
        }
        $logAfterMemory = (Read-Utf8File -Path $Run.FakeLog).Length
        [void](Add-DeskCheck $result 'MEMORY menu/local actions never prompt Pi' ($logAfterMemory -eq $logBeforeLocal))

        $cpu = Measure-DeskCpu -ProcessId $pid1 -Seconds 5
        $stats = Get-DeskProcessStats -ProcessId $pid1
        if ($cpu -and $stats) {
            [void](Add-DeskCheck $result 'idle CPU stays below 2%' ($cpu.CpuPct -lt 2.0) `
                ('cpu={0}% ws={1}MB' -f $cpu.CpuPct, $stats.WSMB))
            [void](Add-DeskNote $result ('idle stats: ws={0}MB private={1}MB threads={2} gdi={3} user={4}' -f `
                $stats.WSMB, $stats.PrivateMB, $stats.Threads, $stats.GDI, $stats.USER))
        }
        $descendants = @(Get-ProcessDescendants -RootProcessId $pid1)
        [void](Add-DeskNote $result ('children before close: {0}' -f `
            (($descendants | ForEach-Object { $_.Name }) -join ',')))

        if ($pre.Bitmap) { $pre.Bitmap.Dispose() }
        if ($summon.Bitmap) { $summon.Bitmap.Dispose() }
        if ($answer.Bitmap) { $answer.Bitmap.Dispose() }
    } finally {
        # Cleanup set = the positively observed fake Pi plus the current subtree.
        # Stop-RunDescendants re-proves PID + creation time + image per record and
        # never kills by name or an unverified PID.
        $descendants = Merge-ProcessRecords $fakeChildRecord @(Get-BoardChildProcesses -RootProcessId $pid1)
        $closed = Close-BoardProcess -Run $Run -ProcessId $pid1 -Hwnd $hwnd1
        $closedOk = ($closed.Graceful -or $closed.Killed)
        if (-not $closedOk -and $pid1 -gt 0) { $closedOk = -not (Test-DeskTestAlive -ProcessId $pid1) }
        [void](Add-DeskCheck $result 'offline instance closes within 12s' $closedOk)
        Start-Sleep -Milliseconds 700
        $cleanup = Stop-RunDescendants -Descendants $descendants
        Add-CleanupChecks -Result $result -Cleanup $cleanup -Prefix 'offline'
        # Identity-based leftover check: nothing of this run's copied image may
        # survive, whether it closed gracefully or was force-stopped above. This
        # also proves the run left no duplicate instance behind.
        $leakedImages = @(Find-ProcessByImagePath -ImagePath $Run.Exe)
        [void](Add-DeskCheck $result 'offline leaves no board image of this run' `
            ($leakedImages.Count -eq 0) `
            (($leakedImages | ForEach-Object { '{0}:{1}' -f $_.ProcessId, $_.Name }) -join ','))
        if ('DeskAssistantLaunch' -as [type]) { [DeskAssistantLaunch]::ForgetTracked($pid1) }
        $runChanges = @(Compare-DeskRunKey -Before $regBefore -After (Get-DeskRunKeySnapshot))
        [void](Add-DeskCheck $result 'HKCU Run is unchanged after the run' ($runChanges.Count -eq 0) `
            ($runChanges -join '; '))
        $prodAfter = Get-ProductionConfigSnapshot
        Add-ProductionConfigChecks -Result $result -Before $prodBefore -After $prodAfter -Prefix 'offline'
    }

    # Abort run: partial text then Esc must not append an episode.
    $abortPid = 0
    $abortHwnd = [IntPtr]::Zero
    $abortChildRecord = $null
    try {
        Write-AssistantConfig -ConfigDir $Run.AbortConfigDir -Fake $true -FakeExe $Run.FakeExe -Mode 'abort'
        $abortPid = Start-BoardProcess -Run $Run -Environment (New-OfflineEnvironment `
            -Run $Run -ConfigDir $Run.AbortConfigDir -Suffix $Run.AbortSuffix -FakeLog $Run.AbortFakeLog -Mode 'abort')
        $abortDiagnostic = Join-Path $Run.EvidenceDir 'abort-window-diagnostic.json'
        $abortWindow = Wait-BoardHwnd -Run $Run -ProcessId $abortPid `
            -ClassName ('wg.desk-' + $Run.AbortSuffix) -DiagnosticPath $abortDiagnostic
        if (-not $abortWindow) {
            [void](Add-DeskNote $result ('abort window diagnostic: ' + (Read-Utf8File -Path $abortDiagnostic)))
            [void](Add-DeskCheck $result 'abort instance starts' $false 'no window')
        } else {
            $abortHwnd = $abortWindow.Hwnd
            [void](Add-DeskCheck $result 'abort instance starts' ($abortWindow.Class -eq ('wg.desk-' + $Run.AbortSuffix)))
            [void][DeskTestNative]::PostMessage($abortHwnd, $script:HotkeyMessage,
                [IntPtr]$script:HotkeyAssistantId, [IntPtr]::Zero)
            Start-Sleep -Milliseconds 500
            $abortQuestion = '请流式回答：M1 取消验收'
            Send-AssistantQuestion -Hwnd $abortHwnd -Text $abortQuestion
            [void](Wait-FilePredicate -Path $Run.AbortFakeLog -TimeoutSeconds 20 -Predicate {
                param($text) $text.ToLowerInvariant().Contains('prompt')
            })
            Start-Sleep -Milliseconds 800
            Send-DeskEscape -Hwnd $abortHwnd
            $abortLog = Wait-FilePredicate -Path $Run.AbortFakeLog -TimeoutSeconds 10 -Predicate {
                param($text) $text.ToLowerInvariant().Contains('abort')
            }
            Start-Sleep -Milliseconds 2500
            $abortEpisodes = Join-Path $Run.AbortConfigDir 'assistant-episodes.jsonl'
            $after = @(Read-JsonLines -Path $abortEpisodes)
            [void](Add-DeskCheck $result 'Esc on a partial answer appends no episode' ($after.Count -eq 0) `
                ('episodes=' + $after.Count))
            [void](Add-DeskCheck $result 'fake RPC records the abort request' `
                ($abortLog.ToLowerInvariant().Contains('abort')))
            # Positive child control: Esc aborted a *live* fake Pi, so the child
            # has to be observed (PID > 0, our copied image) before the board is
            # closed. The recorded identity feeds the cleanup below.
            $abortChild = Wait-BoardChildImage -RootProcessId $abortPid -ImagePath $Run.FakeExe -TimeoutSeconds 15
            if ($abortChild.Found) { $abortChildRecord = $abortChild.Child }
            [void](Add-DeskCheck $result 'abort run has a live fake Pi child' `
                ($null -ne $abortChildRecord -and $abortChildRecord.ProcessId -gt 0) `
                ('observed: ' + (Format-ProcessRecord -Record $abortChildRecord)))
            [void](Add-DeskNote $result ('abort fake Pi subtree before close: {0}' -f `
                (($abortChild.Subtree | ForEach-Object { '{0}:{1}' -f $_.ProcessId, $_.Name }) -join ',')))
        }
    } finally {
        $abortDescendants = Merge-ProcessRecords $abortChildRecord @(Get-BoardChildProcesses -RootProcessId $abortPid)
        $abortClosed = Close-BoardProcess -Run $Run -ProcessId $abortPid -Hwnd $abortHwnd
        if ($abortPid -gt 0) {
            $abortClosedOk = ($abortClosed.Graceful -or $abortClosed.Killed)
            if (-not $abortClosedOk) { $abortClosedOk = -not (Test-DeskTestAlive -ProcessId $abortPid) }
            [void](Add-DeskCheck $result 'abort instance closes' $abortClosedOk)
        } else {
            [void](Add-DeskNote $result 'abort instance never started; nothing was launched to clean up')
        }
        Start-Sleep -Milliseconds 700
        $abortCleanup = Stop-RunDescendants -Descendants $abortDescendants
        Add-CleanupChecks -Result $result -Cleanup $abortCleanup -Prefix 'abort'
    }

    # enabled=0: assistant/memory are not registered at all. In assistant-only
    # mode that leaves no module, so the host must fail closed with no window
    # and no fake prompt.
    $disabledPid = 0
    $disabledHwnd = [IntPtr]::Zero
    $disabledDescendants = @()
    try {
        Write-AssistantConfig -ConfigDir $Run.DisabledConfigDir -Fake $true `
            -FakeExe $Run.FakeExe -Mode 'qa' -Enabled 0
        $disabledPid = Start-BoardProcess -Run $Run -Environment (New-OfflineEnvironment `
            -Run $Run -ConfigDir $Run.DisabledConfigDir -Suffix $Run.DisabledSuffix `
            -FakeLog $Run.DisabledFakeLog -Mode 'qa')
        # enabled=0 owns no window at all. Every visible window this PID creates
        # is sampled (not just the board class), so an error modal cannot be
        # filtered away and then pass as "no board window".
        $disabledWindows = @(Watch-DeskProcessWindows -ProcessId $disabledPid -TimeoutSeconds 6)
        $disabledWindowText = @($disabledWindows | ForEach-Object { '{0}:{1}' -f $_.Class, $_.Title })
        # Compare against this instance's own class, not the main board's: the
        # old main-class filter could never match and passed even when the
        # enabled=0 board really did open wg.desk-<DisabledSuffix>.
        $disabledBoardWindows = @($disabledWindows | Where-Object {
            $_.Class -eq ('wg.desk-' + $Run.DisabledSuffix) })
        [void](Add-DeskCheck $result 'enabled=0 registers no assistant/memory card' `
            ($disabledBoardWindows.Count -eq 0) ($disabledWindowText -join '; '))
        [void](Add-DeskCheck $result 'enabled=0 shows no visible window of its own' `
            ($disabledWindows.Count -eq 0) ($disabledWindowText -join '; '))
        # A board window here is already a failure; keep its handle so the close
        # below is a graceful WM_CLOSE instead of a blind wait.
        if ($disabledBoardWindows.Count -gt 0) { $disabledHwnd = $disabledBoardWindows[0].Hwnd }
        $disabledExited = Wait-DeskTestExit -ProcessId $disabledPid -TimeoutMs 5000
        [void](Add-DeskCheck $result 'enabled=0 assistant-only instance exits cleanly' $disabledExited)
        # Negative control: no child subtree, and no process anywhere with this
        # run's copied fake-Pi image. Read-only; nothing is killed here.
        $disabledDescendants = @(Get-BoardChildProcesses -RootProcessId $disabledPid)
        $disabledImageHits = @(Find-ProcessByImagePath -ImagePath $Run.FakeExe)
        [void](Add-DeskCheck $result 'enabled=0 owns no child subtree' `
            ($disabledDescendants.Count -eq 0) `
            (($disabledDescendants | ForEach-Object { '{0}:{1}' -f $_.ProcessId, $_.Name }) -join ','))
        [void](Add-DeskCheck $result 'enabled=0 starts no fake Pi image' `
            ($disabledImageHits.Count -eq 0) `
            (($disabledImageHits | ForEach-Object { '{0}:{1}' -f $_.ProcessId, $_.Name }) -join ','))
        $disabledIni = Read-DeskIniFile -Path (Join-Path $Run.DisabledConfigDir 'desk.ini')
        $disabledHasAssistant = $false
        if ($disabledIni) {
            foreach ($key in $disabledIni.Keys) {
                if ($key -like 'card.assistant.*' -or $key -like 'card.memory.*') { $disabledHasAssistant = $true }
            }
        }
        [void](Add-DeskCheck $result 'enabled=0 writes no assistant card state' (-not $disabledHasAssistant))
        $disabledLog = Read-Utf8File -Path $Run.DisabledFakeLog
        [void](Add-DeskCheck $result 'enabled=0 never starts the fake Pi' (-not $disabledLog.ToLowerInvariant().Contains('prompt')))
    } finally {
        # enabled=0 normally exits by itself; the close is a no-op in that case.
        # The recorded (empty) subtree still goes through the same cleanup and
        # the negative controls above are what prove nothing was spawned.
        $disabledClosed = Close-BoardProcess -Run $Run -ProcessId $disabledPid -Hwnd $disabledHwnd
        if ($disabledPid -gt 0 -and -not (Test-DeskTestAlive -ProcessId $disabledPid) `
                -and -not $disabledClosed.PathVerified) {
            [void](Add-DeskNote $result 'enabled=0 instance had already exited; close was a no-op')
        }
        $disabledCleanup = Stop-RunDescendants -Descendants $disabledDescendants
        Add-CleanupChecks -Result $result -Cleanup $disabledCleanup -Prefix 'enabled=0'
    }
    return $result
}

function Set-AssistantBodyFocus {
    # Re-focus the assistant after clicking other cards. Board::BlurCard runs for
    # every click a card does not claim for the keyboard, and WM_CHAR is only
    # forwarded to the focused card, so a typed question would otherwise go
    # nowhere. AssistantCard::WantsKeyboard is true for every body point; the
    # title bar does not claim the keyboard, so the click must land in the body.
    param([object]$Run, [IntPtr]$Hwnd)
    $ini = Read-DeskIniFile -Path (Join-Path $Run.ConfigDir 'desk.ini')
    if ($null -eq $ini -or -not $ini.Contains('card.assistant.x') -or
        -not $ini.Contains('card.assistant.y')) {
        return $false
    }
    [void](Send-DeskClick -Hwnd $Hwnd -X ([int]$ini['card.assistant.x'] + 40) `
        -Y ([int]$ini['card.assistant.y'] + 60))
    Start-Sleep -Milliseconds 400
    return $true
}

function Get-RealNodeRecords {
    param([int]$RootProcessId)
    return @(Get-BoardChildProcesses -RootProcessId $RootProcessId | Where-Object {
        ([string]$_.Name).ToLowerInvariant() -match '^node(\.exe)?$' })
}

function Get-SessionStatePath {
    # The path Engine::SaveSessionState records under session_path in
    # <ConfigDir>\assistant-session.ini. That is the file the next Ask resumes,
    # so it is read from the engine's own state file: no key is guessed and no
    # session content is inspected.
    param([string]$ConfigDir)
    $state = Read-DeskIniFile -Path (Join-Path $ConfigDir 'assistant-session.ini')
    if ($null -eq $state -or -not $state.Contains('session_path')) { return '' }
    return [string]$state['session_path']
}

function Test-PrivateSessionPath {
    # Trust only an absolute session file that really lives inside this run's
    # isolated config dir and exists on disk right now.
    param([string]$ConfigDir, [string]$SessionPath)
    if (-not $SessionPath -or -not [System.IO.Path]::IsPathRooted($SessionPath)) { return $false }
    $configFull = [System.IO.Path]::GetFullPath($ConfigDir)
    $sessionFull = [System.IO.Path]::GetFullPath($SessionPath)
    if (-not $sessionFull.StartsWith($configFull + [System.IO.Path]::DirectorySeparatorChar,
                                      [System.StringComparison]::OrdinalIgnoreCase)) {
        return $false
    }
    return (Test-Path -LiteralPath $sessionFull -PathType Leaf)
}

function Get-PrivateSessionFiles {
    # Full names of the JSONL session files under this run's pi-sessions dir.
    # Used only to prove a wake did not open an additional session file.
    param([string]$ConfigDir)
    $dir = Join-Path $ConfigDir 'pi-sessions'
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) { return @() }
    return @(Get-ChildItem -LiteralPath $dir -File -Recurse -Filter '*.jsonl' -ErrorAction SilentlyContinue |
        ForEach-Object { [System.IO.Path]::GetFullPath($_.FullName) } | Sort-Object)
}

function Get-SendLogCount {
    # assistant-send.log gets one UTF-8 line per remote Ask that the engine
    # dispatched (ContextBuilder::AppendSendLog). Counting lines is therefore a
    # payload-free record of how many questions reached the Pi lane.
    param([string]$Path)
    $text = Read-Utf8File -Path $Path
    if (-not $text) { return 0 }
    return @($text -split "`n" | Where-Object { $_.Trim() }).Count
}

function Test-CitationWhitelist {
    param([string]$Sources)
    if (-not $Sources) { return $false }
    foreach ($source in ($Sources -split ',')) {
        $trimmed = $source.Trim()
        if (-not ($trimmed.StartsWith('dsh-') -or $trimmed.StartsWith('mem-'))) { return $false }
    }
    return $true
}

function Invoke-RealSuite {
    param([object]$Run, [bool]$Minimal)
    $result = New-DeskResult
    $result.Name = if ($Minimal) { 'assistant-real-probe' } else { 'assistant-real-gui' }
    Add-RuntimeDependencyChecks -Result $result -Prefix 'real'
    $regBefore = Get-DeskRunKeySnapshot
    $prodBefore = Get-ProductionConfigSnapshot
    $pid1 = 0
    $hwnd1 = [IntPtr]::Zero
    $pre = $null
    $nodeRecords = @()
    try {
        Write-AssistantConfig -ConfigDir $Run.ConfigDir -Fake $false
        # The harness itself never passes a credential value: key_ref is only a
        # name in assistant.ini, and the app resolves it into memory and its own
        # child environment. USERPROFILE/LOCALAPPDATA/TEMP stay real on purpose
        # so the trusted launcher (node / pi.cmd), the official provider
        # extension and the key reference resolve; Engine already keeps
        # PI_CODING_AGENT_DIR (pi-agent), pi-work and the session file inside the
        # isolated DESK_TEST_CONFIG_DIR.
        $realEnv = New-RealEnvironment -Run $Run -Suffix $Run.Suffix `
            -RegisterAssistantHotkey ([bool]$PhysicalHotkeyProbe)
        $secretLike = @($realEnv.GetEnumerator() | Where-Object {
            ([string]$_.Value) -match '^(sk-|Bearer\s)' -or ([string]$_.Value).Length -gt 260 })
        [void](Add-DeskCheck $result 'real harness env carries no credential value' `
            ($secretLike.Count -eq 0) (($secretLike | ForEach-Object { $_.Key }) -join ','))
        $pid1 = Start-BoardProcess -Run $Run -Environment $realEnv
        $windowDiagnostic = Join-Path $Run.EvidenceDir 'window-diagnostic.json'
        $window = Wait-BoardHwnd -Run $Run -ProcessId $pid1 -DiagnosticPath $windowDiagnostic
        if (-not $window) {
            [void](Add-DeskNote $result ('real window diagnostic: ' + (Read-Utf8File -Path $windowDiagnostic)))
            [void](Add-DeskCheck $result 'real board window starts' $false 'no window')
            return $result
        }
        $hwnd1 = $window.Hwnd
        [void](Add-DeskCheck $result 'real board window starts' ($window.Class -eq $Run.ClassName) $window.Class)
        [void](Add-DeskCheck $result 'real stage uses an isolated desk config' `
            (Test-Path -LiteralPath (Join-Path $Run.ConfigDir 'assistant.ini')))
        [void](Add-DeskNote $result 'real stage keeps the real USERPROFILE/LOCALAPPDATA/TEMP so node/pi.cmd, the official provider and key_ref resolve; DSH and desk config are fixtures')
        [void](Add-DeskNote $result 'real Pi boundary: Pi agent dir, pi-work and the session file live under DESK_TEST_CONFIG_DIR (Engine), but the trusted Pi/provider can still write its install temp locks and caches outside the run dir - this is not an OS-level write sandbox')
        if ($Minimal) {
            [void](Add-DeskNote $result 'probe is a minimal network/citation smoke test, not full GUI acceptance')
        }

        $shot = Save-PrintWindowEvidence -Hwnd $hwnd1 -Width $window.W -Height $window.H `
            -Path (Join-Path $Run.EvidenceDir 'printwindow-real.png')
        [void](Add-DeskNote $result ('PrintWindow real: present={0} blank={1} {2}' -f `
            $shot.Present, $shot.Blank, $shot.Stats))
        $pre = Get-OwnCanvasFrame -Hwnd $hwnd1 -Run $Run -ConfigDir $Run.ConfigDir -Slot 0 -Label 'canvas-real-pre'
        [void](Add-DeskCheck $result 'real own-canvas frame is non-empty' `
            ($pre.Export -and $null -ne $pre.Bitmap -and $pre.Ink -gt 50) $pre.Stats)

        if ($PhysicalHotkeyProbe) {
            Send-PhysicalCtrlAltF10
            Start-Sleep -Milliseconds 700
            [void](Add-DeskNote $result 'physical hotkey probe used Ctrl+Alt+F10 only; default Ctrl+Alt+A was not pressed')
        } else {
            [void][DeskTestNative]::PostMessage($hwnd1, $script:HotkeyMessage,
                [IntPtr]$script:HotkeyAssistantId, [IntPtr]::Zero)
            Start-Sleep -Milliseconds 500
        }

        $question = if ($Minimal) { '请用一句中文回答：M1 真实通道探针' } else { '请用一句中文回答：M1 真实通道验收' }
        $episodePath = Join-Path $Run.ConfigDir 'assistant-episodes.jsonl'
        Send-AssistantQuestion -Hwnd $hwnd1 -Text $question
        [void](Wait-FilePredicate -Path $episodePath -TimeoutSeconds $TimeoutSeconds -Predicate {
            param($text) $text.Contains('"question"')
        })
        $episodes = @(Read-JsonLines -Path $episodePath)
        $episode = $null
        if ($episodes.Count -gt 0) { $episode = $episodes[$episodes.Count - 1] }
        [void](Add-DeskCheck $result 'real channel returns an answer' ($null -ne $episode))
        if ($episode) {
            [void](Add-DeskCheck $result 'real answer contains Chinese text' `
                ([string]$episode.answer -match '[\u4e00-\u9fff]'))
            $sources = [string]$episode.sources
            $sourceOk = $false
            if ($sources) {
                $sourceOk = $true
                foreach ($source in ($sources -split ',')) {
                    $trimmed = $source.Trim()
                    if (-not ($trimmed.StartsWith('dsh-') -or $trimmed.StartsWith('mem-'))) { $sourceOk = $false }
                }
            }
            [void](Add-DeskCheck $result 'real citation is on the whitelist' $sourceOk $sources)
        }
        $answerFrame = Get-OwnCanvasFrame -Hwnd $hwnd1 -Run $Run -ConfigDir $Run.ConfigDir -Slot 1 -Label 'canvas-real-answer'
        $diff = 0.0
        if ($pre.Bitmap -and $answerFrame.Bitmap) {
            $diff = [DeskTestImage]::DiffFraction($pre.Bitmap, $answerFrame.Bitmap, 0, 0,
                [math]::Min($pre.Bitmap.Width, $answerFrame.Bitmap.Width),
                [math]::Min($pre.Bitmap.Height, $answerFrame.Bitmap.Height), 20)
        }
        [void](Add-DeskCheck $result 'real answer changes the own-canvas frame' `
            ($answerFrame.Export -and $diff -gt 0.001) ('diff={0:F4}' -f $diff))
        if ($answerFrame.Bitmap) { $answerFrame.Bitmap.Dispose() }

        $costPath = Join-Path $Run.ConfigDir 'assistant-cost.ini'
        $cost = Read-DeskIniFile -Path $costPath
        $tokens = 0
        if ($cost) {
            foreach ($key in $cost.Keys) {
                if ($key -like 'day.*.tokens') { $tokens += [int]$cost[$key] }
            }
        }
        [void](Add-DeskCheck $result 'real cost file records tokens' ($tokens -gt 0) "tokens=$tokens")

        # Pi/Node subtree evidence, captured while the answered session is still
        # live: after this point the stage sends Esc, runs local commands and
        # closes the board, so an empty set later would only prove the child is
        # gone, not that it ran. Three separate facts are kept apart: board idle
        # CPU/WS (below), Node running WS/CPU (here) and exit evidence (finally).
        # Thresholds are the documented M1 plan ones (docs/assistant-m1-plan.md
        # section 7: pi runtime memory < 150MB, idle ~0 CPU); no new limit is
        # invented and no existing one is relaxed.
        $piSubtree = @(Get-BoardChildProcesses -RootProcessId $pid1)
        $nodeRecords = @($piSubtree | Where-Object {
            ([string]$_.Name).ToLowerInvariant() -match '^node(\.exe)?$' })
        $nodePidOk = @($nodeRecords | Where-Object { [int]$_.ProcessId -gt 0 }).Count -gt 0
        [void](Add-DeskCheck $result 'real Pi runs as a Node child of this board' `
            ($nodePidOk) `
            ('subtree=' + (($piSubtree | ForEach-Object { '{0}:{1}' -f $_.ProcessId, $_.Name }) -join ',')))
        foreach ($nodeRecord in $nodeRecords) {
            [void](Add-DeskNote $result ('real Pi node identity: ' + (Format-ProcessRecord -Record $nodeRecord)))
        }
        $nodeSamples = @(Measure-ProcessSet -Records $nodeRecords -Seconds 3)
        $nodeFailed = @($nodeSamples | Where-Object { $_.SamplingFailed })
        $nodeLive = @($nodeSamples | Where-Object { -not $_.SamplingFailed })
        [void](Add-DeskCheck $result 'real Pi node resource sampling succeeds' `
            ($nodeSamples.Count -gt 0 -and $nodeFailed.Count -eq 0) `
            (($nodeFailed | ForEach-Object { '{0}:{1} ({2})' -f $_.ProcessId, $_.Name, $_.Reason }) -join '; '))
        $nodeWsMax = 0.0
        foreach ($sample in $nodeLive) {
            if ([double]$sample.WSMB -gt $nodeWsMax) { $nodeWsMax = [double]$sample.WSMB }
            if ([double]$sample.WSMBEnd -gt $nodeWsMax) { $nodeWsMax = [double]$sample.WSMBEnd }
        }
        [void](Add-DeskCheck $result 'real Pi node memory stays below 150MB (M1 plan 7)' `
            ($nodeLive.Count -gt 0 -and $nodeWsMax -lt 150.0) ('peak node ws={0}MB' -f $nodeWsMax))
        [void](Add-DeskNote $result ('real node samples (window 3s each, ws in MB start->end, cpu in ms over the window): {0}' -f `
            (($nodeSamples | ForEach-Object {
                '{0}:{1} ws={2}->{3}MB cpu={4}ms ({5}%)' -f $_.ProcessId, $_.Name, $_.WSMB, $_.WSMBEnd, $_.CpuMs, $_.CpuPct
            }) -join '; ')))

        if (-not $Minimal) {
            # Extended real acceptance (never part of the probe): abort recovery,
            # natural idle stop and wake/session continuation. Network budget is
            # first question + 1 cancel + 1 recovery + 1 wake; nothing retries.
            try {
                $sendLogPath = Join-Path $Run.ConfigDir 'assistant-send.log'

                # 1) Cancel a genuinely in-flight answer. The request asks for a
                # long multi-point answer so it is still streaming when Esc is
                # sent. An answer that had already completed would append an
                # episode and the count check below fails closed. Positive
                # controls that the remote really received it: the send-log line
                # and the user turn Pi persists in its private session JSONL.
                $cancelSessionPath = Get-SessionStatePath -ConfigDir $Run.ConfigDir
                $cancelSessionTrusted = Test-PrivateSessionPath -ConfigDir $Run.ConfigDir `
                    -SessionPath $cancelSessionPath
                $cancelSessionBytesBefore = 0
                if ($cancelSessionTrusted) {
                    $cancelSessionBytesBefore = (Get-Item -LiteralPath $cancelSessionPath).Length
                }
                $countBeforeCancel = @(Read-JsonLines -Path $episodePath).Count
                $sendBeforeCancel = Get-SendLogCount -Path $sendLogPath
                $cancelQuestion = 'M1_CANCEL_OK：请用中文分二十点详细展开这段纯合成说明，每点至少两句话，不要提前结束。'
                Send-AssistantQuestion -Hwnd $hwnd1 -Text $cancelQuestion
                Start-Sleep -Milliseconds 900
                Send-DeskEscape -Hwnd $hwnd1
                Start-Sleep -Milliseconds 3000
                $countAfterCancel = @(Read-JsonLines -Path $episodePath).Count
                $sendAfterCancel = Get-SendLogCount -Path $sendLogPath
                $cancelSessionText = ''
                if ($cancelSessionTrusted) {
                    $cancelSessionText = Wait-FilePredicate -Path $cancelSessionPath -TimeoutSeconds 10 -Predicate {
                        param($text) $text.Contains('M1_CANCEL_OK')
                    }
                }
                $cancelPersisted = $cancelSessionTrusted -and $cancelSessionText.Contains('M1_CANCEL_OK')
                $cancelSessionBytesAfter = 0
                if ($cancelSessionTrusted) {
                    $cancelSessionBytesAfter = (Get-Item -LiteralPath $cancelSessionPath).Length
                }
                [void](Add-DeskCheck $result 'real cancel question reached the isolated Pi session' `
                    ($sendAfterCancel -eq ($sendBeforeCancel + 1) -and $cancelPersisted) `
                    ('sendlog={0}->{1} session={2}B->{3}B userTurn={4}' -f $sendBeforeCancel, $sendAfterCancel, $cancelSessionBytesBefore, $cancelSessionBytesAfter, $cancelPersisted))
                [void](Add-DeskCheck $result 'real Esc cancel appends no episode' `
                    ($countAfterCancel -eq $countBeforeCancel) `
                    ('before={0} after={1}' -f $countBeforeCancel, $countAfterCancel))
                [void](Add-DeskNote $result ('real cancel window: 900ms in flight, 3000ms settle, episodes={0}' -f $countAfterCancel))

                # 2) Recovery ask: the abort must have settled before the engine
                # accepts (and answers) a new question. A BUSY rejection appends
                # no episode and fails closed; nothing is retried.
                $recoveryFocusOk = Set-AssistantBodyFocus -Run $Run -Hwnd $hwnd1
                $recoveryQuestion = 'M1_RECOVER_OK：请用一句中文并依据 DSH 会话来源回答：M1 fixture project 的结论是什么？'
                $sendBeforeRecovery = Get-SendLogCount -Path $sendLogPath
                Send-AssistantQuestion -Hwnd $hwnd1 -Text $recoveryQuestion
                $recoveryDeadline = (Get-Date).AddSeconds($TimeoutSeconds)
                $recoveryEpisodes = @()
                while ((Get-Date) -lt $recoveryDeadline) {
                    $recoveryEpisodes = @(Read-JsonLines -Path $episodePath)
                    if ($recoveryEpisodes.Count -gt $countAfterCancel) { break }
                    Start-Sleep -Milliseconds 400
                }
                $recovery = $null
                if ($recoveryEpisodes.Count -gt $countAfterCancel) {
                    $recovery = $recoveryEpisodes[$recoveryEpisodes.Count - 1]
                }
                $sendAfterRecovery = Get-SendLogCount -Path $sendLogPath
                $recoveryExact = ($null -ne $recovery -and [string]$recovery.question -eq $recoveryQuestion)
                [void](Add-DeskCheck $result 'real recovery question is dispatched after abort' `
                    ($sendAfterRecovery -eq ($sendBeforeRecovery + 1)) `
                    ('focus={0} sendlog={1}->{2}' -f $recoveryFocusOk, $sendBeforeRecovery, $sendAfterRecovery))
                [void](Add-DeskCheck $result 'real recovery appends exactly one new episode' `
                    ($null -ne $recovery -and $recoveryEpisodes.Count -eq ($countAfterCancel + 1) -and $recoveryExact) `
                    ('before={0} after={1} exactQuestion={2}' -f $countAfterCancel, $recoveryEpisodes.Count, $recoveryExact))
                $recoveryAnswer = if ($recovery) { [string]$recovery.answer } else { '' }
                $recoverySources = if ($recovery) { [string]$recovery.sources } else { '' }
                [void](Add-DeskCheck $result 'real recovery answer contains Chinese text' `
                    ($recoveryAnswer -match '[\u4e00-\u9fff]') ('chars={0}' -f $recoveryAnswer.Length))
                [void](Add-DeskCheck $result 'real recovery citation is on the whitelist' `
                    (Test-CitationWhitelist -Sources $recoverySources) $recoverySources)

                # 3) Natural idle stop. The Engine state file is authoritative for
                # the resumed session path; its 1 minute idle setting is real here
                # (production keeps the 10 minute default, covered by config+seam,
                # not by this wait). Nothing is killed to fake the idle.
                $sessionPathBefore = Get-SessionStatePath -ConfigDir $Run.ConfigDir
                $sessionTrustedBefore = Test-PrivateSessionPath -ConfigDir $Run.ConfigDir `
                    -SessionPath $sessionPathBefore
                $sessionFilesBefore = @(Get-PrivateSessionFiles -ConfigDir $Run.ConfigDir)
                $sessionBytesBefore = 0
                if ($sessionTrustedBefore) {
                    $sessionBytesBefore = (Get-Item -LiteralPath $sessionPathBefore).Length
                }
                [void](Add-DeskCheck $result 'real Engine state records the private Pi session file' `
                    $sessionTrustedBefore $sessionPathBefore)
                [void](Add-DeskNote $result ('real idle session files: {0}' -f ($sessionFilesBefore -join '; ')))

                $idleNodes = @(Get-RealNodeRecords -RootProcessId $pid1)
                [void](Add-DeskCheck $result 'real live Pi node identity is observed before idle' `
                    ($idleNodes.Count -gt 0) `
                    (($idleNodes | ForEach-Object { Format-ProcessRecord -Record $_ }) -join '; '))
                foreach ($record in $idleNodes) {
                    [void](Add-DeskNote $result ('real idle node: ' + (Format-ProcessRecord -Record $record)))
                }
                $idleGone = $false
                $idleElapsed = 0.0
                if ($idleNodes.Count -gt 0) {
                    $idleWatch = [System.Diagnostics.Stopwatch]::StartNew()
                    $idleDeadline = (Get-Date).AddSeconds(75)
                    while ((Get-Date) -lt $idleDeadline) {
                        if (-not (Test-DeskTestAlive -ProcessId $pid1)) { break }
                        $idleAlive = 0
                        foreach ($record in $idleNodes) {
                            $idleState = Test-RecordedProcess -Recorded $record
                            if ($idleState.State -eq 'same' -or $idleState.State -eq 'unverified') { $idleAlive++ }
                        }
                        if ($idleAlive -eq 0) { $idleGone = $true; break }
                        Start-Sleep -Milliseconds 1000
                    }
                    $idleWatch.Stop()
                    $idleElapsed = $idleWatch.Elapsed.TotalSeconds
                }
                $boardAliveAfterIdle = Test-DeskTestAlive -ProcessId $pid1
                [void](Add-DeskCheck $result 'real idle stops the live Pi node naturally' `
                    ($idleNodes.Count -gt 0 -and $idleGone -and $boardAliveAfterIdle) `
                    ('elapsed={0:F1}s gone={1} boardAlive={2}' -f $idleElapsed, $idleGone, $boardAliveAfterIdle))
                [void](Add-DeskCheck $result 'real idle stop waits the configured one minute' `
                    ($idleGone -and $idleElapsed -ge 50.0 -and $idleElapsed -le 80.0) `
                    ('elapsed={0:F1}s' -f $idleElapsed))
                [void](Add-DeskCheck $result 'real board stays alive after Pi idles out' $boardAliveAfterIdle)

                # 4) Wake after the natural idle: a new episode plus a new Node
                # child of the same board, while the same private session file is
                # resumed (strict path comparison, no new session file).
                $wakeFocusOk = Set-AssistantBodyFocus -Run $Run -Hwnd $hwnd1
                $wakeQuestion = 'M1_IDLE_WAKE_OK：请用一句中文回答：空闲唤醒后的同会话续用已生效。'
                $countBeforeWake = @(Read-JsonLines -Path $episodePath).Count
                $sendBeforeWake = Get-SendLogCount -Path $sendLogPath
                Send-AssistantQuestion -Hwnd $hwnd1 -Text $wakeQuestion
                $wakeDeadline = (Get-Date).AddSeconds($TimeoutSeconds)
                $wakeEpisodes = @()
                while ((Get-Date) -lt $wakeDeadline) {
                    $wakeEpisodes = @(Read-JsonLines -Path $episodePath)
                    if ($wakeEpisodes.Count -gt $countBeforeWake) { break }
                    Start-Sleep -Milliseconds 400
                }
                $wakeEpisode = $null
                if ($wakeEpisodes.Count -gt $countBeforeWake) {
                    $wakeEpisode = $wakeEpisodes[$wakeEpisodes.Count - 1]
                }
                $sendAfterWake = Get-SendLogCount -Path $sendLogPath
                [void](Add-DeskCheck $result 'real wake question is dispatched after idle' `
                    ($sendAfterWake -eq ($sendBeforeWake + 1)) `
                    ('focus={0} sendlog={1}->{2}' -f $wakeFocusOk, $sendBeforeWake, $sendAfterWake))
                [void](Add-DeskCheck $result 'real wake appends a new episode with the wake marker' `
                    ($null -ne $wakeEpisode -and $wakeEpisodes.Count -eq ($countBeforeWake + 1) -and
                     ([string]$wakeEpisode.question).Contains('M1_IDLE_WAKE_OK')) `
                    ('before={0} after={1}' -f $countBeforeWake, $wakeEpisodes.Count))
                $wakeAnswer = if ($wakeEpisode) { [string]$wakeEpisode.answer } else { '' }
                [void](Add-DeskCheck $result 'real wake answer contains Chinese text' `
                    ($wakeAnswer -match '[\u4e00-\u9fff]') ('chars={0}' -f $wakeAnswer.Length))

                $wakeNodeDeadline = (Get-Date).AddSeconds(15)
                $wakeNodes = @()
                while ((Get-Date) -lt $wakeNodeDeadline) {
                    $wakeNodes = @(Get-RealNodeRecords -RootProcessId $pid1)
                    if ($wakeNodes.Count -gt 0) { break }
                    Start-Sleep -Milliseconds 500
                }
                $oldIdentity = @{}
                foreach ($record in $idleNodes) {
                    $oldIdentity[('{0}|{1}' -f $record.ProcessId, $record.CreationTicks)] = $true
                }
                $newNodes = @($wakeNodes | Where-Object {
                    ([int]$_.ProcessId -gt 0) -and
                    (-not $oldIdentity.ContainsKey(('{0}|{1}' -f $_.ProcessId, $_.CreationTicks))) })
                [void](Add-DeskCheck $result 'real wake starts a new Pi node child of this board' `
                    ($null -ne $wakeEpisode -and $newNodes.Count -gt 0) `
                    (($wakeNodes | ForEach-Object { Format-ProcessRecord -Record $_ }) -join '; '))
                foreach ($record in $newNodes) {
                    [void](Add-DeskNote $result ('real wake node identity: ' + (Format-ProcessRecord -Record $record)))
                }
                # The wake child joins the same finally cleanup identity set.
                $nodeRecords = @(Merge-ProcessRecords $nodeRecords $wakeNodes)

                $sessionPathAfter = Get-SessionStatePath -ConfigDir $Run.ConfigDir
                $sessionTrustedAfter = Test-PrivateSessionPath -ConfigDir $Run.ConfigDir `
                    -SessionPath $sessionPathAfter
                $sessionSame = $false
                if ($sessionTrustedBefore -and $sessionTrustedAfter) {
                    $sessionSame = [System.StringComparer]::OrdinalIgnoreCase.Equals(
                        [System.IO.Path]::GetFullPath($sessionPathBefore),
                        [System.IO.Path]::GetFullPath($sessionPathAfter))
                }
                $sessionBytesAfter = 0
                if ($sessionTrustedAfter) {
                    $sessionBytesAfter = (Get-Item -LiteralPath $sessionPathAfter).Length
                }
                [void](Add-DeskCheck $result 'real wake reuses the same private Pi session file' `
                    ($null -ne $wakeEpisode -and $sessionSame -and $sessionBytesAfter -ge $sessionBytesBefore -and $sessionBytesAfter -gt 0) `
                    ('before={0} after={1} bytes={2}->{3}' -f $sessionPathBefore, $sessionPathAfter, $sessionBytesBefore, $sessionBytesAfter))
                $sessionFilesAfter = @(Get-PrivateSessionFiles -ConfigDir $Run.ConfigDir)
                $sessionSetSame = (($sessionFilesBefore -join '|') -eq ($sessionFilesAfter -join '|'))
                [void](Add-DeskCheck $result 'real wake opens no additional Pi session file' `
                    ($null -ne $wakeEpisode -and $sessionSetSame -and $sessionFilesBefore.Count -gt 0) `
                    ('files={0}->{1}' -f $sessionFilesBefore.Count, $sessionFilesAfter.Count))
                [void](Add-DeskCheck $result 'real network budget stayed within 4 dispatches (first+cancel+recovery+wake)' `
                    ($sendAfterWake -le 4) ('dispatches={0}' -f $sendAfterWake))
            } catch {
                # Fail closed with evidence: the finally below still closes the
                # board, cleans the recorded children and writes the result.
                [void](Add-DeskCheck $result 'real extended acceptance completed without an exception' `
                    $false $_.Exception.Message)
                [void](Add-DeskNote $result ('real extended section exception: ' + $_.Exception.Message))
            }
        }

        $cpu = Measure-DeskCpu -ProcessId $pid1 -Seconds 5
        $stats = Get-DeskProcessStats -ProcessId $pid1
        if ($cpu -and $stats) {
            [void](Add-DeskCheck $result 'real board idle CPU stays below 2%' ($cpu.CpuPct -lt 2.0) `
                ('cpu={0}% ws={1}MB' -f $cpu.CpuPct, $stats.WSMB))
            [void](Add-DeskNote $result ('real board (desk.exe) idle stats: ws={0}MB private={1}MB threads={2}' -f `
                $stats.WSMB, $stats.PrivateMB, $stats.Threads))
        }
        if ($pre.Bitmap) { $pre.Bitmap.Dispose(); $pre = $null }
    } finally {
        if ($pre -and $pre.Bitmap) { $pre.Bitmap.Dispose() }
        # Cleanup set = the sampled Node/Pi children plus the current subtree;
        # each record is re-proved (PID + creation time + image) before a kill.
        $descendants = Merge-ProcessRecords $nodeRecords @(Get-BoardChildProcesses -RootProcessId $pid1)
        $closed = Close-BoardProcess -Run $Run -ProcessId $pid1 -Hwnd $hwnd1
        $closedOk = ($closed.Graceful -or $closed.Killed)
        if (-not $closedOk -and $pid1 -gt 0) { $closedOk = -not (Test-DeskTestAlive -ProcessId $pid1) }
        [void](Add-DeskCheck $result 'real instance closes within 12s' $closedOk)
        Start-Sleep -Milliseconds 800
        $cleanup = Stop-RunDescendants -Descendants $descendants
        Add-CleanupChecks -Result $result -Cleanup $cleanup -Prefix 'real'
        $leakedImages = @(Find-ProcessByImagePath -ImagePath $Run.Exe)
        [void](Add-DeskCheck $result 'real leaves no board image of this run' `
            ($leakedImages.Count -eq 0) `
            (($leakedImages | ForEach-Object { '{0}:{1}' -f $_.ProcessId, $_.Name }) -join ','))
        if ('DeskAssistantLaunch' -as [type]) { [DeskAssistantLaunch]::ForgetTracked($pid1) }
        # Exit evidence for the sampled Node/Pi children: none may still be the
        # same process; an unreadable identity counts as still present.
        if ($nodeRecords.Count -gt 0) {
            $nodeStates = New-Object System.Collections.Generic.List[string]
            $nodeAlive = 0
            foreach ($nodeRecord in $nodeRecords) {
                $nodeState = Test-RecordedProcess -Recorded $nodeRecord
                if ($nodeState.State -eq 'same' -or $nodeState.State -eq 'unverified') { $nodeAlive++ }
                $nodeStates.Add(('{0}:{1}={2}' -f $nodeRecord.ProcessId, $nodeRecord.Name, $nodeState.State))
            }
            [void](Add-DeskCheck $result 'real Pi node exits with the board' ($nodeAlive -eq 0) `
                ($nodeStates.ToArray() -join '; '))
        }
        $runChanges = @(Compare-DeskRunKey -Before $regBefore -After (Get-DeskRunKeySnapshot))
        [void](Add-DeskCheck $result 'HKCU Run is unchanged after the real run' ($runChanges.Count -eq 0) `
            ($runChanges -join '; '))
        $prodAfter = Get-ProductionConfigSnapshot
        Add-ProductionConfigChecks -Result $result -Before $prodBefore -After $prodAfter -Prefix 'real'
    }
    return $result
}

function Test-Preflight {
    param([object]$Run)
    if (-not (Test-Path -LiteralPath $Run.Exe -PathType Leaf)) { throw "test exe missing: $($Run.Exe)" }
    # The three wide hooks must all be present before a copy is launched. A
    # stale exe that lacks any of them could open the real board, register the
    # real global hotkey or spawn the real Pi first and only then fail.
    foreach ($needle in @('DESK_TEST_CONFIG_DIR', 'DESK_TEST_ASSISTANT_ONLY',
                          'DESK_TEST_REGISTER_ASSISTANT_HOTKEY')) {
        if (-not (Test-DeskTestHook -Path $Run.Exe -Needle $needle)) {
            throw "build\bin\desk.exe has no $needle hook; refusing to launch a copy"
        }
    }
    if ($Stage -eq 'offline') {
        if (-not (Test-Path -LiteralPath $Run.FakeExe -PathType Leaf)) {
            throw "fake Pi missing: $($Run.FakeExe) (RPC lane must build fake-pi-rpc.exe)"
        }
        if (-not (Test-DeskTestHook -Path $Run.FakeExe -Needle 'FAKE_PI_MODE')) {
            throw 'fake-pi-rpc.exe has no FAKE_PI_MODE hook; the fake contract is not built yet'
        }
    }
}

try {
    if ($Stage -ne 'offline' -and -not $AuthorizedNetworkProbe) {
        Write-Host '[assistant-test] real/probe needs -AuthorizedNetworkProbe; nothing was started or sent.' -ForegroundColor Yellow
        exit 2
    }

    $run = New-RunLayout -Label $Stage -SuffixPrefix 'asq'
    $script:LastRun = $run
    $sourceExe = if ($ExePath) { [System.IO.Path]::GetFullPath($ExePath) } else { Join-Path $script:BuildRoot 'bin\desk.exe' }
    if (-not (Test-Path -LiteralPath $sourceExe -PathType Leaf)) { throw "desk.exe missing: $sourceExe" }
    Copy-Item -LiteralPath $sourceExe -Destination $run.Exe -Force
    # A QA copy must be self-contained: record every static import and copy any
    # same-build runtime DLL that exists beside the source exe (nothing is added
    # to PATH, nothing is installed).
    $script:RuntimeDependencies = @(Copy-ExeRuntimeDependencies -SourceExe $sourceExe `
        -DestinationDirectory $run.AppDir)
    if ($Stage -eq 'offline') {
        $sourceFake = Join-Path $script:BuildRoot 'fake-pi-rpc.exe'
        if (-not (Test-Path -LiteralPath $sourceFake -PathType Leaf)) { throw "fake-pi-rpc.exe missing: $sourceFake" }
        Copy-Item -LiteralPath $sourceFake -Destination $run.FakeExe -Force
        $script:RuntimeDependencies += @(Copy-ExeRuntimeDependencies -SourceExe $sourceFake `
            -DestinationDirectory $run.AppDir)
    }
    Write-JsonFile -Path (Join-Path $run.EvidenceDir 'dependency-diagnostic.json') `
        -Value @($script:RuntimeDependencies)
    Write-DshFixture -Run $run
    if ($Stage -eq 'offline') {
        Write-AssistantConfig -ConfigDir $run.ConfigDir -Fake $true -FakeExe $run.FakeExe -Mode 'qa'
    }
    Test-Preflight -Run $run

    $result = $null
    if ($Stage -eq 'offline') {
        $result = Invoke-OfflineSuite -Run $run
    } else {
        $result = Invoke-RealSuite -Run $run -Minimal ($Stage -eq 'probe')
    }

    $fileName = if ($Stage -eq 'probe') { 'probe.json' } elseif ($Stage -eq 'real') { 'gui-real.json' } else { 'gui-offline.json' }
    $path = Join-Path $run.EvidenceDir $fileName
    $passed = Save-DeskResult -Result $result -Path $path
    Write-Host ('[assistant-test] result: {0}' -f $path)
    if ($Stage -eq 'probe') {
        Write-Host '[assistant-test] probe result is separate and is not GUI acceptance.' -ForegroundColor Yellow
    }
    if ($passed) { exit 0 } else { exit 1 }
} finally {
    Write-Host '[assistant-test] done (only the PID started by this script was stopped).'
}
