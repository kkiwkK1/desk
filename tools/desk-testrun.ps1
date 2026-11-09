# desk-testrun.ps1 - isolated scenario harness for desk.exe.
#
# For every scenario this script:
#   build\testrun\<scenario>\desk.exe     signed copy of build\bin\desk.exe
#   build\testrun\<scenario>\desk.ini     seeded config (UTF-16LE + BOM)
#   launches the copy with DESK_TEST_INSTANCE=<suffix> (board id desk-<suffix>)
#   waits for the board window (class wg.desk-<suffix>)
#   collects window rect, work area, desk.ini, PNG screenshot, CPU/memory
#   closes it with WM_CLOSE (Stop-Process only for the PID this script started)
#   writes evidence + assertions to build\qa\runs\<RunId>\<scenario>\
#
# Safety rules encoded here:
#   * the binary must contain the DESK_TEST_INSTANCE hook, otherwise nothing is
#     launched (a hookless copy would touch the live app's HKCU Run value),
#   * HKCU Run is snapshotted before/after every run and any change fails it,
#   * only processes started by this script are messaged or stopped,
#   * the installed app folder is only ever read.
#
# Examples:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\desk-testrun.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\desk-testrun.ps1 -Scenario fresh,oob -RunId acceptance
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\desk-testrun.ps1 -DryRun

param(
    [string[]]$Scenario = @('fresh', 'legacy760', 'corrupt', 'corrupt-badvalues', 'oob', 'cannot-fit', 'cannot-fit-huge', 'drag-release', 'drag-small-board', 'reset-panel', 'drag-cancel', 'snapshot-clear', 'adapt-clamp', 'restore-strict', 'consecutive-adapt', 'dpi-roundtrip', 'display-shim'),
    [string]$Exe = '',
    [string]$RunId = '',
    [int]$TimeoutSeconds = 25,
    [int]$IdleSeconds = 4,
    [switch]$NoSign,
    [switch]$KeepProcess,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
# -File passes "a,b" as one string; accept both that and a real array.
$Scenario = @($Scenario | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'desk-testlib.ps1')

if (-not $Exe) { $Exe = Join-Path $root 'build\bin\desk.exe' }
$TestRoot = Join-Path $root 'build\testrun'
$EvidenceRoot = Join-Path $root 'build\qa\runs'
if (-not $RunId) { $RunId = Get-Date -Format 'yyyyMMdd-HHmmss' }
$RunDir = Join-Path $EvidenceRoot $RunId

$script:LaunchedPids = New-Object System.Collections.Generic.List[int]

# ---------------------------------------------------------------------------
# scenario definitions
# ---------------------------------------------------------------------------

function Get-LegacySeed {
    # Exact snapshot of the old 760x800 install (M:\workspace\_backup\...\desk.ini.snapshot).
    return [ordered]@{
        'board.grid'            = 1
        'board.h'               = 800
        'board.solid'           = 0
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
    }
}

function Get-ScenarioDefinition {
    param([string]$Name)
    switch ($Name) {
        'fresh' {
            return [pscustomobject]@{
                Name = 'fresh'; Suffix = 'fresh'; Kind = 'default'
                Seed = $null; RunTwice = $false
            }
        }
        'legacy760' {
            return [pscustomobject]@{
                Name = 'legacy760'; Suffix = 'legacy760'; Kind = 'migration'
                Seed = Get-LegacySeed; RunTwice = $true
            }
        }
        'corrupt' {
            return [pscustomobject]@{
                Name = 'corrupt'; Suffix = 'corrupt'; Kind = 'corrupt'
                Seed = 'garbage'; RunTwice = $false
            }
        }
        'corrupt-badvalues' {
            return [pscustomobject]@{
                Name = 'corrupt-badvalues'; Suffix = 'corruptbad'; Kind = 'corrupt'
                Seed = 'badvalues'; RunTwice = $false
            }
        }
        'oob' {
            $seed = [ordered]@{
                'board.grid'            = 1
                'board.h'               = 800
                'board.solid'           = 0
                'board.w'               = 760
                'board.x'               = 9000
                'board.y'               = -200
                'card.account.expanded' = 0
                'card.account.visible'  = 1
                'card.account.x'        = -500
                'card.account.y'        = -300
                'card.ps.expanded'      = 1
                'card.ps.visible'       = 1
                'card.ps.x'             = 40
                'card.ps.y'             = 32
                'card.servers.expanded' = 1
                'card.servers.visible'  = 1
                'card.servers.x'        = 9000
                'card.servers.y'        = 9000
            }
            return [pscustomobject]@{
                Name = 'oob'; Suffix = 'oob'; Kind = 'out-of-bounds'
                Seed = $seed; RunTwice = $false
            }
        }
        'cannot-fit' {
            # Smallest board (ClampBoard minimum 400x420): the interior is only
            # 372x358, so the 568 px Wide terminal card cannot be planned.
            $seed = [ordered]@{
                'board.grid'            = 1
                'board.h'               = 420
                'board.solid'           = 0
                'board.version'         = 2
                'board.w'               = 400
                'board.x'               = 300
                'board.y'               = 300
                'card.account.expanded' = 1
                'card.account.visible'  = 1
                'card.account.x'        = 0
                'card.account.y'        = 0
                'card.ps.expanded'      = 1
                'card.ps.visible'       = 1
                'card.ps.x'             = 0
                'card.ps.y'             = 0
                'card.servers.expanded' = 1
                'card.servers.visible'  = 1
                'card.servers.x'        = 0
                'card.servers.y'        = 0
            }
            return [pscustomobject]@{
                Name = 'cannot-fit'; Suffix = 'cannotfit'; Kind = 'cannot-fit'
                Seed = $seed; RunTwice = $false
            }
        }
        'cannot-fit-huge' {
            $seed = [ordered]@{
                'board.grid'            = 1
                'board.h'               = 3000
                'board.solid'           = 0
                'board.version'         = 2
                'board.w'               = 4000
                'board.x'               = 0
                'board.y'               = 0
                'card.account.expanded' = 1
                'card.account.visible'  = 1
                'card.account.x'        = 3900
                'card.account.y'        = 2900
                'card.ps.expanded'      = 1
                'card.ps.visible'       = 1
                'card.ps.x'             = 2000
                'card.ps.y'             = 2900
                'card.servers.expanded' = 1
                'card.servers.visible'  = 1
                'card.servers.x'        = 3900
                'card.servers.y'        = 2500
            }
            return [pscustomobject]@{
                Name = 'cannot-fit-huge'; Suffix = 'huge'; Kind = 'cannot-fit-huge'
                Seed = $seed; RunTwice = $false
            }
        }
        'drag-release' {
            # Fresh default layout, then drag the PS card far past the board edge
            # and release: the board must resolve the nearest legal position
            # (inside the interior, 12px gaps) instead of dropping it outside.
            return [pscustomobject]@{
                Name = 'drag-release'; Suffix = 'drag'; Kind = 'drag'
                Seed = $null; RunTwice = $false
            }
        }
        'drag-small-board' {
            # A v2 400x420 board cannot hold the 568 px terminal at that size.
            # No arrangement with the shipped three modules can exhaust the
            # grown right-half board, so this scenario verifies the grown
            # minimal board plus drag release (the true "no legal position"
            # path is pinned by tests/board_composition.cpp).
            $seed = [ordered]@{
                'board.grid'            = 1
                'board.h'               = 420
                'board.solid'           = 0
                'board.version'         = 2
                'board.w'               = 400
                'board.x'               = 300
                'board.y'               = 300
                'card.account.expanded' = 1
                'card.account.visible'  = 1
                'card.account.x'        = 0
                'card.account.y'        = 0
                'card.ps.expanded'      = 1
                'card.ps.visible'       = 1
                'card.ps.x'             = 0
                'card.ps.y'             = 0
                'card.servers.expanded' = 1
                'card.servers.visible'  = 1
                'card.servers.x'        = 0
                'card.servers.y'        = 0
            }
            return [pscustomobject]@{
                Name = 'drag-small-board'; Suffix = 'dragsmall'; Kind = 'drag-noslot'
                Seed = $seed; RunTwice = $false
            }
        }
        'reset-panel' {
            # A v2 760x800 board in the middle of the screen; the board menu's
            # "Reset to right standard panel" must back the config up and move
            # the board back to 1280x1310 at the right edge.
            $seed = [ordered]@{
                'board.grid'            = 1
                'board.h'               = 800
                'board.solid'           = 0
                'board.version'         = 2
                'board.w'               = 760
                'board.x'               = 500
                'board.y'               = 300
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
            }
            return [pscustomobject]@{
                Name = 'reset-panel'; Suffix = 'reset'; Kind = 'reset'
                Seed = $seed; RunTwice = $false
            }
        }
        'drag-cancel' {
            # Interrupted drag (WM_CAPTURECHANGED, e.g. Alt+Tab): the card must
            # return to its pre-drag position instead of being saved wherever
            # the pointer was.
            $seed = [ordered]@{
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
            }
            return [pscustomobject]@{
                Name = 'drag-cancel'; Suffix = 'dragcancel'; Kind = 'drag-cancel'
                Seed = $seed; RunTwice = $false
            }
        }
        'snapshot-clear' {
            # Review fix (P1 #1): create -> clear -> restart. The seeded saved
            # board does NOT fit the work area, so startup keeps it pending
            # (restore must not be faked); Reset then clears the snapshot, we
            # wait for the background save, restart, and saved.* must be absent.
            $seed = [ordered]@{
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
                'card.ps.expanded'      = 0
                'card.ps.visible'       = 1
                'card.ps.x'             = 14
                'card.ps.y'             = 192
                'card.servers.expanded' = 0
                'card.servers.visible'  = 1
                'card.servers.x'        = 366
                'card.servers.y'        = 32
                'saved.board.h'         = 2000
                'saved.board.w'         = 3000
                'saved.board.x'         = 100
                'saved.board.y'         = 50
                'saved.card.account.x'  = 2000
                'saved.card.account.y'  = 1500
                'saved.card.ps.x'       = 100
                'saved.card.ps.y'       = 1700
                'saved.card.servers.x'  = 2400
                'saved.card.servers.y'  = 1500
                'saved.pending'         = 1
            }
            return [pscustomobject]@{
                Name = 'snapshot-clear'; Suffix = 'snapshot'; Kind = 'snapshot'
                Seed = $seed; RunTwice = $false
            }
        }
        'adapt-clamp' {
            # Review fix (P1 #3): a v2 board larger than the work area is clamped
            # at startup and the ORIGINAL rect is snapshotted under saved.*, so a
            # later Reset can clear it and the restart keeps it cleared.
            $seed = [ordered]@{
                'board.grid'            = 1
                'board.h'               = 2000
                'board.solid'           = 0
                'board.version'         = 2
                'board.w'               = 3000
                'board.x'               = 0
                'board.y'               = 0
                'card.account.expanded' = 0
                'card.account.visible'  = 1
                'card.account.x'        = 14
                'card.account.y'        = 32
                'card.ps.expanded'      = 0
                'card.ps.visible'       = 1
                'card.ps.x'             = 14
                'card.ps.y'             = 192
                'card.servers.expanded' = 0
                'card.servers.visible'  = 1
                'card.servers.x'        = 366
                'card.servers.y'        = 32
            }
            return [pscustomobject]@{
                Name = 'adapt-clamp'; Suffix = 'adapt'; Kind = 'adapt'
                Seed = $seed; RunTwice = $false
            }
        }
        'restore-strict' {
            # Review fix (P1 #2): a saved snapshot that does NOT fit the current
            # work area must be kept (pending=1, saved board 3000x2000), and the
            # actual layout must stay legal.
            $seed = [ordered]@{
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
                'card.ps.expanded'      = 0
                'card.ps.visible'       = 1
                'card.ps.x'             = 14
                'card.ps.y'             = 192
                'card.servers.expanded' = 0
                'card.servers.visible'  = 1
                'card.servers.x'        = 366
                'card.servers.y'        = 32
                'saved.board.h'         = 2000
                'saved.board.w'         = 3000
                'saved.board.x'         = 100
                'saved.board.y'         = 50
                'saved.card.account.x'  = 2000
                'saved.card.account.y'  = 1500
                'saved.card.ps.x'       = 100
                'saved.card.ps.y'       = 1700
                'saved.card.servers.x'  = 2400
                'saved.card.servers.y'  = 1500
                'saved.pending'         = 1
            }
            return [pscustomobject]@{
                Name = 'restore-strict'; Suffix = 'restorestrict'; Kind = 'restore-strict'
                Seed = $seed; RunTwice = $false
            }
        }
        'consecutive-adapt' {
            # Second review round: two consecutive simulated work-area adaptions
            # must keep the ORIGINAL layout in saved.* (never snapshot the first
            # adapted rect), and going back to the original work area restores
            # the board and the card positions and clears saved.*.
            $seed = [ordered]@{
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
                'card.ps.expanded'      = 0
                'card.ps.visible'       = 1
                'card.ps.x'             = 14
                'card.ps.y'             = 192
                'card.servers.expanded' = 0
                'card.servers.visible'  = 1
                'card.servers.x'        = 366
                'card.servers.y'        = 32
            }
            return [pscustomobject]@{
                Name = 'consecutive-adapt'; Suffix = 'consecadapt'; Kind = 'consecutive-adapt'
                Seed = $seed; RunTwice = $false
                DisplayWork = '0,0,2560,1392'; DisplayDpi = 96
            }
        }
        'dpi-roundtrip' {
            # Second review round: simulated DPI change to 144 with a suggested
            # rect snapshots the original layout; going back to 96 with the
            # original suggested rect restores it and clears saved.*.
            $seed = [ordered]@{
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
                'card.ps.expanded'      = 0
                'card.ps.visible'       = 1
                'card.ps.x'             = 14
                'card.ps.y'             = 192
                'card.servers.expanded' = 0
                'card.servers.visible'  = 1
                'card.servers.x'        = 366
                'card.servers.y'        = 32
            }
            return [pscustomobject]@{
                Name = 'dpi-roundtrip'; Suffix = 'dpiround'; Kind = 'dpi'
                Seed = $seed; RunTwice = $false
                DisplayWork = '0,0,2560,1392'; DisplayDpi = 96
            }
        }
        'display-shim' {
            # Interface batch: DESK_TEST_DISPLAY_FILE + WM_APP+9/+10 stay as
            # compatibility shims over the new display source. This scenario
            # proves they still route: env file + message adapts/snapshots,
            # a malformed file is ignored safely, the shim restores, and the
            # messages are inert when the env var is absent.
            $seed = [ordered]@{
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
                'card.ps.expanded'      = 0
                'card.ps.visible'       = 1
                'card.ps.x'             = 14
                'card.ps.y'             = 192
                'card.servers.expanded' = 0
                'card.servers.visible'  = 1
                'card.servers.x'        = 366
                'card.servers.y'        = 32
            }
            return [pscustomobject]@{
                Name = 'display-shim'; Suffix = 'displayshim'; Kind = 'display-shim'
                Seed = $seed; RunTwice = $false
                DisplayWork = '0,0,2560,1392'; DisplayDpi = 96
            }
        }
    }
    throw "unknown scenario '$Name' (valid: fresh, legacy760, corrupt, corrupt-badvalues, oob, cannot-fit, cannot-fit-huge)"
}

# ---------------------------------------------------------------------------
# safety helpers
# ---------------------------------------------------------------------------

function Test-TestHook {
    param([string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $text = [System.Text.Encoding]::Unicode.GetString($bytes)
    return $text.Contains('DESK_TEST_INSTANCE')
}

function Get-LiveDeskProcess {
    $installed = Join-Path $env:LOCALAPPDATA 'Programs\Desktop Desk\desk.exe'
    return @(Get-Process -Name desk -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and ($_.Path -ieq $installed)
    })
}

function Get-ProcessHash {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Get-BackupFiles {
    param([string]$Directory)
    if (-not (Test-Path -LiteralPath $Directory)) { return @() }
    return @(Get-ChildItem -LiteralPath $Directory -File -ErrorAction SilentlyContinue |
        Where-Object {
            ($_.Name -match '(?i)(\.bak|\.backup|backup|\.prev|\.old)') -or
            (($_.Name -like '*desk.ini*') -and ($_.Name -ne 'desk.ini'))
        } |
        Select-Object -ExpandProperty Name)
}

function Get-AppDataDeskFingerprint {
    $dir = Join-Path $env:APPDATA 'Desktop Desk'
    $file = Join-Path $dir 'desk.ini'
    if (-not (Test-Path -LiteralPath $file)) { return 'absent' }
    $item = Get-Item -LiteralPath $file
    return ('{0}|{1}|{2}' -f $item.LastWriteTimeUtc.Ticks, $item.Length, (Get-ProcessHash $file))
}

# ---------------------------------------------------------------------------
# scenario plumbing
# ---------------------------------------------------------------------------

function Stop-TestRunProcesses {
    # Only processes whose image lives under build	estrun are touched. The
    # installed app (and its PID/config) is never in scope.
    $testFull = [System.IO.Path]::GetFullPath($TestRoot)
    $stopped = @()
    foreach ($process in @(Get-Process -Name desk -ErrorAction SilentlyContinue)) {
        $path = ''
        if ($process.Path) { $path = [System.IO.Path]::GetFullPath($process.Path) }
        if ($path -and $path.StartsWith($testFull, [System.StringComparison]::OrdinalIgnoreCase)) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $stopped += $process.Id
        }
    }
    if ($stopped.Count -gt 0) { Start-Sleep -Milliseconds 400 }
    return $stopped
}

function Initialize-ScenarioDir {
    param([string]$Path)
    $full = [System.IO.Path]::GetFullPath($Path)
    $rootFull = [System.IO.Path]::GetFullPath($TestRoot)
    if (-not $full.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to clean '$full' (outside $rootFull)"
    }
    if (Test-Path -LiteralPath $full) {
        try {
            Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction Stop
        } catch {
            # A previous run may still hold desk.exe; the harness only ever
            # stops its own testrun processes (checked by image path).
            [void](Stop-TestRunProcesses)
            Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction Stop
        }
    }
    New-Item -ItemType Directory -Path $full -Force | Out-Null
    return $full
}

function Install-TestExe {
    param([string]$Source, [string]$Destination, [bool]$Sign)
    $copy = Join-Path $Destination 'desk.exe'
    [void][System.IO.File]::Copy($Source, $copy, $true)
    $signed = $false
    if ($Sign) {
        & (Join-Path $PSScriptRoot 'sign-local.ps1') -Path $copy | Out-Host
        $signed = $true
    }
    return [pscustomobject]@{ Path = $copy; Signed = $signed; Hash = (Get-ProcessHash $copy) }
}

function Start-TestBoard {
    param([string]$ExePath, [string]$WorkingDirectory, [string]$Suffix, [string]$DisplayFile = '')
    # DETACHED_PROCESS + an explicit environment block: the test copy must not
    # inherit this console (otherwise the ConPTY client inside the POWERSHELL
    # card attaches to it and the card shows EXITED). The display/DPI scenarios
    # additionally pass DESK_TEST_DISPLAY_FILE.
    $processId = Start-DeskTestInstance -ExePath $ExePath -WorkingDirectory $WorkingDirectory -Suffix $Suffix -DisplayFile $DisplayFile
    $script:LaunchedPids.Add($processId)
    return $processId
}

function Test-TestProcessAlive {
    param([int]$ProcessId)
    return [bool](Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
}

function Get-TestProcessPath {
    param([int]$ProcessId)
    $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($process -and $process.Path) { return $process.Path }
    return ''
}

function Wait-TestProcessExit {
    param([int]$ProcessId, [int]$TimeoutMs)
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        if (-not (Test-TestProcessAlive -ProcessId $ProcessId)) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return -not (Test-TestProcessAlive -ProcessId $ProcessId)
}

function Get-BoardWindow {
    param([int]$ProcessId, [string]$Suffix)
    return @(Get-DeskWindowList -ProcessId $ProcessId | Where-Object {
        $_.Visible -and ($_.Class -eq "wg.desk-$Suffix" -or $_.Class -eq 'wg.desk')
    }) | Select-Object -First 1
}

function Wait-BoardWindowRect {
    param([int]$ProcessId, [string]$Suffix, [scriptblock]$Predicate, [int]$TimeoutSeconds = 6)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $latest = $null
    while ((Get-Date) -lt $deadline) {
        $latest = Get-BoardWindow -ProcessId $ProcessId -Suffix $Suffix
        if ($latest -and (& $Predicate $latest)) { return $latest }
        Start-Sleep -Milliseconds 100
    }
    return $latest
}

function Get-WindowTextDump {
    param([IntPtr]$Hwnd)
    $parts = New-Object System.Collections.Generic.List[string]
    $class = New-Object System.Text.StringBuilder 256
    [void][DeskTestNative]::GetClassName($Hwnd, $class, $class.Capacity)
    $title = New-Object System.Text.StringBuilder 512
    [void][DeskTestNative]::GetWindowText($Hwnd, $title, $title.Capacity)
    $parts.Add(("[{0}] {1}" -f $class.ToString(), $title.ToString()))
    return $parts
}

function Wait-BoardWindow {
    param([int]$ProcessId, [string]$Suffix, [int]$TimeoutSeconds)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $expectedClass = "wg.desk-$Suffix"
    $sawDialog = @()
    while ((Get-Date) -lt $deadline) {
        if (-not (Test-TestProcessAlive -ProcessId $ProcessId)) {
            return [pscustomobject]@{ Window = $null; Exited = $true; ExitCode = 'exited'; Dialogs = $sawDialog }
        }
        $windows = @(Get-DeskWindowList -ProcessId $ProcessId)
        $board = $windows | Where-Object {
            $_.Visible -and (($_.Class -eq $expectedClass) -or ($_.Class -eq 'wg.desk'))
        } | Select-Object -First 1
        if ($board) {
            return [pscustomobject]@{ Window = $board; Exited = $false; ExitCode = 0; Dialogs = $sawDialog }
        }
        $dialogs = @($windows | Where-Object { $_.Class -eq '#32770' -and $_.Visible })
        if ($dialogs.Count -gt 0) { $sawDialog = $dialogs }
        Start-Sleep -Milliseconds 100
    }
    $windows = @(Get-DeskWindowList -ProcessId $ProcessId)
    $board = $windows | Where-Object {
        $_.Visible -and (($_.Class -eq $expectedClass) -or ($_.Class -eq 'wg.desk'))
    } | Select-Object -First 1
    $exited = -not (Test-TestProcessAlive -ProcessId $ProcessId)
    return [pscustomobject]@{
        Window = $board; Exited = $exited
        ExitCode = $(if ($exited) { 'exited' } else { 0 })
        Dialogs = @($windows | Where-Object { $_.Class -eq '#32770' -and $_.Visible })
    }
}

function Wait-IniStable {
    param([string]$Path, [int]$TimeoutSeconds = 6)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $last = ''
    while ((Get-Date) -lt $deadline) {
        if (Test-Path -LiteralPath $Path) {
            $item = Get-Item -LiteralPath $Path
            $fingerprint = '{0}|{1}' -f $item.LastWriteTimeUtc.Ticks, $item.Length
            if ($fingerprint -eq $last) { return $true }
            $last = $fingerprint
        }
        Start-Sleep -Milliseconds 250
    }
    return (Test-Path -LiteralPath $Path)
}

function Close-TestBoard {
    param([int]$ProcessId, [object]$Window, [object]$Dialogs, [object]$Result)
    $graceful = $false
    # A modal startup dialog would block the board loop: close it first.
    if ($Dialogs -and $Dialogs.Count -gt 0) {
        foreach ($dialog in $Dialogs) {
            if ($dialog -and $dialog.Hwnd -ne [IntPtr]::Zero) {
                [void][DeskTestNative]::PostMessage($dialog.Hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
                Start-Sleep -Milliseconds 400
            }
        }
    }
    if ($Window -and $Window.Hwnd -ne [IntPtr]::Zero) {
        [void][DeskTestNative]::PostMessage($Window.Hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        $graceful = Wait-TestProcessExit -ProcessId $ProcessId -TimeoutMs 10000
    }
    if (Test-TestProcessAlive -ProcessId $ProcessId) {
        if ($KeepProcess) {
            Add-DeskNote $Result ("process {0} left running (-KeepProcess)" -f $ProcessId)
            return [pscustomobject]@{ Graceful = $false; Killed = $false; LeftRunning = $true }
        }
        # Fallback: stop only the exact PID this script started, and only after
        # checking it still runs the copied exe under build\testrun.
        $livePath = Get-TestProcessPath -ProcessId $ProcessId
        $testFull = [System.IO.Path]::GetFullPath($TestRoot)
        if ($livePath -and ([System.IO.Path]::GetFullPath($livePath)).StartsWith($testFull, [System.StringComparison]::OrdinalIgnoreCase)) {
            Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue
            Start-Sleep -Milliseconds 400
            return [pscustomobject]@{ Graceful = $false; Killed = $true; LeftRunning = $false }
        }
        return [pscustomobject]@{ Graceful = $false; Killed = $false; LeftRunning = $true }
    }
    return [pscustomobject]@{ Graceful = $graceful; Killed = $false; LeftRunning = $false }
}

# ---------------------------------------------------------------------------
# assertions
# ---------------------------------------------------------------------------


function ConvertTo-DeskGrid {
    # Round a distance up onto the 8 px layout grid.
    param([int]$Value, [int]$Grid = 8)
    if ($Value -le 0) { return 0 }
    return [int]([math]::Ceiling($Value / [double]$Grid) * $Grid)
}

function Invoke-DeskBoardMenuReset {
    # Right-click the board title strip, then activate "Reset to right standard
    # panel" through MSAA (Win32 popups ignore posted WM_KEYDOWN and are not in
    # the UIA tree). Shared by the reset/snapshot/adaptation scenarios.
    param([int]$ProcessId, [object]$Window, [object]$Result)
    [void][DeskTestNative]::PostMessage($Window.Hwnd, 0x0205, [IntPtr]::Zero, [DeskTestNative]::MakeLParam(54, 10))
    $menu = $null
    $deadline = (Get-Date).AddSeconds(3)
    while ((Get-Date) -lt $deadline -and -not $menu) {
        $menuWindows = @(Get-DeskWindowList -ProcessId $ProcessId | Where-Object { $_.Class -eq '#32768' })
        if ($menuWindows.Count -gt 0) { $menu = $menuWindows[0] } else { Start-Sleep -Milliseconds 100 }
    }
    $menuDetail = 'not found'
    if ($menu) { $menuDetail = $menu.Hwnd }
    [void](Add-DeskCheck $Result 'board menu opens' ($null -ne $menu) ("menu window: $menuDetail"))
    $invoked = $false
    if ($menu) {
        try {
            Add-Type -AssemblyName Accessibility -ErrorAction SilentlyContinue
            $iidAccessible = [Guid]'618736E0-3C3D-11CF-810C-00AA00389B71'
            $accessible = $null
            $objIdClient = [uint32]4294967292
            $hr = [MsaaNative]::AccessibleObjectFromWindow($menu.Hwnd, $objIdClient, [ref]$iidAccessible, [ref]$accessible)
            if ($hr -eq 0 -and $accessible) {
                $childCount = $accessible.accChildCount
                for ($child = 1; $child -le $childCount; $child++) {
                    $name = ''
                    try { $name = [string]$accessible.accName($child) } catch { continue }
                    if ($name -notlike 'Reset to right standard panel*') { continue }
                    $accessible.accDoDefaultAction($child)
                    $invoked = $true
                    break
                }
            }
        } catch {
            Add-DeskNote $Result ("MSAA menu activation failed: " + $_.Exception.Message)
        }
    }
    $Result.Evidence.Add("reset menu item invoked via MSAA: $invoked")
    return $invoked
}

function Wait-DeskSavedSnapshotState {
    # Waits until the asynchronous save has written the expected saved.* state.
    param([string]$Path, [bool]$Present, [int]$TimeoutSeconds = 8)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $latest = $null
    while ((Get-Date) -lt $deadline) {
        $latest = Read-DeskIniFile -Path $Path
        if ($latest) {
            $hasSaved = $latest.Contains('saved.pending') -or $latest.Contains('saved.board.w')
            if ($hasSaved -eq $Present) { return $latest }
        }
        Start-Sleep -Milliseconds 200
    }
    return $latest
}

function Get-DeskMinimalBoardArea {
    # Offline lower bound for "the smallest board that can hold these cards",
    # used to check that auto-growth did not skip a much smaller 8 px candidate.
    # Rectangles are combined guillotine-style (any packing of at most three
    # rectangles admits a guillotine cut), then rounded up onto the board grid.
    param([object[]]$Cards, [int]$MinWidth = 400, [int]$MinHeight = 420,
          [int]$MaxWidth = 1280, [int]$MaxHeight = 1392,
          [int]$Pad = 28, [int]$Chrome = 62, [int]$Gap = 12)

    $candidates = New-Object System.Collections.Generic.List[object]

    function Add-Combination {
        param([int]$RequiredW, [int]$RequiredH)
        $w = ConvertTo-DeskGrid -Value ([math]::Max($MinWidth - $Pad, $RequiredW))
        $w = [math]::Max($MinWidth, $w + $Pad)
        $h = ConvertTo-DeskGrid -Value ([math]::Max($MinHeight - $Chrome, $RequiredH))
        $h = [math]::Max($MinHeight, $h + $Chrome)
        $w = ConvertTo-DeskGrid -Value $w
        $h = ConvertTo-DeskGrid -Value $h
        if ($w -gt $MaxWidth -or $h -gt $MaxHeight) { return }
        $candidates.Add([pscustomobject]@{ W = $w; H = $h; Area = $w * $h })
    }

    $n = $Cards.Count
    if ($n -eq 1) {
        Add-Combination -RequiredW $Cards[0].W -RequiredH $Cards[0].H
    } elseif ($n -eq 2) {
        Add-Combination -RequiredW ($Cards[0].W + $Gap + $Cards[1].W) -RequiredH ([math]::Max($Cards[0].H, $Cards[1].H))
        Add-Combination -RequiredW ([math]::Max($Cards[0].W, $Cards[1].W)) -RequiredH ($Cards[0].H + $Gap + $Cards[1].H)
    } elseif ($n -eq 3) {
        $perms = @(
            @(0,1,2), @(0,2,1), @(1,0,2), @(1,2,0), @(2,0,1), @(2,1,0)
        )
        foreach ($p in $perms) {
            $a = $Cards[$p[0]]; $b = $Cards[$p[1]]; $c = $Cards[$p[2]]
            Add-Combination -RequiredW ([math]::Max($a.W, [math]::Max($b.W, $c.W))) -RequiredH ($a.H + $Gap + $b.H + $Gap + $c.H)
            Add-Combination -RequiredW ($a.W + $Gap + $b.W + $Gap + $c.W) -RequiredH ([math]::Max($a.H, [math]::Max($b.H, $c.H)))
            Add-Combination -RequiredW ($a.W + $Gap + [math]::Max($b.W, $c.W)) -RequiredH ([math]::Max($a.H, $b.H + $Gap + $c.H))
            Add-Combination -RequiredW ([math]::Max($a.W, $b.W + $Gap + $c.W)) -RequiredH ($a.H + $Gap + [math]::Max($b.H, $c.H))
            Add-Combination -RequiredW ([math]::Max($a.W, $b.W) + $Gap + $c.W) -RequiredH ([math]::Max($a.H + $Gap + $b.H, $c.H))
            Add-Combination -RequiredW ([math]::Max($a.W + $Gap + $b.W, $c.W)) -RequiredH ([math]::Max($a.H, $b.H) + $Gap + $c.H)
        }
    }
    if ($candidates.Count -eq 0) { return $null }
    return ($candidates | Sort-Object Area | Select-Object -First 1)
}

function Get-DeskCardRects {
    # Rectangles for every visible card, using desk.ini coordinates.
    param([object]$Ini)
    $rects = @()
    foreach ($id in @('account', 'servers', 'ps')) {
        $xKey = "card.$id.x"; $yKey = "card.$id.y"; $vKey = "card.$id.visible"
        if (-not $Ini.Contains($xKey) -or -not $Ini.Contains($yKey)) { continue }
        if ((Get-DeskIniInt $Ini $vKey 1) -eq 0) { continue }
        $expanded = (Get-DeskIniInt $Ini "card.$id.expanded" 0) -ne 0
        $width = 340
        if ($id -eq 'ps') { $width = 568 }
        $height = Get-DeskCardHeight -Id $id -Expanded $expanded
        $rects += [pscustomobject]@{
            Id = $id
            Rect = (Get-DeskCardRect -BoardX 0 -BoardY 0 -CardX (Get-DeskIniInt $Ini $xKey) -CardY (Get-DeskIniInt $Ini $yKey) -CardW $width -CardH $height)
        }
    }
    return $rects
}

function Test-DeskCardGrid {
    # Review fix #6: card coordinates must sit on the 8 px grid measured from the
    # interior origin (14,32) after migration, restore adaptations and Reset.
    param([object]$Ini, [object]$Interior, [object]$Result, [string]$Prefix)
    $offGrid = @()
    foreach ($entry in (Get-DeskCardRects -Ini $Ini)) {
        $dx = $entry.Rect.X - $Interior.X
        $dy = $entry.Rect.Y - $Interior.Y
        if (($dx % 8) -ne 0 -or ($dy % 8) -ne 0) {
            $offGrid += ("{0}=({1},{2}) offsets ({3},{4})" -f $entry.Id, $entry.Rect.X, $entry.Rect.Y, $dx, $dy)
        }
    }
    [void](Add-DeskCheck $Result ("$Prefix card coordinates sit on the 8 px grid") ($offGrid.Count -eq 0) ($offGrid -join '; '))
}

function Test-CardsClampedOnBoard {
    # Weaker invariant used when the layout cannot be completed: every card must
    # still live on the board (its origin inside the interior) instead of being
    # lost off-screen or at a negative coordinate.
    param([object]$Ini, [object]$Interior, [object]$Result, [string]$Prefix)
    $ok = $true
    foreach ($entry in (Get-DeskCardRects -Ini $Ini)) {
        $rect = $entry.Rect
        $clamped = ($rect.X -ge $Interior.X) -and ($rect.Y -ge $Interior.Y) -and
                   ($rect.X -lt ($Interior.X + $Interior.W)) -and ($rect.Y -lt ($Interior.Y + $Interior.H))
        [void](Add-DeskCheck $Result ("$Prefix card $($entry.Id) clamped on board") $clamped ("({0},{1}) {2}x{3} vs interior ({4},{5}) {6}x{7}" -f $rect.X, $rect.Y, $rect.W, $rect.H, $Interior.X, $Interior.Y, $Interior.W, $Interior.H))
        if (-not $clamped) { $ok = $false }
    }
    return $ok
}

function Test-CardsInsideInterior {
    param([object]$Ini, [object]$Interior, [object]$Result, [string]$Prefix)
    $ids = @('account', 'servers', 'ps')
    $cardRects = @()
    $ok = $true
    foreach ($id in $ids) {
        $xKey = "card.$id.x"
        $yKey = "card.$id.y"
        $vKey = "card.$id.visible"
        if (-not $Ini.Contains($xKey) -or -not $Ini.Contains($yKey)) { continue }
        if ((Get-DeskIniInt $Ini $vKey 1) -eq 0) { continue }
        $expanded = (Get-DeskIniInt $Ini "card.$id.expanded" 0) -ne 0
        $width = 340
        if ($id -eq 'ps') { $width = 568 }
        $height = Get-DeskCardHeight -Id $id -Expanded $expanded
        $rect = Get-DeskCardRect -BoardX 0 -BoardY 0 -CardX (Get-DeskIniInt $Ini $xKey) -CardY (Get-DeskIniInt $Ini $yKey) -CardW $width -CardH $height
        $cardRects += [pscustomobject]@{ Id = $id; Rect = $rect }
        $inside = ($rect.X -ge $Interior.X -and $rect.Y -ge $Interior.Y -and
                   $rect.Right -le ($Interior.X + $Interior.W) -and
                   $rect.Bottom -le ($Interior.Y + $Interior.H))
        if (-not $inside) {
            $ok = $false
            [void](Add-DeskCheck $Result ("$Prefix card $id inside interior") $false ("({0},{1}) {2}x{3} not inside ({4},{5}) {6}x{7}" -f $rect.X, $rect.Y, $rect.W, $rect.H, $Interior.X, $Interior.Y, $Interior.W, $Interior.H))
        } else {
            [void](Add-DeskCheck $Result ("$Prefix card $id inside interior") $true)
        }
    }
    # 12 px minimum gap between cards.
    for ($i = 0; $i -lt $cardRects.Count; $i++) {
        for ($j = $i + 1; $j -lt $cardRects.Count; $j++) {
            $a = $cardRects[$i]; $b = $cardRects[$j]
            $overlap = Get-DeskOverlap -A $a.Rect -B $b.Rect -Gap 12
            [void](Add-DeskCheck $Result ("$Prefix gap $($a.Id)/$($b.Id) >= 12px") (-not $overlap) $(if ($overlap) { 'rectangles closer than 12px' } else { '' }))
            if ($overlap) { $ok = $false }
        }
    }
    return $ok
}

function Invoke-Scenario {
    param([object]$Definition, [string]$ExePath, [bool]$Sign, [string]$BaseRunDir)

    $result = New-DeskResult
    $result.Name = $Definition.Name
    $scenarioDir = Join-Path $TestRoot $Definition.Name
    $evidenceDir = Join-Path $BaseRunDir $Definition.Name
    New-Item -ItemType Directory -Path $evidenceDir -Force | Out-Null

    Write-Host ""
    Write-Host ("=== scenario {0} (suffix {1})" -f $Definition.Name, $Definition.Suffix)

    if (-not $DryRun) {
        [void](Initialize-ScenarioDir $scenarioDir)
        $installed = Install-TestExe -Source $ExePath -Destination $scenarioDir -Sign $Sign
        $result.Evidence.Add("exe=$($installed.Path)")
        $result.Evidence.Add("exe.sha256=$($installed.Hash)")
        Add-DeskCheck $result 'test exe installed' (Test-Path -LiteralPath $installed.Path)
    }

    # --- seed config -------------------------------------------------------
    $iniPath = Join-Path $scenarioDir 'desk.ini'
    if (-not $DryRun) {
        switch ($Definition.Kind) {
            'corrupt' {
                if ($Definition.Seed -eq 'badvalues') {
                    Write-DeskIniFile -Path $iniPath -Values ([ordered]@{
                        'board.grid'            = 1
                        'board.h'               = 'not-a-number'
                        'board.solid'           = 0
                        'board.w'               = -5
                        'board.x'               = 'abc'
                        'board.y'               = 64
                        'card.account.expanded' = 2
                        'card.account.visible'  = 1
                        'card.account.x'        = 'xyz'
                        'card.account.y'        = -40
                        'card.ps.expanded'      = 1
                        'card.ps.visible'       = 1
                        'card.ps.x'             = 40
                        'card.ps.y'             = 200
                        'card.servers.visible'  = 1
                    })
                } else {
                    # Unparsable bytes: no '=' pairs at all, so Store sees
                    # "state exists" but no keys (hadState = true).
                    $random = New-Object byte[] 257
                    (New-Object System.Random 20261106).NextBytes($random)
                    [System.IO.File]::WriteAllBytes($iniPath, $random)
                }
            }
            default {
                if ($Definition.Seed -is [System.Collections.IDictionary]) {
                    Write-DeskIniFile -Path $iniPath -Values $Definition.Seed
                }
            }
        }
        if (Test-Path -LiteralPath $iniPath) {
            Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.seed') -Force
            $result.Evidence.Add("desk.ini.seed=$evidenceDir\desk.ini.seed")
        }
    }

    if ($DryRun) {
        Add-DeskNote $result ("dry run: would launch {0} with DESK_TEST_INSTANCE={1} in {2}" -f $ExePath, $Definition.Suffix, $scenarioDir)
        Add-DeskCheck $result 'dry run completes' $true
        [void](Save-DeskResult -Result $result -Path (Join-Path $evidenceDir 'result.json'))
        return $result
    }

    # --- safety snapshots --------------------------------------------------
    $runKeyBefore = Get-DeskRunKeySnapshot
    $appDataBefore = Get-AppDataDeskFingerprint
    $backupsBefore = @(Get-BackupFiles -Directory $scenarioDir)

    # --- launch ------------------------------------------------------------
    $displayPath = Join-Path $scenarioDir 'display.txt'
    $displayFile = ''
    if ($Definition.PSObject.Properties['DisplayWork'] -and $Definition.DisplayWork) {
        Write-DeskDisplayFile -Path $displayPath -Work $Definition.DisplayWork -Dpi $(if ($Definition.DisplayDpi) { $Definition.DisplayDpi } else { 96 })
        Copy-Item -LiteralPath $displayPath -Destination (Join-Path $evidenceDir 'display.initial.txt') -Force
        $displayFile = $displayPath
        $result.Evidence.Add("display file initial: work=$($Definition.DisplayWork) dpi=$($Definition.DisplayDpi)")
    }
    # Startup timing (used mainly for the cannot-fit growth-search cost): from
    # process launch to the first board window and to a settled desk.ini.
    $startupWatch = [System.Diagnostics.Stopwatch]::StartNew()
    $processId = Start-TestBoard -ExePath (Join-Path $scenarioDir 'desk.exe') -WorkingDirectory $scenarioDir -Suffix $Definition.Suffix -DisplayFile $displayFile
    $result.Evidence.Add("process.id=$processId")

    $wait = Wait-BoardWindow -ProcessId $processId -Suffix $Definition.Suffix -TimeoutSeconds $TimeoutSeconds
    $window = $wait.Window
    $timeToWindowMs = $startupWatch.ElapsedMilliseconds
    Add-DeskCheck $result 'process still running after startup' (-not $wait.Exited) ("exitCode=$($wait.ExitCode)")
    $windowDetail = 'no wg.desk window'
    if ($window) { $windowDetail = "class=$($window.Class) rect=$($window.X),$($window.Y) $($window.W)x$($window.H)" }
    Add-DeskCheck $result 'board window created' ($null -ne $window) $windowDetail
    if ($wait.Dialogs -and $wait.Dialogs.Count -gt 0) {
        foreach ($dialog in $wait.Dialogs) {
            $dump = (Get-WindowTextDump $dialog.Hwnd) -join ' / '
            Add-DeskNote $result ("startup dialog: $dump")
            $result.Evidence.Add("dialog=$dump")
        }
    }

    if (-not $window) {
        $close = Close-TestBoard -ProcessId $processId -Window $null -Dialogs $wait.Dialogs -Result $result
        Add-DeskCheck $result 'graceful close' ($close.Graceful -or $close.LeftRunning)
        [void](Save-DeskResult -Result $result -Path (Join-Path $evidenceDir 'result.json'))
        return $result
    }

    # Let the initial save settle (SaveState runs right after the window shows).
    [void](Wait-IniStable -Path $iniPath -TimeoutSeconds 6)
    Start-Sleep -Milliseconds 600
    $timeToSettledMs = $startupWatch.ElapsedMilliseconds
    $result.Evidence.Add(("startup timing: window={0}ms settled={1}ms (kind={2})" -f $timeToWindowMs, $timeToSettledMs, $Definition.Kind))

    $work = Get-DeskWorkArea
    $result.Evidence.Add(("workarea={0},{1} {2}x{3}" -f $work.X, $work.Y, $work.W, $work.H))
    $result.Evidence.Add(("boardrect={0},{1} {2}x{3}" -f $window.X, $window.Y, $window.W, $window.H))

    # --- quality of the started window -------------------------------------
    Add-DeskCheck $result 'window fully inside work area' (
        $window.X -ge $work.X -and $window.Y -ge $work.Y -and
        $window.Right -le $work.Right -and $window.Bottom -le $work.Bottom) (
        "board $($window.X),$($window.Y) $($window.W)x$($window.H) vs work $($work.X),$($work.Y) $($work.W)x$($work.H)")

    $ini = Read-DeskIniFile -Path $iniPath
    $iniDetail = 'missing desk.ini'
    if ($ini) { $iniDetail = "keys=$($ini.Count)" }
    Add-DeskCheck $result 'desk.ini written and parseable' ($null -ne $ini -and $ini.Contains('board.w') -and $ini.Contains('board.h')) $iniDetail

    if ($ini) {
        Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after') -Force
        $result.Evidence.Add("desk.ini.after=$evidenceDir\desk.ini.after")
        $boardX = Get-DeskIniInt $ini 'board.x' 0
        $boardY = Get-DeskIniInt $ini 'board.y' 0
        $boardW = Get-DeskIniInt $ini 'board.w' 0
        $boardH = Get-DeskIniInt $ini 'board.h' 0
        Add-DeskCheck $result 'desk.ini board rect plausible' ($boardW -ge 400 -and $boardH -ge 420) ("{0},{1} {2}x{3}" -f $boardX, $boardY, $boardW, $boardH)
        $interior = Get-DeskInterior -BoardX 0 -BoardY 0 -BoardW $boardW -BoardH $boardH
        if ($Definition.Kind -eq 'cannot-fit-huge' -or $Definition.Kind -eq 'cannot-fit' -or $Definition.Kind -eq 'drag-noslot') {
            # Cards come back either fully arranged or merely clamped.
            $rects = @(Get-DeskCardRects -Ini $ini)
            $allInside = $true
            foreach ($entry in $rects) {
                $rect = $entry.Rect
                if (-not ($rect.X -ge $interior.X -and $rect.Y -ge $interior.Y -and
                          $rect.Right -le ($interior.X + $interior.W) -and
                          $rect.Bottom -le ($interior.Y + $interior.H))) { $allInside = $false }
            }
            if ($allInside) {
                [void](Test-CardsInsideInterior -Ini $ini -Interior $interior -Result $result -Prefix 'desk.ini')
            } else {
                Add-DeskNote $result 'layout fell back to clamped cards after the huge-board clamp'
                [void](Test-CardsClampedOnBoard -Ini $ini -Interior $interior -Result $result -Prefix 'fallback')
            }
        } else {
            [void](Test-CardsInsideInterior -Ini $ini -Interior $interior -Result $result -Prefix 'desk.ini')
        }
    }

    # --- screenshot + process stats ---------------------------------------
    $shot = Save-DeskWindowShot -Hwnd $window.Hwnd -X $window.X -Y $window.Y -W $window.W -H $window.H -Path (Join-Path $evidenceDir 'board.png')
    $result.Evidence.Add("screenshot=$($shot.Path) (mode=$($shot.Mode) $($shot.Stats))")
    Add-DeskCheck $result 'screenshot captured' (Test-Path -LiteralPath $shot.Path) ("mode=$($shot.Mode) $($shot.Stats)")

    $stats = Get-DeskProcessStats -ProcessId $processId
    if ($stats) {
        $result.Evidence.Add(("process ws={0}MB private={1}MB threads={2} gdi={3} user={4}" -f $stats.WSMB, $stats.PrivateMB, $stats.Threads, $stats.GDI, $stats.USER))
    }
    # Startup work (shell spawn for the PowerShell card, first paint, worker
    # tail) is not idle: wait for a quiet 1 s probe before sampling, otherwise
    # the first scenario would measure startup CPU instead of idle CPU.
    $warm = Measure-DeskCpu -ProcessId $processId -Seconds 1
    $warmTries = 0
    while ($warm -and $warm.CpuMs -gt 25 -and $warmTries -lt 8) {
        Start-Sleep -Milliseconds 700
        $warm = Measure-DeskCpu -ProcessId $processId -Seconds 1
        $warmTries += 1
    }
    if ($warm) { $result.Evidence.Add(("idle warm-up: {0} ms/1 s after {1} extra probe(s)" -f $warm.CpuMs, $warmTries)) }
    $cpu = Measure-DeskCpu -ProcessId $processId -Seconds $IdleSeconds
    if ($cpu) {
        $result.Evidence.Add(("idle cpu {0}ms over {1}s" -f $cpu.CpuMs, $cpu.Seconds))
        Add-DeskCheck $result 'idle CPU below 2% of one core' ($cpu.CpuPct -lt 2.0) ("$($cpu.CpuMs) ms / $($cpu.Seconds) s = $($cpu.CpuPct)%")
    }

    # --- scenario-specific assertions -------------------------------------
    switch ($Definition.Kind) {
        'default' {
            Add-DeskCheck $result 'fresh board is 1280x1310 at (1280,0)' (
                $window.X -eq 1280 -and $window.Y -eq 0 -and $window.W -eq 1280 -and $window.H -eq 1310) (
                "$($window.X),$($window.Y) $($window.W)x$($window.H)")
            Add-DeskCheck $result 'fresh board flush to right edge of work area' (
                ($window.X + $window.W) -eq $work.Right) ("right=$($window.Right) work.right=$($work.Right)")
            Add-DeskCheck $result 'fresh interior is 1252x1248' (
                ($window.W - 28) -eq 1252 -and ($window.H - 62) -eq 1248) ("interior $($window.W - 28)x$($window.H - 62)")
            # First-run shelf rule (review fix #1): every seeded coordinate is on
            # the 8 px grid relative to the interior origin (14,32); row one
            # starts at the origin, Servers sits one 12 px gap right of Account,
            # and the terminal starts one 12 px gap below the taller of the two,
            # rounded up onto the grid. With an empty config the Servers card is
            # its 112 px Standard minimum and Account 144 px, so
            # ps.y = 32 + grid(144 + 12) = 192 (it was 188 before the fix);
            # with >=2 host rows the same rule gives 224.
            if ($ini) {
                $shelfInterior = Get-DeskInterior -BoardX 0 -BoardY 0 -BoardW 1280 -BoardH 1310
                $accountX = Get-DeskIniInt $ini 'card.account.x' -9999
                $accountY = Get-DeskIniInt $ini 'card.account.y' -9999
                $serversX = Get-DeskIniInt $ini 'card.servers.x' -9999
                $serversY = Get-DeskIniInt $ini 'card.servers.y' -9999
                $psX = Get-DeskIniInt $ini 'card.ps.x' -9999
                $psY = Get-DeskIniInt $ini 'card.ps.y' -9999
                $accountExpanded = (Get-DeskIniInt $ini 'card.account.expanded' 0) -ne 0
                $serversExpanded = (Get-DeskIniInt $ini 'card.servers.expanded' 0) -ne 0
                $accountH = Get-DeskCardHeight -Id 'account' -Expanded $accountExpanded
                $serversH = Get-DeskCardHeight -Id 'servers' -Expanded $serversExpanded
                $row2Offset = ConvertTo-DeskGrid -Value ([math]::Max($accountY + $accountH, $serversY + $serversH) + 12 - $shelfInterior.Y)
                $shelfRow2Y = $shelfInterior.Y + $row2Offset
                $shelfOk = ($accountX -eq $shelfInterior.X) -and
                           ($accountY -eq $shelfInterior.Y) -and
                           ($serversY -eq $shelfInterior.Y) -and
                           ($serversX -eq ($accountX + 340 + 12)) -and
                           ($psX -eq $shelfInterior.X) -and
                           ($psY -eq $shelfRow2Y)
                $shelfDetail = ("account=({0},{1}) servers=({2},{3}) ps=({4},{5}); expected ps.y={6} (row1 bottoms: account {7}, servers {8})" -f $accountX, $accountY, $serversX, $serversY, $psX, $psY, $shelfRow2Y, ($accountY + $accountH), ($serversY + $serversH))
                [void](Add-DeskCheck $result 'fresh cards use the reference shelf rule' $shelfOk $shelfDetail)
                # Review fix #1: all seeded card coordinates on the 8 px grid.
                $offGrid = @()
                foreach ($entry in @(
                    @{ id = 'account'; x = $accountX; y = $accountY },
                    @{ id = 'servers'; x = $serversX; y = $serversY },
                    @{ id = 'ps'; x = $psX; y = $psY })) {
                    $dx = $entry.x - $shelfInterior.X
                    $dy = $entry.y - $shelfInterior.Y
                    if (($dx % 8) -ne 0 -or ($dy % 8) -ne 0) {
                        $offGrid += ("{0}=({1},{2}) offsets ({3},{4})" -f $entry.id, $entry.x, $entry.y, $dx, $dy)
                    }
                }
                [void](Add-DeskCheck $result 'fresh card coordinates sit on the 8 px grid' ($offGrid.Count -eq 0) ($offGrid -join '; '))
            }

            # Single-instance scope: a second copy for the same board id must
            # exit without creating a second window and without touching the
            # first one.
            $secondId = Start-TestBoard -ExePath (Join-Path $scenarioDir 'desk.exe') -WorkingDirectory $scenarioDir -Suffix $Definition.Suffix
            $secondExit = Wait-TestProcessExit -ProcessId $secondId -TimeoutMs 8000
            Add-DeskCheck $result 'second instance for same board id exits (mutex scoped)' $secondExit ("pid=$secondId")
            Add-DeskCheck $result 'first instance unaffected by second launch' (Test-TestProcessAlive -ProcessId $processId)
        }
        'migration' {
            $backupsAfter = @(Get-BackupFiles -Directory $scenarioDir)
            $newBackups = @($backupsAfter | Where-Object { $backupsBefore -notcontains $_ })
            Add-DeskCheck $result 'migration created a config backup' ($newBackups.Count -ge 1) ("backups: $($newBackups -join ', ')")
            $result.Evidence.Add("backups=$($backupsAfter -join ',')")
            Add-DeskCheck $result 'legacy board migrated to 1280x1310 at (1280,0)' (
                $window.X -eq 1280 -and $window.Y -eq 0 -and $window.W -eq 1280 -and $window.H -eq 1310) (
                "$($window.X),$($window.Y) $($window.W)x$($window.H)")
            if ($ini) {
                Add-DeskCheck $result 'migrated desk.ini records standard panel' (
                    (Get-DeskIniInt $ini 'board.w') -eq 1280 -and (Get-DeskIniInt $ini 'board.h') -eq 1310 -and
                    (Get-DeskIniInt $ini 'board.x') -eq 1280 -and (Get-DeskIniInt $ini 'board.y') -eq 0) (
                    ("board.x={0} board.y={1} board.w={2} board.h={3}" -f (Get-DeskIniInt $ini 'board.x'), (Get-DeskIniInt $ini 'board.y'), (Get-DeskIniInt $ini 'board.w'), (Get-DeskIniInt $ini 'board.h')))
                Add-DeskCheck $result 'migration keeps card visibility/expansion' (
                    (Get-DeskIniInt $ini 'card.ps.expanded' 0) -eq 1 -and
                    (Get-DeskIniInt $ini 'card.account.visible' 1) -eq 1 -and
                    (Get-DeskIniInt $ini 'card.servers.visible' 1) -eq 1)
                $migratedInterior = Get-DeskInterior -BoardX 0 -BoardY 0 -BoardW 1280 -BoardH 1310
                Test-DeskCardGrid -Ini $ini -Interior $migratedInterior -Result $result -Prefix 'after-migration'
            }
        }
        'corrupt' {
            Add-DeskCheck $result 'corrupted config still starts a board' ($null -ne $window)
            if ($ini) {
                Add-DeskCheck $result 'corrupted config replaced by valid desk.ini' (
                    $ini.Contains('board.w') -and $ini.Contains('card.account.visible')) ("keys=$($ini.Count)")
            }
        }
        'out-of-bounds' {
            Add-DeskCheck $result 'out-of-bounds board brought into work area' (
                $window.X -ge $work.X -and $window.Y -ge $work.Y -and
                $window.Right -le $work.Right -and $window.Bottom -le $work.Bottom)
            if ($ini) {
                Add-DeskCheck $result 'card state (visible/expanded) preserved' (
                    (Get-DeskIniInt $ini 'card.ps.expanded' 0) -eq 1 -and
                    (Get-DeskIniInt $ini 'card.servers.expanded' 0) -eq 1 -and
                    (Get-DeskIniInt $ini 'card.account.visible' 1) -eq 1)
            }
        }
        'cannot-fit' {
            Add-DeskCheck $result 'board starts even when saved layout cannot fit' (
                (Test-TestProcessAlive -ProcessId $processId) -and $null -ne $window)
            # Growth-search cost (the height search strategy changed in the second
            # review round): launch -> first window -> settled layout.
            $result.Evidence.Add(("growth timing: window={0}ms settled={1}ms" -f $timeToWindowMs, $timeToSettledMs))
            Add-DeskCheck $result 'growth search starts the board promptly' (
                $timeToWindowMs -lt 5000) ("first window after {0} ms" -f $timeToWindowMs)
            Add-DeskCheck $result 'growth search settles the layout promptly' (
                $timeToSettledMs -lt 8000) ("settled desk.ini after {0} ms" -f $timeToSettledMs)
            # The saved 400x420 board cannot hold the 568 px terminal, so the
            # board is expected to grow minimally, INSIDE THE RIGHT HALF and
            # right-aligned (review fix: growth used to end up at x=0).
            $rightHalfLeft = $work.X + [int]($work.W / 2)
            $result.Evidence.Add(("grown board rect = {0},{1} {2}x{3} (work {4},{5} {6}x{7}, right-half from x={8})" -f $window.X, $window.Y, $window.W, $window.H, $work.X, $work.Y, $work.W, $work.H, $rightHalfLeft))
            Add-DeskCheck $result 'grown board stays inside the right half of the work area' (
                $window.X -ge $rightHalfLeft -and $window.W -le $work.W) ("board.left=$($window.X) >= $rightHalfLeft")
            Add-DeskCheck $result 'grown board is right-aligned to the work area' (
                ($window.X + $window.W) -eq $work.Right) ("board.right=$($window.Right) work.right=$($work.Right)")
            Add-DeskCheck $result 'saved-too-small board grew within the right-half cap' (
                $window.W -ge 400 -and $window.W -le 1280 -and $window.H -ge 420 -and
                ($window.W -gt 400 -or $window.H -gt 420)) ("$($window.W)x$($window.H)")
            # Minimal-growth search must not pick a near-corner size: the
            # previous coarse ladder produced 1280x484, so a larger area here
            # would be an over-grow regression.
            $grownArea = $window.W * $window.H
            Add-DeskCheck $result 'auto-growth stays minimal (no over-grow)' ($grownArea -le (1280 * 484)) (
                ("{0}x{1} = {2} px2 (previous coarse result 1280x484 = {3})" -f $window.W, $window.H, $grownArea, (1280 * 484)))
            # Offline brute force of the feasible 8 px candidates from the
            # measured module sizes (guillotine packing, see helper): prove no
            # much smaller board was skipped.
            if ($ini) {
                $cards = @(Get-DeskCardRects -Ini $ini | ForEach-Object {
                    [pscustomobject]@{ W = $_.Rect.W; H = $_.Rect.H } })
                if ($cards.Count -gt 0) {
                    $bestAll = Get-DeskMinimalBoardArea -Cards $cards -MaxWidth 1280 -MaxHeight $work.H
                    $bestNarrow = Get-DeskMinimalBoardArea -Cards $cards -MaxWidth $window.W -MaxHeight $work.H
                    if ($bestAll) {
                        $result.Evidence.Add(("offline minimal candidates: best={0}x{1} ({2} px2), best-not-wider={3}x{4} ({5} px2); chosen {6}x{7} ({8} px2)" -f $bestAll.W, $bestAll.H, $bestAll.Area, $bestNarrow.W, $bestNarrow.H, $bestNarrow.Area, $window.W, $window.H, $grownArea))
                        Add-DeskCheck $result 'no 8px candidate much smaller than the grown board was skipped' (
                            $grownArea -le ($bestAll.Area + $window.W * 8)) (
                            ("chosen {0} px2 vs best feasible {1} px2 (+{2})" -f $grownArea, $bestAll.Area, ($grownArea - $bestAll.Area)))
                        Add-DeskCheck $result 'grown board is within one grid step of the best candidate at its width' (
                            $grownArea -le ($bestNarrow.Area + $window.W * 8)) (
                            ("chosen {0}x{1} vs best<=w {2}x{3}" -f $window.W, $window.H, $bestNarrow.W, $bestNarrow.H))
                    }
                }
            }
            $laidOut = Test-CardsInsideInterior -Ini $ini -Interior $interior -Result $result -Prefix 'grown'
            Add-DeskCheck $result 'grown board holds every card with a 12px gap' $laidOut
            if ($ini) {
                Add-DeskCheck $result 'fallback keeps every card visible with its state' (
                    (Get-DeskIniInt $ini 'card.account.visible' 1) -eq 1 -and
                    (Get-DeskIniInt $ini 'card.servers.visible' 1) -eq 1 -and
                    (Get-DeskIniInt $ini 'card.ps.visible' 1) -eq 1 -and
                    (Get-DeskIniInt $ini 'card.ps.expanded' 0) -eq 1)
            }
            if ($wait.Dialogs -and $wait.Dialogs.Count -gt 0) {
                Add-DeskNote $result 'startup reported the layout problem with a dialog instead of exiting'
            }
        }
        'cannot-fit-huge' {
            Add-DeskCheck $result 'huge board clamped into work area' (
                $window.W -le $work.W -and $window.H -le $work.H -and
                $window.X -ge $work.X -and $window.Y -ge $work.Y -and
                $window.Right -le $work.Right -and $window.Bottom -le $work.Bottom) (
                "board $($window.X),$($window.Y) $($window.W)x$($window.H) vs work $($work.W)x$($work.H)")
        }

        'snapshot' {
            # Review fix (P1 #1) - cleared snapshot keys must not resurrect:
            # trigger a real user layout change (Reset), let the background save
            # run, restart, and verify saved.* is still absent.
            Start-Sleep -Milliseconds 1500
            $savedAtStartup = Read-DeskIniFile -Path $iniPath
            $savedBeforeReset = $false
            $savedBoardWBefore = -1
            if ($savedAtStartup) {
                $savedBeforeReset = ((Get-DeskIniInt $savedAtStartup 'saved.pending' 0) -eq 1)
                $savedBoardWBefore = Get-DeskIniInt $savedAtStartup 'saved.board.w' -1
            }
            $result.Evidence.Add(("startup keeps the non-fitting snapshot: pending=$savedBeforeReset saved.board.w=$savedBoardWBefore"))
            [void](Add-DeskCheck $result 'startup keeps a non-fitting saved snapshot pending' ($savedBeforeReset -and $savedBoardWBefore -eq 3000) ("pending=$savedBeforeReset saved.board.w=$savedBoardWBefore"))
            $resetInvoked = Invoke-DeskBoardMenuReset -ProcessId $processId -Window $window -Result $result
            [void](Add-DeskCheck $result 'user layout change (Reset) invoked' $resetInvoked)
            $afterReset = Wait-DeskSavedSnapshotState -Path $iniPath -Present $false -TimeoutSeconds 8
            $savedAfterReset = ($null -ne $afterReset) -and ($afterReset.Contains('saved.pending') -or $afterReset.Contains('saved.board.w'))
            [void](Add-DeskCheck $result 'Reset cleared saved.* in desk.ini' (-not $savedAfterReset) ("saved.* present: $savedAfterReset"))
            if ($afterReset) { Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-reset') -Force }
            [void](Wait-IniStable -Path $iniPath -TimeoutSeconds 5)

            $close = Close-TestBoard -ProcessId $processId -Window $window -Dialogs $wait.Dialogs -Result $result
            [void](Add-DeskCheck $result 'graceful close before restart' ($close.Graceful -or $close.LeftRunning))
            Start-Sleep -Milliseconds 700
            $processId = Start-TestBoard -ExePath (Join-Path $scenarioDir 'desk.exe') -WorkingDirectory $scenarioDir -Suffix $Definition.Suffix
            $wait = Wait-BoardWindow -ProcessId $processId -Suffix $Definition.Suffix -TimeoutSeconds $TimeoutSeconds
            $window = $wait.Window
            [void](Add-DeskCheck $result 'restart after clearing the snapshot starts' ((-not $wait.Exited) -and ($null -ne $window)))
            [void](Wait-IniStable -Path $iniPath -TimeoutSeconds 6)
            $afterRestart = Read-DeskIniFile -Path $iniPath
            if ($afterRestart) { Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-restart') -Force }
            $savedAfterRestart = ($null -ne $afterRestart) -and ($afterRestart.Contains('saved.pending') -or $afterRestart.Contains('saved.board.w'))
            [void](Add-DeskCheck $result 'saved.* stays gone after restart' (-not $savedAfterRestart) ("saved.* present: $savedAfterRestart"))
            if ($window) {
                [void](Add-DeskCheck $result 'restart keeps the standard panel' (
                    $window.X -eq 1280 -and $window.Y -eq 0 -and $window.W -eq 1280 -and $window.H -eq 1310) (
                    "$($window.X),$($window.Y) $($window.W)x$($window.H)"))
            }
        }
        'adapt' {
            # Review fix (P1 #3) - startup adaptation of an oversized saved board
            # must clamp the window AND snapshot the original rect under saved.*.
            $saved = Wait-DeskSavedSnapshotState -Path $iniPath -Present $true -TimeoutSeconds 8
            $pending = ($null -ne $saved) -and ((Get-DeskIniInt $saved 'saved.pending' 0) -eq 1)
            [void](Add-DeskCheck $result 'startup adaptation writes saved.pending=1' $pending)
            if ($saved) {
                Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-adapt') -Force
                $savedW = Get-DeskIniInt $saved 'saved.board.w' -1
                $savedH = Get-DeskIniInt $saved 'saved.board.h' -1
                $savedX = Get-DeskIniInt $saved 'saved.board.x' -1
                $savedY = Get-DeskIniInt $saved 'saved.board.y' -1
                $result.Evidence.Add(("adapted snapshot: saved.board={0},{1} {2}x{3}" -f $savedX, $savedY, $savedW, $savedH))
                [void](Add-DeskCheck $result 'snapshot keeps the original 3000x2000 board rect' (
                    $savedW -eq 3000 -and $savedH -eq 2000) ("saved.board.w=$savedW saved.board.h=$savedH"))
                [void](Add-DeskCheck $result 'snapshot keeps the original board origin' (
                    $savedX -eq 0 -and $savedY -eq 0) ("saved.board.x=$savedX saved.board.y=$savedY"))
            }
            [void](Add-DeskCheck $result 'adapted board window is inside the work area' (
                $window.X -ge $work.X -and $window.Y -ge $work.Y -and
                $window.Right -le $work.Right -and $window.Bottom -le $work.Bottom) (
                "board $($window.X),$($window.Y) $($window.W)x$($window.H)"))

            # A user layout change must clear the adaptation snapshot...
            $resetInvoked = Invoke-DeskBoardMenuReset -ProcessId $processId -Window $window -Result $result
            [void](Add-DeskCheck $result 'Reset invoked after adaptation' $resetInvoked)
            $afterReset = Wait-DeskSavedSnapshotState -Path $iniPath -Present $false -TimeoutSeconds 8
            $savedAfterReset = ($null -ne $afterReset) -and ($afterReset.Contains('saved.pending') -or $afterReset.Contains('saved.board.w'))
            [void](Add-DeskCheck $result 'Reset cleared the adaptation snapshot' (-not $savedAfterReset) ("saved.* present: $savedAfterReset"))
            if ($afterReset) {
                $adaptInterior = Get-DeskInterior -BoardX 0 -BoardY 0 -BoardW 1280 -BoardH 1310
                Test-DeskCardGrid -Ini $afterReset -Interior $adaptInterior -Result $result -Prefix 'after-adapt-reset'
            }
            [void](Wait-IniStable -Path $iniPath -TimeoutSeconds 5)

            # ...and the restart must keep it cleared.
            $close = Close-TestBoard -ProcessId $processId -Window $window -Dialogs $wait.Dialogs -Result $result
            [void](Add-DeskCheck $result 'graceful close before restart' ($close.Graceful -or $close.LeftRunning))
            Start-Sleep -Milliseconds 700
            $processId = Start-TestBoard -ExePath (Join-Path $scenarioDir 'desk.exe') -WorkingDirectory $scenarioDir -Suffix $Definition.Suffix
            $wait = Wait-BoardWindow -ProcessId $processId -Suffix $Definition.Suffix -TimeoutSeconds $TimeoutSeconds
            $window = $wait.Window
            [void](Add-DeskCheck $result 'restart after adaptation clear starts' ((-not $wait.Exited) -and ($null -ne $window)))
            [void](Wait-IniStable -Path $iniPath -TimeoutSeconds 6)
            $afterRestart = Read-DeskIniFile -Path $iniPath
            if ($afterRestart) { Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-restart') -Force }
            $savedAfterRestart = ($null -ne $afterRestart) -and ($afterRestart.Contains('saved.pending') -or $afterRestart.Contains('saved.board.w'))
            [void](Add-DeskCheck $result 'adaptation saved.* stays gone after restart' (-not $savedAfterRestart) ("saved.* present: $savedAfterRestart"))
            if ($window) {
                [void](Add-DeskCheck $result 'restart after adaptation keeps the standard panel' (
                    $window.X -eq 1280 -and $window.Y -eq 0 -and $window.W -eq 1280 -and $window.H -eq 1310) (
                    "$($window.X),$($window.Y) $($window.W)x$($window.H)"))
            }
        }
        'restore-strict' {
            # Review fix (P1 #2) - a saved snapshot that cannot fully fit the
            # current work area must NOT be cleared by the failed restore.
            Start-Sleep -Milliseconds 2000
            $saved = Read-DeskIniFile -Path $iniPath
            $pending = ($null -ne $saved) -and ((Get-DeskIniInt $saved 'saved.pending' 0) -eq 1)
            [void](Add-DeskCheck $result 'unrestorable snapshot is not cleared (saved.pending=1)' $pending)
            if ($saved) {
                Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-restore-attempt') -Force
                $savedW = Get-DeskIniInt $saved 'saved.board.w' -1
                $savedH = Get-DeskIniInt $saved 'saved.board.h' -1
                $result.Evidence.Add(("restore-strict snapshot kept: saved.board.w={0} saved.board.h={1}" -f $savedW, $savedH))
                [void](Add-DeskCheck $result 'unrestorable saved board rect is preserved' (
                    $savedW -eq 3000 -and $savedH -eq 2000) ("saved.board.w=$savedW saved.board.h=$savedH"))
                [void](Add-DeskCheck $result 'unrestorable saved card positions are preserved' (
                    $saved.Contains('saved.card.ps.x') -and $saved.Contains('saved.card.account.x')))
            }
            $strictBoardW = Get-DeskIniInt $ini 'board.w' 0
            $strictBoardH = Get-DeskIniInt $ini 'board.h' 0
            $strictInterior = Get-DeskInterior -BoardX 0 -BoardY 0 -BoardW $strictBoardW -BoardH $strictBoardH
            $strictLegal = $true
            foreach ($entry in (Get-DeskCardRects -Ini $ini)) {
                $rect = $entry.Rect
                $inside = ($rect.X -ge $strictInterior.X -and $rect.Y -ge $strictInterior.Y -and
                           $rect.Right -le ($strictInterior.X + $strictInterior.W) -and
                           $rect.Bottom -le ($strictInterior.Y + $strictInterior.H))
                if (-not $inside) { $strictLegal = $false }
            }
            [void](Add-DeskCheck $result 'layout stays legal while a restore is pending' $strictLegal)
            Test-DeskCardGrid -Ini $ini -Interior $strictInterior -Result $result -Prefix 'restore-pending'
            [void](Add-DeskCheck $result 'board window inside the work area while a restore is pending' (
                $window.X -ge $work.X -and $window.Y -ge $work.Y -and
                $window.Right -le $work.Right -and $window.Bottom -le $work.Bottom) (
                "board $($window.X),$($window.Y) $($window.W)x$($window.H)"))
        }
        'consecutive-adapt' {
            # Second review round - consecutive simulated work-area changes.
            $displayPath = Join-Path $scenarioDir 'display.txt'
            $origin = [ordered]@{}
            foreach ($key in @('card.account.x', 'card.account.y', 'card.servers.x', 'card.servers.y', 'card.ps.x', 'card.ps.y')) {
                $origin[$key] = Get-DeskIniInt $ini $key -9999
            }
            $originalSavedOk = {
                param($values)
                (Get-DeskIniInt $values 'saved.board.x' -1) -eq 1280 -and
                (Get-DeskIniInt $values 'saved.board.y' -1) -eq 0 -and
                (Get-DeskIniInt $values 'saved.board.w' -1) -eq 1280 -and
                (Get-DeskIniInt $values 'saved.board.h' -1) -eq 1310
            }

            # --- adaptation 1: 2032x972 -------------------------------------
            Write-DeskDisplayFile -Path $displayPath -Work '0,0,2032,972' -Dpi 96
            [void](Send-DeskDisplayMessage -Hwnd $window.Hwnd -Message 0x8009)
            $iniA1 = Wait-DeskIniCondition -Path $iniPath -Predicate {
                param($v) (Get-DeskIniInt $v 'saved.pending' 0) -eq 1 -and (Get-DeskIniInt $v 'board.w' -1) -le 1100
            } -TimeoutSeconds 8
            if ($iniA1) { Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-adapt1') -Force }
            $pending1 = ($null -ne $iniA1) -and ((Get-DeskIniInt $iniA1 'saved.pending' 0) -eq 1)
            [void](Add-DeskCheck $result 'adaptation 1 writes saved.pending=1' $pending1)
            $savedOk1 = ($null -ne $iniA1) -and (& $originalSavedOk $iniA1)
            [void](Add-DeskCheck $result 'adaptation 1 snapshots the ORIGINAL 1280x1310 @ (1280,0)' $savedOk1 (
                "saved.board=" + $(if ($iniA1) { "{0},{1} {2}x{3}" -f (Get-DeskIniInt $iniA1 'saved.board.x'), (Get-DeskIniInt $iniA1 'saved.board.y'), (Get-DeskIniInt $iniA1 'saved.board.w'), (Get-DeskIniInt $iniA1 'saved.board.h') } else { 'missing' })))
            $windowA1 = Wait-BoardWindowRect -ProcessId $processId -Suffix $Definition.Suffix -Predicate {
                param($w) $w.X -ge 0 -and $w.Y -ge 0 -and $w.Right -le 2032 -and $w.Bottom -le 972
            } -TimeoutSeconds 6
            [void](Add-DeskCheck $result 'board adapted inside the simulated 2032x972 work area' (
                $windowA1 -and $windowA1.X -ge 0 -and $windowA1.Y -ge 0 -and $windowA1.Right -le 2032 -and $windowA1.Bottom -le 972) (
                $(if ($windowA1) { "rect=$($windowA1.X),$($windowA1.Y) $($windowA1.W)x$($windowA1.H)" } else { 'window gone' })))
            if ($iniA1) {
                $a1W = Get-DeskIniInt $iniA1 'board.w' 0
                $a1H = Get-DeskIniInt $iniA1 'board.h' 0
                $a1Interior = Get-DeskInterior -BoardX 0 -BoardY 0 -BoardW $a1W -BoardH $a1H
                [void](Test-CardsInsideInterior -Ini $iniA1 -Interior $a1Interior -Result $result -Prefix 'adapt1')
            }

            # --- adaptation 2: 1600x800 (must keep the ORIGINAL snapshot) ----
            Write-DeskDisplayFile -Path $displayPath -Work '0,0,1600,800' -Dpi 96
            [void](Send-DeskDisplayMessage -Hwnd $window.Hwnd -Message 0x8009)
            $iniA2 = Wait-DeskIniCondition -Path $iniPath -Predicate {
                param($v) (Get-DeskIniInt $v 'saved.board.w' -1) -eq 1280 -and (Get-DeskIniInt $v 'board.w' -1) -le 900
            } -TimeoutSeconds 8
            if ($iniA2) { Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-adapt2') -Force }
            $savedOk2 = ($null -ne $iniA2) -and (& $originalSavedOk $iniA2)
            [void](Add-DeskCheck $result 'adaptation 2 keeps the ORIGINAL snapshot (not the first adapted rect)' $savedOk2 (
                "saved.board=" + $(if ($iniA2) { "{0},{1} {2}x{3}" -f (Get-DeskIniInt $iniA2 'saved.board.x'), (Get-DeskIniInt $iniA2 'saved.board.y'), (Get-DeskIniInt $iniA2 'saved.board.w'), (Get-DeskIniInt $iniA2 'saved.board.h') } else { 'missing' })))
            $windowA2 = Wait-BoardWindowRect -ProcessId $processId -Suffix $Definition.Suffix -Predicate {
                param($w) $w.X -ge 0 -and $w.Y -ge 0 -and $w.Right -le 1600 -and $w.Bottom -le 800
            } -TimeoutSeconds 6
            [void](Add-DeskCheck $result 'board adapted inside the simulated 1600x800 work area' (
                $windowA2 -and $windowA2.X -ge 0 -and $windowA2.Y -ge 0 -and $windowA2.Right -le 1600 -and $windowA2.Bottom -le 800) (
                $(if ($windowA2) { "rect=$($windowA2.X),$($windowA2.Y) $($windowA2.W)x$($windowA2.H)" } else { 'window gone' })))

            # --- back to the original work area: restore --------------------
            Write-DeskDisplayFile -Path $displayPath -Work '0,0,2560,1392' -Dpi 96
            [void](Send-DeskDisplayMessage -Hwnd $window.Hwnd -Message 0x8009)
            $iniBack = Wait-DeskIniCondition -Path $iniPath -Predicate {
                param($v) -not ($v.Contains('saved.pending') -or $v.Contains('saved.board.w'))
            } -TimeoutSeconds 8
            if ($iniBack) { Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-restore') -Force }
            $savedCleared = ($null -ne $iniBack) -and -not ($iniBack.Contains('saved.pending') -or $iniBack.Contains('saved.board.w'))
            [void](Add-DeskCheck $result 'restore clears saved.* when the original work area is back' $savedCleared)
            $windowBack = Wait-BoardWindowRect -ProcessId $processId -Suffix $Definition.Suffix -Predicate {
                param($w) $w.X -eq 1280 -and $w.Y -eq 0 -and $w.W -eq 1280 -and $w.H -eq 1310
            } -TimeoutSeconds 6
            [void](Add-DeskCheck $result 'restore returns the board to 1280x1310 @ (1280,0)' (
                $windowBack -and $windowBack.X -eq 1280 -and $windowBack.Y -eq 0 -and $windowBack.W -eq 1280 -and $windowBack.H -eq 1310) (
                $(if ($windowBack) { "rect=$($windowBack.X),$($windowBack.Y) $($windowBack.W)x$($windowBack.H)" } else { 'window gone' })))
            $cardsRestored = $true
            $cardDetail = @()
            foreach ($key in $origin.Keys) {
                $now = Get-DeskIniInt $iniBack $key -9999
                if ($now -ne $origin[$key]) { $cardsRestored = $false }
                $cardDetail += ("{0}={1} (want {2})" -f $key, $now, $origin[$key])
            }
            [void](Add-DeskCheck $result 'restore returns every card to its original position' $cardsRestored ($cardDetail -join ', '))
            [void](Add-DeskCheck $result 'board still running after two adaptations and a restore' (Test-TestProcessAlive -ProcessId $processId))
        }
        'dpi' {
            # Second review round - simulated DPI change round trip.
            $displayPath = Join-Path $scenarioDir 'display.txt'
            $origin = [ordered]@{}
            foreach ($key in @('card.account.x', 'card.account.y', 'card.servers.x', 'card.servers.y', 'card.ps.x', 'card.ps.y')) {
                $origin[$key] = Get-DeskIniInt $ini $key -9999
            }

            # --- DPI 96 -> 144 with a suggested 0,0 1920x1310 rect -----------
            Write-DeskDisplayFile -Path $displayPath -Work '0,0,2560,1392' -Dpi 144 -Rect '0,0,1920,1310'
            [void](Send-DeskDisplayMessage -Hwnd $window.Hwnd -Message 0x800A)
            $iniDpi = Wait-DeskIniCondition -Path $iniPath -Predicate {
                param($v) (Get-DeskIniInt $v 'saved.pending' 0) -eq 1 -and (Get-DeskIniInt $v 'board.w' -1) -ne 1280
            } -TimeoutSeconds 8
            if ($iniDpi) { Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-dpi144') -Force }
            $pendingDpi = ($null -ne $iniDpi) -and ((Get-DeskIniInt $iniDpi 'saved.pending' 0) -eq 1)
            [void](Add-DeskCheck $result 'DPI 144 adaptation writes saved.pending=1' $pendingDpi)
            $savedDpiOk = ($null -ne $iniDpi) -and
                ((Get-DeskIniInt $iniDpi 'saved.board.x' -1) -eq 1280) -and
                ((Get-DeskIniInt $iniDpi 'saved.board.y' -1) -eq 0) -and
                ((Get-DeskIniInt $iniDpi 'saved.board.w' -1) -eq 1280) -and
                ((Get-DeskIniInt $iniDpi 'saved.board.h' -1) -eq 1310)
            [void](Add-DeskCheck $result 'DPI 144 snapshots the ORIGINAL 1280x1310 @ (1280,0)' $savedDpiOk (
                "saved.board=" + $(if ($iniDpi) { "{0},{1} {2}x{3}" -f (Get-DeskIniInt $iniDpi 'saved.board.x'), (Get-DeskIniInt $iniDpi 'saved.board.y'), (Get-DeskIniInt $iniDpi 'saved.board.w'), (Get-DeskIniInt $iniDpi 'saved.board.h') } else { 'missing' })))
            $windowDpi = Wait-BoardWindowRect -ProcessId $processId -Suffix $Definition.Suffix -Predicate {
                param($w) $w.X -ne 1280 -or $w.Y -ne 0 -or $w.W -ne 1280 -or $w.H -ne 1310
            } -TimeoutSeconds 6
            $dpiAdapted = $windowDpi -and ($windowDpi.X -ne 1280 -or $windowDpi.Y -ne 0 -or $windowDpi.W -ne 1280 -or $windowDpi.H -ne 1310)
            [void](Add-DeskCheck $result 'DPI 144 adaptation changed the board window' $dpiAdapted (
                $(if ($windowDpi) { "rect=$($windowDpi.X),$($windowDpi.Y) $($windowDpi.W)x$($windowDpi.H)" } else { 'window gone' })))
            [void](Add-DeskCheck $result 'DPI 144 board stays inside the work area' (
                $windowDpi -and $windowDpi.X -ge 0 -and $windowDpi.Y -ge 0 -and $windowDpi.Right -le $work.Right -and $windowDpi.Bottom -le $work.Bottom) (
                $(if ($windowDpi) { "rect=$($windowDpi.X),$($windowDpi.Y) $($windowDpi.W)x$($windowDpi.H)" } else { 'window gone' })))

            # --- back to DPI 96 with the original suggested rect -------------
            Write-DeskDisplayFile -Path $displayPath -Work '0,0,2560,1392' -Dpi 96 -Rect '1280,0,1280,1310'
            [void](Send-DeskDisplayMessage -Hwnd $window.Hwnd -Message 0x800A)
            $iniDpiBack = Wait-DeskIniCondition -Path $iniPath -Predicate {
                param($v) -not ($v.Contains('saved.pending') -or $v.Contains('saved.board.w'))
            } -TimeoutSeconds 8
            if ($iniDpiBack) { Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-dpi96') -Force }
            $dpiSavedCleared = ($null -ne $iniDpiBack) -and -not ($iniDpiBack.Contains('saved.pending') -or $iniDpiBack.Contains('saved.board.w'))
            [void](Add-DeskCheck $result 'DPI round trip clears saved.* at 96 DPI' $dpiSavedCleared)
            $windowBack = Wait-BoardWindowRect -ProcessId $processId -Suffix $Definition.Suffix -Predicate {
                param($w) $w.X -eq 1280 -and $w.Y -eq 0 -and $w.W -eq 1280 -and $w.H -eq 1310
            } -TimeoutSeconds 6
            [void](Add-DeskCheck $result 'DPI round trip restores 1280x1310 @ (1280,0)' (
                $windowBack -and $windowBack.X -eq 1280 -and $windowBack.Y -eq 0 -and $windowBack.W -eq 1280 -and $windowBack.H -eq 1310) (
                $(if ($windowBack) { "rect=$($windowBack.X),$($windowBack.Y) $($windowBack.W)x$($windowBack.H)" } else { 'window gone' })))
            $cardsRestored = $true
            $cardDetail = @()
            foreach ($key in $origin.Keys) {
                $now = Get-DeskIniInt $iniDpiBack $key -9999
                if ($now -ne $origin[$key]) { $cardsRestored = $false }
                $cardDetail += ("{0}={1} (want {2})" -f $key, $now, $origin[$key])
            }
            [void](Add-DeskCheck $result 'DPI round trip returns every card to its original position' $cardsRestored ($cardDetail -join ', '))
            [void](Add-DeskCheck $result 'board still running after the DPI round trip' (Test-TestProcessAlive -ProcessId $processId))
        }
        'display-shim' {
            # Compatibility-shim routing for the interface batch: the env-file +
            # message combination must still drive the (new) display source.
            $displayPath = Join-Path $scenarioDir 'display.txt'
            $originalSavedOk = {
                param($values)
                (Get-DeskIniInt $values 'saved.board.x' -1) -eq 1280 -and
                (Get-DeskIniInt $values 'saved.board.y' -1) -eq 0 -and
                (Get-DeskIniInt $values 'saved.board.w' -1) -eq 1280 -and
                (Get-DeskIniInt $values 'saved.board.h' -1) -eq 1310
            }

            # (a) env file + WM_APP+9 must adapt through the display source.
            Write-DeskDisplayFile -Path $displayPath -Work '0,0,2032,972' -Dpi 96
            [void](Send-DeskDisplayMessage -Hwnd $window.Hwnd -Message 0x8009)
            $iniShim = Wait-DeskIniCondition -Path $iniPath -Predicate {
                param($v) (Get-DeskIniInt $v 'saved.pending' 0) -eq 1 -and (Get-DeskIniInt $v 'board.w' -1) -le 1100
            } -TimeoutSeconds 8
            if ($iniShim) { Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-shim-adapt') -Force }
            $shimAdapted = ($null -ne $iniShim) -and ((Get-DeskIniInt $iniShim 'saved.pending' 0) -eq 1)
            [void](Add-DeskCheck $result 'display shim: env file + WM_APP+9 adapts through the display source' $shimAdapted)
            $shimSnapshot = ($null -ne $iniShim) -and (& $originalSavedOk $iniShim)
            $shimSnapshotDetail = 'missing saved.board'
            if ($iniShim) { $shimSnapshotDetail = 'saved.board={0},{1} {2}x{3}' -f (Get-DeskIniInt $iniShim 'saved.board.x'), (Get-DeskIniInt $iniShim 'saved.board.y'), (Get-DeskIniInt $iniShim 'saved.board.w'), (Get-DeskIniInt $iniShim 'saved.board.h') }
            [void](Add-DeskCheck $result 'display shim: adaptation snapshots the original layout' $shimSnapshot $shimSnapshotDetail)
            $windowShim = Wait-BoardWindowRect -ProcessId $processId -Suffix $Definition.Suffix -Predicate {
                param($w) $w.X -ge 0 -and $w.Y -ge 0 -and $w.Right -le 2032 -and $w.Bottom -le 972
            } -TimeoutSeconds 6
            $windowShimDetail = 'window gone'
            if ($windowShim) { $windowShimDetail = 'rect={0},{1} {2}x{3}' -f $windowShim.X, $windowShim.Y, $windowShim.W, $windowShim.H }
            [void](Add-DeskCheck $result 'display shim: adapted board sits inside the simulated 2032x972 work area' ($null -ne $windowShim) $windowShimDetail)

            # (b) a malformed display file must be ignored safely.
            $badText = 'work=not-a-rect' + [string][char]10 + 'dpi=oops' + [string][char]10
            [System.IO.File]::WriteAllText($displayPath, $badText, (New-Object System.Text.UTF8Encoding($false)))
            [void](Send-DeskDisplayMessage -Hwnd $window.Hwnd -Message 0x8009)
            Start-Sleep -Milliseconds 800
            [void](Add-DeskCheck $result 'display shim: malformed file does not crash the board' (Test-TestProcessAlive -ProcessId $processId))
            $iniBad = Read-DeskIniFile -Path $iniPath
            $badSnapshotOk = ($null -ne $iniBad) -and ((Get-DeskIniInt $iniBad 'saved.pending' 0) -eq 1) -and (& $originalSavedOk $iniBad)
            $badDetail = 'missing desk.ini'
            if ($iniBad) { $badDetail = 'saved.board={0},{1} {2}x{3}' -f (Get-DeskIniInt $iniBad 'saved.board.x'), (Get-DeskIniInt $iniBad 'saved.board.y'), (Get-DeskIniInt $iniBad 'saved.board.w'), (Get-DeskIniInt $iniBad 'saved.board.h') }
            [void](Add-DeskCheck $result 'display shim: malformed file keeps the original snapshot' $badSnapshotOk $badDetail)

            # (c) restoring through the shim.
            Write-DeskDisplayFile -Path $displayPath -Work '0,0,2560,1392' -Dpi 96
            [void](Send-DeskDisplayMessage -Hwnd $window.Hwnd -Message 0x8009)
            $iniShimBack = Wait-DeskIniCondition -Path $iniPath -Predicate {
                param($v) -not ($v.Contains('saved.pending') -or $v.Contains('saved.board.w'))
            } -TimeoutSeconds 8
            if ($iniShimBack) { Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-shim-restore') -Force }
            $windowShimBack = Wait-BoardWindowRect -ProcessId $processId -Suffix $Definition.Suffix -Predicate {
                param($w) $w.X -eq 1280 -and $w.Y -eq 0 -and $w.W -eq 1280 -and $w.H -eq 1310
            } -TimeoutSeconds 6
            $shimRestored = ($null -ne $windowShimBack) -and ($windowShimBack.W -eq 1280) -and
                ($null -ne $iniShimBack) -and -not ($iniShimBack.Contains('saved.pending'))
            $windowBackDetail = 'window gone'
            if ($windowShimBack) { $windowBackDetail = 'rect={0},{1} {2}x{3}' -f $windowShimBack.X, $windowShimBack.Y, $windowShimBack.W, $windowShimBack.H }
            [void](Add-DeskCheck $result 'display shim: restore clears saved.* and returns the standard panel' $shimRestored $windowBackDetail)

            # (d) without the env var the messages must be inert.
            $close = Close-TestBoard -ProcessId $processId -Window $window -Dialogs $wait.Dialogs -Result $result
            [void](Add-DeskCheck $result 'display shim: graceful close before the inert-message check' ($close.Graceful -or $close.LeftRunning))
            Start-Sleep -Milliseconds 600
            $processId = Start-TestBoard -ExePath (Join-Path $scenarioDir 'desk.exe') -WorkingDirectory $scenarioDir -Suffix $Definition.Suffix -DisplayFile ''
            $wait = Wait-BoardWindow -ProcessId $processId -Suffix $Definition.Suffix -TimeoutSeconds $TimeoutSeconds
            $window = $wait.Window
            [void](Add-DeskCheck $result 'display shim: restart without DESK_TEST_DISPLAY_FILE starts' ((-not $wait.Exited) -and ($null -ne $window)))
            [void](Wait-IniStable -Path $iniPath -TimeoutSeconds 6)
            Start-Sleep -Milliseconds 500
            if ($window) { [void](Send-DeskDisplayMessage -Hwnd $window.Hwnd -Message 0x8009) }
            Start-Sleep -Milliseconds 800
            $iniNoFile = Read-DeskIniFile -Path $iniPath
            $windowNoFile = Get-BoardWindow -ProcessId $processId -Suffix $Definition.Suffix
            $inertOk = ($null -ne $windowNoFile) -and ($windowNoFile.X -eq 1280) -and ($windowNoFile.Y -eq 0) -and
                ($windowNoFile.W -eq 1280) -and ($windowNoFile.H -eq 1310) -and
                ($null -ne $iniNoFile) -and -not ($iniNoFile.Contains('saved.board.w'))
            $noFileDetail = 'window gone'
            if ($windowNoFile) { $noFileDetail = 'rect={0},{1} {2}x{3}' -f $windowNoFile.X, $windowNoFile.Y, $windowNoFile.W, $windowNoFile.H }
            [void](Add-DeskCheck $result 'display shim: WM_APP+9 is inert without DESK_TEST_DISPLAY_FILE' $inertOk $noFileDetail)
            [void](Add-DeskCheck $result 'display shim: board still running after the inert message' (Test-TestProcessAlive -ProcessId $processId))
        }
        'reset' {
            # Open the board menu (right-click on the board title strip) and
            # choose "Reset to right standard panel" with the keyboard:
            # Home selects the first item, 4 x Down lands on item 5 (Add module,
            # Tidy board, Refresh all, Fit cards, Reset...), Enter commits.
            [void][DeskTestNative]::PostMessage($window.Hwnd, 0x0205, [IntPtr]::Zero, [DeskTestNative]::MakeLParam(54, 10))
            $menu = $null
            $menuDeadline = (Get-Date).AddSeconds(3)
            while ((Get-Date) -lt $menuDeadline -and -not $menu) {
                $menuWindows = @(Get-DeskWindowList -ProcessId $processId | Where-Object { $_.Class -eq '#32768' })
                if ($menuWindows.Count -gt 0) { $menu = $menuWindows[0] } else { Start-Sleep -Milliseconds 100 }
            }
            [void](Add-DeskCheck $result 'board menu opens' ($null -ne $menu) ("menu window: $(if ($menu) { $menu.Hwnd } else { 'not found' })"))
            # Win32 popup menus do not take posted WM_KEYDOWN reliably; drive the
            # menu item through UI Automation instead, which is how accessibility
            # tools activate menus. Falls back to keyboard navigation if UIA is
            # unavailable.
            $invoked = $false
            if ($menu) {
                try {
                    Add-Type -AssemblyName Accessibility -ErrorAction SilentlyContinue
                    $iidAccessible = [Guid]'618736E0-3C3D-11CF-810C-00AA00389B71'
                    $accessible = $null
                    $objIdClient = [uint32]4294967292
                    $hr = [MsaaNative]::AccessibleObjectFromWindow($menu.Hwnd, $objIdClient, [ref]$iidAccessible, [ref]$accessible)
                    if ($hr -eq 0 -and $accessible) {
                        $childCount = $accessible.accChildCount
                        for ($child = 1; $child -le $childCount; $child++) {
                            $name = ''
                            try { $name = [string]$accessible.accName($child) } catch { continue }
                            if ($name -notlike 'Reset to right standard panel*') { continue }
                            $accessible.accDoDefaultAction($child)
                            $invoked = $true
                            break
                        }
                    }
                } catch {
                    Add-DeskNote $result ("MSAA menu activation failed: " + $_.Exception.Message)
                }
                if (-not $invoked) {
                    [void][DeskTestNative]::PostMessage($menu.Hwnd, 0x0100, [IntPtr]0x24, [IntPtr]1)   # VK_HOME
                    Start-Sleep -Milliseconds 150
                    for ($step = 0; $step -lt 4; $step++) {
                        [void][DeskTestNative]::PostMessage($menu.Hwnd, 0x0100, [IntPtr]0x28, [IntPtr]1)   # VK_DOWN
                        Start-Sleep -Milliseconds 110
                    }
                    [void][DeskTestNative]::PostMessage($menu.Hwnd, 0x0100, [IntPtr]0x0D, [IntPtr]1)   # VK_RETURN
                    Start-Sleep -Milliseconds 400
                }
            }
            $result.Evidence.Add("reset menu item invoked via UIAutomation: $invoked")
            Start-Sleep -Milliseconds 1200
            $resetWindow = $null
            $deadline = (Get-Date).AddSeconds(4)
            while ((Get-Date) -lt $deadline -and -not $resetWindow) {
                $resetWindow = @(Get-DeskWindowList -ProcessId $processId | Where-Object { $_.Visible -and ($_.Class -eq "wg.desk-$($Definition.Suffix)" -or $_.Class -eq 'wg.desk') }) | Select-Object -First 1
                if (-not $resetWindow) { Start-Sleep -Milliseconds 150 }
            }
            [void](Wait-IniStable -Path $iniPath -TimeoutSeconds 5)
            $iniReset = Read-DeskIniFile -Path $iniPath
            if ($iniReset) { Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-reset') -Force }
            $backupsReset = @(Get-BackupFiles -Directory $scenarioDir)
            $newResetBackups = @($backupsReset | Where-Object { $backupsBefore -notcontains $_ })
            [void](Add-DeskCheck $result 'Reset created a config backup' ($newResetBackups.Count -ge 1) ("backups: $($newResetBackups -join ', ')"))
            Add-DeskCheck $result 'Reset moved the board to the standard panel' (
                $resetWindow -and $resetWindow.X -eq 1280 -and $resetWindow.Y -eq 0 -and
                $resetWindow.W -eq 1280 -and $resetWindow.H -eq 1310) ("rect=" + $(if ($resetWindow) { "$($resetWindow.X),$($resetWindow.Y) $($resetWindow.W)x$($resetWindow.H)" } else { 'window gone' }))
            if ($iniReset) {
                Add-DeskCheck $result 'Reset persisted the standard panel in desk.ini' (
                    (Get-DeskIniInt $iniReset 'board.w') -eq 1280 -and (Get-DeskIniInt $iniReset 'board.h') -eq 1310 -and
                    (Get-DeskIniInt $iniReset 'board.x') -eq 1280 -and (Get-DeskIniInt $iniReset 'board.y') -eq 0) (
                    ("board.x={0} board.y={1} board.w={2} board.h={3}" -f (Get-DeskIniInt $iniReset 'board.x'), (Get-DeskIniInt $iniReset 'board.y'), (Get-DeskIniInt $iniReset 'board.w'), (Get-DeskIniInt $iniReset 'board.h')))
                $resetInterior = Get-DeskInterior -BoardX 0 -BoardY 0 -BoardW 1280 -BoardH 1310
                [void](Test-CardsInsideInterior -Ini $iniReset -Interior $resetInterior -Result $result -Prefix 'after-reset')
                Test-DeskCardGrid -Ini $iniReset -Interior $resetInterior -Result $result -Prefix 'after-reset'
            }
            Add-DeskCheck $result 'board still running after Reset' (Test-TestProcessAlive -ProcessId $processId)
            if ($resetWindow) {
                $shotReset = Save-DeskWindowShot -Hwnd $resetWindow.Hwnd -X $resetWindow.X -Y $resetWindow.Y -W $resetWindow.W -H $resetWindow.H -Path (Join-Path $evidenceDir 'board-after-reset.png')
                $result.Evidence.Add("after-reset screenshot=$($shotReset.Path)")
                $window = $resetWindow
            }
        }
        'drag-cancel' {
            # Start a card drag, move far, then interrupt it the way Alt+Tab
            # would (WM_CAPTURECHANGED): the card must return to its pre-drag
            # position and the saved layout must keep it there.
            $beforeX = Get-DeskIniInt $ini 'card.ps.x' 0
            $beforeY = Get-DeskIniInt $ini 'card.ps.y' 0
            $dragX = $beforeX + 40
            $dragY = $beforeY + 15
            [void][DeskTestNative]::PostMessage($window.Hwnd, 0x0201, [IntPtr]1, [DeskTestNative]::MakeLParam($dragX, $dragY))
            for ($step = 1; $step -le 20; $step++) {
                [void][DeskTestNative]::PostMessage($window.Hwnd, 0x0200, [IntPtr]1, [DeskTestNative]::MakeLParam(($dragX + $step * 30), ($dragY + $step * 20)))
                Start-Sleep -Milliseconds 16
            }
            [void][DeskTestNative]::PostMessage($window.Hwnd, 0x0018, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CAPTURECHANGED
            Start-Sleep -Milliseconds 900
            [void](Wait-IniStable -Path $iniPath -TimeoutSeconds 5)
            $iniCancel = Read-DeskIniFile -Path $iniPath
            if ($iniCancel) { Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-cancel') -Force }
            $cancelled = $false
            if ($iniCancel) {
                $cancelled = ((Get-DeskIniInt $iniCancel 'card.ps.x' -9999) -eq $beforeX) -and
                             ((Get-DeskIniInt $iniCancel 'card.ps.y' -9999) -eq $beforeY)
                $result.Evidence.Add(("cancelled drag kept card.ps at ({0},{1}) (was ({2},{3}))" -f (Get-DeskIniInt $iniCancel 'card.ps.x'), (Get-DeskIniInt $iniCancel 'card.ps.y'), $beforeX, $beforeY))
            }
            Add-DeskCheck $result 'interrupted drag restores the pre-drag position' $cancelled
            Add-DeskCheck $result 'board still running after interrupted drag' (Test-TestProcessAlive -ProcessId $processId)
            $shotCancel = Save-DeskWindowShot -Hwnd $window.Hwnd -X $window.X -Y $window.Y -W $window.W -H $window.H -Path (Join-Path $evidenceDir 'board-after-cancel.png')
            $result.Evidence.Add("after-cancel screenshot=$($shotCancel.Path)")
        }
        'drag' {
            # Read the pre-drag card positions, drag the PS card far outside the
            # board, release, then verify the saved layout is legal again.
            $dragX = (Get-DeskIniInt $ini 'card.ps.x' 0) + 40
            $dragY = (Get-DeskIniInt $ini 'card.ps.y' 0) + 15
            $beforeX = Get-DeskIniInt $ini 'card.ps.x' 0
            $beforeY = Get-DeskIniInt $ini 'card.ps.y' 0
            [void](Send-DeskDrag -Hwnd $window.Hwnd -X0 $dragX -Y0 $dragY -X1 2100 -Y1 1500 -Steps 40)
            Start-Sleep -Milliseconds 900
            [void](Wait-IniStable -Path $iniPath -TimeoutSeconds 5)
            $iniAfter = Read-DeskIniFile -Path $iniPath
            if ($iniAfter) {
                Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-drag') -Force
                $result.Evidence.Add(("drag target released at 2100,1500; card.ps moved from ({0},{1}) to ({2},{3})" -f $beforeX, $beforeY, (Get-DeskIniInt $iniAfter 'card.ps.x'), (Get-DeskIniInt $iniAfter 'card.ps.y')))
                $afterInterior = Get-DeskInterior -BoardX 0 -BoardY 0 -BoardW $boardW -BoardH $boardH
                [void](Test-CardsInsideInterior -Ini $iniAfter -Interior $afterInterior -Result $result -Prefix 'after-drag')
                Test-DeskCardGrid -Ini $iniAfter -Interior $afterInterior -Result $result -Prefix 'after-drag'
                Add-DeskCheck $result 'dragged card stays inside the board' $true ("card.ps=({0},{1})" -f (Get-DeskIniInt $iniAfter 'card.ps.x'), (Get-DeskIniInt $iniAfter 'card.ps.y'))
                Add-DeskCheck $result 'drag did not lose card state' (
                    (Get-DeskIniInt $iniAfter 'card.ps.visible' 1) -eq 1 -and
                    (Get-DeskIniInt $iniAfter 'card.ps.expanded' 0) -eq 0)
            }
            Add-DeskCheck $result 'board still running after drag release' (Test-TestProcessAlive -ProcessId $processId)
            $shotAfter = Save-DeskWindowShot -Hwnd $window.Hwnd -X $window.X -Y $window.Y -W $window.W -H $window.H -Path (Join-Path $evidenceDir 'board-after-drag.png')
            $result.Evidence.Add("after-drag screenshot=$($shotAfter.Path)")
        }
        'drag-noslot' {
            # Small saved board + tight grown layout: drag the account card and
            # release it over the terminal. The saved layout after the release
            # must still be legal (inside the interior, 12 px gaps).
            $dragX = (Get-DeskIniInt $ini 'card.account.x' 0) + 40
            $dragY = (Get-DeskIniInt $ini 'card.account.y' 0) + 15
            $beforeX = Get-DeskIniInt $ini 'card.account.x' 0
            $beforeY = Get-DeskIniInt $ini 'card.account.y' 0
            [void](Send-DeskDrag -Hwnd $window.Hwnd -X0 $dragX -Y0 $dragY -X1 260 -Y1 260 -Steps 25)
            Start-Sleep -Milliseconds 900
            [void](Wait-IniStable -Path $iniPath -TimeoutSeconds 5)
            $iniAfter = Read-DeskIniFile -Path $iniPath
            if ($iniAfter) {
                Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-drag') -Force
                $result.Evidence.Add(("small-board drag: account ({0},{1}) -> ({2},{3})" -f $beforeX, $beforeY, (Get-DeskIniInt $iniAfter 'card.account.x'), (Get-DeskIniInt $iniAfter 'card.account.y')))
                $afterInterior = Get-DeskInterior -BoardX 0 -BoardY 0 -BoardW $boardW -BoardH $boardH
                [void](Test-CardsInsideInterior -Ini $iniAfter -Interior $afterInterior -Result $result -Prefix 'after-drag')
            }
            Add-DeskCheck $result 'board still running after releasing on a small board' (Test-TestProcessAlive -ProcessId $processId)
            $shotAfter = Save-DeskWindowShot -Hwnd $window.Hwnd -X $window.X -Y $window.Y -W $window.W -H $window.H -Path (Join-Path $evidenceDir 'board-after-drag.png')
            $result.Evidence.Add("after-drag screenshot=$($shotAfter.Path)")
        }
    }

    # --- second run for the one-time migration check ----------------------
    if ($Definition.RunTwice) {
        $firstIni = $null
        $firstBackups = @()
        if ($ini) {
            $firstIni = [ordered]@{}
            foreach ($key in $ini.Keys) { $firstIni[$key] = $ini[$key] }
            $firstBackups = @(Get-BackupFiles -Directory $scenarioDir)
        }
        $close = Close-TestBoard -ProcessId $processId -Window $window -Dialogs $wait.Dialogs -Result $result
        Add-DeskCheck $result 'graceful close (first run)' ($close.Graceful -or $close.LeftRunning)
        Start-Sleep -Milliseconds 700

        $process2Id = Start-TestBoard -ExePath (Join-Path $scenarioDir 'desk.exe') -WorkingDirectory $scenarioDir -Suffix $Definition.Suffix
        $wait2 = Wait-BoardWindow -ProcessId $process2Id -Suffix $Definition.Suffix -TimeoutSeconds $TimeoutSeconds
        $secondWindow = $wait2.Window
        Add-DeskCheck $result 'second run starts' ($null -ne $secondWindow -and -not $wait2.Exited)
        [void](Wait-IniStable -Path $iniPath -TimeoutSeconds 5)
        $ini2 = Read-DeskIniFile -Path $iniPath
        if ($ini2) { Copy-Item -LiteralPath $iniPath -Destination (Join-Path $evidenceDir 'desk.ini.after-run2') -Force }
        $backups2 = @(Get-BackupFiles -Directory $scenarioDir)
        $newBackups2 = @($backups2 | Where-Object { $firstBackups -notcontains $_ })
        Add-DeskCheck $result 'second run does not migrate again (no new backup)' ($newBackups2.Count -eq 0) ("new: $($newBackups2 -join ', ')")
        if ($ini2 -and $firstIni) {
            $same = $true
            foreach ($key in @('board.x', 'board.y', 'board.w', 'board.h', 'card.account.x', 'card.account.y', 'card.ps.x', 'card.ps.y', 'card.servers.x', 'card.servers.y')) {
                $a = ''
                if ($firstIni.Contains($key)) { $a = $firstIni[$key] }
                $b = ''
                if ($ini2.Contains($key)) { $b = $ini2[$key] }
                if ($a -ne $b) { $same = $false }
            }
            Add-DeskCheck $result 'second run keeps migrated geometry' $same
            if ($secondWindow) {
                $result.Evidence.Add(("run2 boardrect={0},{1} {2}x{3}" -f $secondWindow.X, $secondWindow.Y, $secondWindow.W, $secondWindow.H))
                Add-DeskCheck $result 'second run board still 1280x1310 at (1280,0)' (
                    $secondWindow.X -eq 1280 -and $secondWindow.Y -eq 0 -and $secondWindow.W -eq 1280 -and $secondWindow.H -eq 1310) (
                    "$($secondWindow.X),$($secondWindow.Y) $($secondWindow.W)x$($secondWindow.H)")
            }
        }
        $close2 = Close-TestBoard -ProcessId $process2Id -Window $secondWindow -Dialogs $wait2.Dialogs -Result $result
        Add-DeskCheck $result 'graceful close (second run)' ($close2.Graceful -or $close2.LeftRunning)
    } else {
        $close = Close-TestBoard -ProcessId $processId -Window $window -Dialogs $wait.Dialogs -Result $result
        Add-DeskCheck $result 'graceful close via WM_CLOSE' ($close.Graceful -or $close.LeftRunning) ("graceful=$($close.Graceful) killed=$($close.Killed)")
    }

    # --- safety verification ---------------------------------------------
    Start-Sleep -Milliseconds 300
    $runKeyAfter = Get-DeskRunKeySnapshot
    $runKeyChanges = Compare-DeskRunKey -Before $runKeyBefore -After $runKeyAfter
    $runKeyDetail = $runKeyChanges -join '; '
    Add-DeskCheck $result 'HKCU Run autostart unchanged' ($runKeyChanges.Count -eq 0) $runKeyDetail
    if ($runKeyChanges.Count -gt 0) {
        Add-DeskNote $result ("AUTOSTART CHANGED - stop and report: " + ($runKeyChanges -join '; '))
    }
    $appDataAfter = Get-AppDataDeskFingerprint
    Add-DeskCheck $result 'live %APPDATA% desktop desk.ini untouched' ($appDataAfter -eq $appDataBefore) ("before=$appDataBefore after=$appDataAfter")

    # Any stray testrun process left behind?
    $stray = @(Get-Process -Name desk -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and ([System.IO.Path]::GetFullPath($_.Path)).StartsWith([System.IO.Path]::GetFullPath($TestRoot), [System.StringComparison]::OrdinalIgnoreCase)
    })
    $strayIds = ($stray | Select-Object -ExpandProperty Id) -join ','
    Add-DeskCheck $result 'no testrun process left behind' ($stray.Count -eq 0) ("left: $strayIds")

    [void](Save-DeskResult -Result $result -Path (Join-Path $evidenceDir 'result.json'))
    return $result
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

Write-Host ("desk-testrun: exe={0}" -f $Exe)
Write-Host ("desk-testrun: testroot={0}" -f $TestRoot)
Write-Host ("desk-testrun: evidence={0}" -f $RunDir)

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw "desk.exe not found: $Exe (run tools\build-lock.ps1 first)"
}
if (-not $DryRun -and -not (Test-TestHook -Path $Exe)) {
    $live = Get-LiveDeskProcess
    $message = "build\bin\desk.exe has no DESK_TEST_INSTANCE hook; refusing to launch a copy " +
        "(a hookless first run would rewrite the live app's HKCU Run value). " +
        "Ask the lead to land the hook in src\apps\desk\main.cpp and rebuild."
    if ($live.Count -gt 0) { $message += " Live instance: PID $($live.Id -join ',')." }
    throw $message
}

if (-not $DryRun) {
    New-Item -ItemType Directory -Path $RunDir -Force | Out-Null
}

$sign = -not $NoSign
$leftover = @(Stop-TestRunProcesses)
if ($leftover.Count -gt 0) {
    Write-Host ("desk-testrun: stopped leftover testrun processes: {0}" -f ($leftover -join ', '))
}
$summary = New-Object System.Collections.Generic.List[object]
foreach ($name in $Scenario) {
    $definition = Get-ScenarioDefinition -Name $name
    try {
        $result = Invoke-Scenario -Definition $definition -ExePath $Exe -Sign $sign -BaseRunDir $RunDir
    } catch {
        [void](Stop-TestRunProcesses)
        $result = New-DeskResult
        $result.Name = $name
        [void](Add-DeskCheck $result 'scenario completed without harness error' $false $_.Exception.Message)
        [void](Save-DeskResult -Result $result -Path (Join-Path (Join-Path $RunDir $name) 'result.json'))
    }
    $summary.Add($result)
}

Write-Host ""
Write-Host '=== summary'
foreach ($result in $summary) {
    $failed = @($result.Checks | Where-Object { -not $_.Pass })
    $tag = 'PASS'
    if ($failed.Count -gt 0) { $tag = 'FAIL' }
    $checkCount = $result.Checks.Count
    Write-Host ("[{0}] {1}  ({2} checks, {3} failed)" -f $tag, $result.Name, $checkCount, $failed.Count)
    foreach ($check in $failed) { Write-Host ("        FAIL: {0}  {1}" -f $check.Name, $check.Detail) }
}

if (-not $DryRun) {
    $summaryPath = Join-Path $RunDir 'summary.json'
    $payload = [pscustomobject]@{
        runId   = $RunId
        exe     = $Exe
        exeHash = (Get-ProcessHash $Exe)
        host    = $env:COMPUTERNAME
        user    = $env:USERNAME
        at      = (Get-Date).ToString('s')
        results = @($summary | ForEach-Object {
            [pscustomobject]@{
                name     = $_.Name
                pass     = -not (@($_.Checks | Where-Object { -not $_.Pass }).Count -gt 0)
                failed   = @($_.Checks | Where-Object { -not $_.Pass } | ForEach-Object { $_.Name })
                evidence = @($_.Evidence)
                notes    = @($_.Notes)
            }
        })
    }
    $json = $payload | ConvertTo-Json -Depth 10
    [System.IO.File]::WriteAllText($summaryPath, $json, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("summary: {0}" -f $summaryPath)
}

$failedTotal = @($summary | ForEach-Object { $_.Checks } | Where-Object { -not $_.Pass }).Count
if ($failedTotal -gt 0) { exit 1 }
exit 0
