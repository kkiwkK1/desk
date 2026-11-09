# desk-ui-test.ps1 - practical test of the POWERSHELL card on an isolated instance.
#
# Automates, against build\testrun\ui\desk.exe (DESK_TEST_INSTANCE=ui):
#   * card width 568px (measured from the rendered card border span),
#   * 80 terminal columns (a 100-char line must wrap after exactly 80 cells),
#   * CJK wide glyphs occupy two 7px cells (span of 8 x U+4E00 vs 8 x 'H'),
#   * click-to-focus + typing (posted WM_LBUTTONDOWN/WM_CHAR/WM_KEYDOWN),
#   * caret blink while focused,
#   * wheel scrollback, the yellow "down N" status chip, the BOTTOM button and
#     its click returning to the live tail.
#
# Everything is posted to the board window; no global input is injected. Items
# that stay a human/manual check are listed at the end of the run and in
# build\qa\acceptance.md (IME composition, real drag-select + Ctrl+Shift+C,
# global hotkeys, tray menu, how it actually looks on a real desktop).
#
# Evidence: build\qa\runs\<RunId>\ui\*.png + result.json
#
# Example:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\desk-ui-test.ps1 -RunId acceptance

param(
    [string]$Exe = '',
    [string]$RunId = '',
    [int]$TimeoutSeconds = 25,
    [switch]$Sign,
    [switch]$KeepProcess
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'desk-testlib.ps1')

if (-not $Exe) { $Exe = Join-Path $root 'build\bin\desk.exe' }
$TestRoot = Join-Path $root 'build\testrun'
$EvidenceRoot = Join-Path $root 'build\qa\runs'
if (-not $RunId) { $RunId = Get-Date -Format 'yyyyMMdd-HHmmss' }
$evidenceDir = Join-Path (Join-Path $EvidenceRoot $RunId) 'ui'
New-Item -ItemType Directory -Path $evidenceDir -Force | Out-Null

$result = New-DeskResult
$result.Name = 'ui-powershell-card'

function Save-DeskBitmapAs {
    param([object]$Bitmap, [string]$Path)
    $Bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
}

function Get-DeskBitmap {
    param([IntPtr]$Hwnd, [int]$W, [int]$H)
    $bitmap = [DeskTestImage]::PrintBitmap($Hwnd, $W, $H)
    if ($null -eq $bitmap) { return $null }
    if ([DeskTestImage]::IsBlank($bitmap)) { $bitmap.Dispose(); return $null }
    return $bitmap
}

function Get-InkClusters {
    # Groups of consecutive ink rows, each with its horizontal ink span.
    param([object]$Bitmap, [int]$X0, [int]$X1, [int]$Y0, [int]$Y1, [int]$MinLum = 110, [int]$Gap = 2)
    $rows = [DeskTestImage]::InkRows($Bitmap, $X0, $X1, $Y0, $Y1, $MinLum)
    $clusters = @()
    $start = -1
    $previous = -100
    foreach ($y in $rows) {
        if ($start -lt 0) { $start = $y; $previous = $y; continue }
        if ($y - $previous -le $Gap) { $previous = $y; continue }
        $span = [DeskTestImage]::InkSpan($Bitmap, $start, $previous + 1, $X0, $X1, $MinLum)
        $clusters += [pscustomobject]@{ Y0 = $start; Y1 = $previous; Span = $span }
        $start = $y
        $previous = $y
    }
    if ($start -ge 0) {
        $span = [DeskTestImage]::InkSpan($Bitmap, $start, $previous + 1, $X0, $X1, $MinLum)
        $clusters += [pscustomobject]@{ Y0 = $start; Y1 = $previous; Span = $span }
    }
    return $clusters
}

function Invoke-PsCommand {
    # A real user clicks the card before typing, and the board deliberately
    # drops the keyboard when focus moves elsewhere (WM_KILLFOCUS). This helper
    # does the same click before every command and retries once if the screen
    # did not change (focus was taken by another window in between).
    param([IntPtr]$Hwnd, [string]$Command, [int]$SettleMs = 1400, [int]$ClickX, [int]$ClickY)
    for ($attempt = 0; $attempt -lt 2; $attempt++) {
        [void](Send-DeskClick -Hwnd $Hwnd -X $ClickX -Y $ClickY)
        Start-Sleep -Milliseconds 250
        $before = Get-DeskBitmap -Hwnd $Hwnd -W $script:captureW -H $script:captureH
        Send-DeskText -Hwnd $Hwnd -Text $Command
        Start-Sleep -Milliseconds 120
        Send-DeskEnter -Hwnd $Hwnd
        Start-Sleep -Milliseconds $SettleMs
        $after = Get-DeskBitmap -Hwnd $Hwnd -W $script:captureW -H $script:captureH
        $diff = 0
        if ($before -and $after) {
            $diff = [DeskTestImage]::DiffFraction($before, $after, $consoleX, $consoleY, $consoleW, $consoleH, 30)
        }
        if ($before) { $before.Dispose() }
        if ($after) { $after.Dispose() }
        if ($diff -gt 0.0004) { return $true }
    }
    return $false
}

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { throw "desk.exe not found: $Exe" }
$bytes = [System.IO.File]::ReadAllBytes($Exe)
if (-not ([System.Text.Encoding]::Unicode.GetString($bytes)).Contains('DESK_TEST_INSTANCE')) {
    throw 'build\bin\desk.exe has no DESK_TEST_INSTANCE hook; refusing to start a copy'
}

# --- isolated instance with a known layout --------------------------------
$scenarioDir = Join-Path $TestRoot 'ui'
$leftover = @(Stop-AllDeskTestProcesses -Root $TestRoot)
if ($leftover.Count -gt 0) { Write-Host ("stopped leftover testrun processes: " + ($leftover -join ',')) }
[void](New-DeskTestDir -Path $scenarioDir -Root $TestRoot)
$copy = Join-Path $scenarioDir 'desk.exe'
[void][System.IO.File]::Copy($Exe, $copy, $true)
if ($Sign) { & (Join-Path $PSScriptRoot 'sign-local.ps1') -Path $copy | Out-Host }

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
$processId = Start-DeskTestInstance -ExePath $copy -WorkingDirectory $scenarioDir -Suffix 'ui'
$result.Evidence.Add("process.id=$processId")
$window = $null
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $deadline -and -not $window) {
    if (-not (Test-DeskTestAlive -ProcessId $processId)) { break }
    $candidates = @(Get-DeskWindowList -ProcessId $processId | Where-Object {
        $_.Visible -and ($_.Class -eq 'wg.desk-ui' -or $_.Class -eq 'wg.desk') })
    if ($candidates.Count -gt 0) { $window = $candidates[0] }
    if (-not $window) { Start-Sleep -Milliseconds 100 }
}
[void](Add-DeskCheck $result 'isolated board started' ($null -ne $window) ("pid=$processId"))
if (-not $window) {
    [void](Close-DeskBoard -ProcessId $processId -Hwnd ([IntPtr]::Zero) -Root $TestRoot)
    [void](Save-DeskResult -Result $result -Path (Join-Path $evidenceDir 'result.json'))
    exit 1
}
$result.Evidence.Add(("boardrect={0},{1} {2}x{3}" -f $window.X, $window.Y, $window.W, $window.H))
$script:captureW = $window.W
$script:captureH = $window.H

$iniPath = Join-Path $scenarioDir 'desk.ini'
[void](Wait-IniStable -Path $iniPath -TimeoutSeconds 6)
Start-Sleep -Milliseconds 800
$ini = Read-DeskIniFile -Path $iniPath
if (-not $ini) { throw 'desk.ini missing after startup' }

$psX = Get-DeskIniInt $ini 'card.ps.x' 14
$psY = Get-DeskIniInt $ini 'card.ps.y' 400
$psW = 568
$psExpanded = (Get-DeskIniInt $ini 'card.ps.expanded' 0) -ne 0
$psH = Get-DeskCardHeight -Id 'ps' -Expanded $psExpanded
$consoleX = $psX + 4
$consoleW = $psW - 8
$consoleY = $psY + 38 + 4
$consoleH = $psH - 48 - 8
$result.Evidence.Add(("card ps at ({0},{1}) size {2}x{3}; console {4}x{5}" -f $psX, $psY, $psW, $psH, $consoleW, $consoleH))
Write-Host ("[ui] PS card at {0},{1} {2}x{3}; console inner {4}x{5}" -f $psX, $psY, $psW, $psH, $consoleW, $consoleH)

# --- capture helpers ------------------------------------------------------
$bitmap = Get-DeskBitmap -Hwnd $window.Hwnd -W $window.W -H $window.H
if (-not $bitmap) { throw 'cannot capture the board window' }
try { Save-DeskBitmapAs -Bitmap $bitmap -Path (Join-Path $evidenceDir 'ui-01-initial.png') } finally { $bitmap.Dispose() }

# --- card width from the rendered border ---------------------------------
$cardSpan = 0
$bitmap = Get-DeskBitmap -Hwnd $window.Hwnd -W $window.W -H $window.H
try {
    $cardSpan = [DeskTestImage]::InkSpan($bitmap, $psY + 4, $psY + 26, $psX - 10, $psX + 600, 70)
} finally { $bitmap.Dispose() }
[void](Add-DeskCheck $result 'PS card renders 568 px wide' ($cardSpan -ge 560 -and $cardSpan -le 572) ("measured border span = $cardSpan px"))

# --- click into the terminal body: focus + caret --------------------------
$clickX = $psX + 200
$clickY = $psY + 38 + 60
[void](Send-DeskClick -Hwnd $window.Hwnd -X $clickX -Y $clickY)
Start-Sleep -Milliseconds 400

# The caret toggles every 530ms; a single pair of frames can land in the
# same phase, so sample four frames 200ms apart and require one change.
$caretDiff = 0
$bitmapA = Get-DeskBitmap -Hwnd $window.Hwnd -W $window.W -H $window.H
Save-DeskBitmapAs -Bitmap $bitmapA -Path (Join-Path $evidenceDir 'ui-02-caret-a.png')
for ($frame = 0; $frame -lt 4; $frame++) {
    Start-Sleep -Milliseconds 200
    $bitmapB = Get-DeskBitmap -Hwnd $window.Hwnd -W $window.W -H $window.H
    $frameDiff = [DeskTestImage]::DiffFraction($bitmapA, $bitmapB, $consoleX, $consoleY, $consoleW, $consoleH, 40)
    if ($frameDiff -gt $caretDiff) { $caretDiff = $frameDiff }
    if ($frame -eq 1) { Save-DeskBitmapAs -Bitmap $bitmapB -Path (Join-Path $evidenceDir 'ui-02-caret-b.png') }
    $bitmapB.Dispose()
}
$bitmapA.Dispose()
$caretOk = $caretDiff -gt 0.00001
$caretDetail = "{0:F6} of the console changes between blink frames" -f $caretDiff
[void](Add-DeskCheck $result 'caret blinks after clicking the card' $caretOk $caretDetail)

# --- 80 columns: an 80-char line fits, a 100-char line wraps after 80 ----
$applied = Invoke-PsCommand -Hwnd $window.Hwnd -Command '"A"*80' -SettleMs 1600 -ClickX $clickX -ClickY $clickY
[void](Add-DeskCheck $result 'typing reaches the terminal (80-char line)' $applied)
$wideSpan80 = 0
$bitmap = Get-DeskBitmap -Hwnd $window.Hwnd -W $window.W -H $window.H
try {
    Save-DeskBitmapAs -Bitmap $bitmap -Path (Join-Path $evidenceDir 'ui-03-eighty-columns.png')
    $clusters = @(Get-InkClusters -Bitmap $bitmap -X0 $consoleX -X1 ($consoleX + $consoleW) -Y0 $consoleY -Y1 ($consoleY + $consoleH) -MinLum 110)
    $widest = @($clusters | Sort-Object Span -Descending | Select-Object -First 1)
    if ($widest.Count -gt 0) { $wideSpan80 = $widest[0].Span }
} finally { $bitmap.Dispose() }
[void](Add-DeskCheck $result '80 monospace cells fit on one terminal row' ($wideSpan80 -ge 530 -and $wideSpan80 -le 566) ("measured ink span = $wideSpan80 px (expect ~558 for 80 cells)"))

$applied = Invoke-PsCommand -Hwnd $window.Hwnd -Command '"C"*100' -SettleMs 1600 -ClickX $clickX -ClickY $clickY
[void](Add-DeskCheck $result 'typing reaches the terminal (100-char line)' $applied)
$bitmap = Get-DeskBitmap -Hwnd $window.Hwnd -W $window.W -H $window.H
try {
    Save-DeskBitmapAs -Bitmap $bitmap -Path (Join-Path $evidenceDir 'ui-04-wrap-at-80.png')
    $clusters = @(Get-InkClusters -Bitmap $bitmap -X0 $consoleX -X1 ($consoleX + $consoleW) -Y0 $consoleY -Y1 ($consoleY + $consoleH) -MinLum 110)
} finally { $bitmap.Dispose() }
$clusters = @($clusters | Sort-Object Y0)
$fullRow = $null
$tailRow = $null
for ($i = 0; $i -lt $clusters.Count - 1; $i++) {
    if ($clusters[$i].Span -ge 520 -and $clusters[$i + 1].Span -ge 80 -and $clusters[$i + 1].Span -le 200) {
        $fullRow = $clusters[$i]
        $tailRow = $clusters[$i + 1]
    }
}
$wrapOk = ($null -ne $fullRow) -and ($null -ne $tailRow)
$wrapDetail = 'no 80/20 split found'
if ($wrapOk) { $wrapDetail = "first row span=$($fullRow.Span)px, wrapped tail span=$($tailRow.Span)px" }
[void](Add-DeskCheck $result '100-char line wraps after 80 cells' $wrapOk $wrapDetail)
$tailDetail = 'n/a'
$tailOk = $false
if ($wrapOk) { $tailOk = ($tailRow.Span -ge 120 -and $tailRow.Span -le 170); $tailDetail = "tail span = $($tailRow.Span) px" }
[void](Add-DeskCheck $result 'wrapped remainder is 20 cells wide' $tailOk $tailDetail)

# --- CJK glyphs occupy two cells ------------------------------------------
[void](Invoke-PsCommand -Hwnd $window.Hwnd -Command 'Clear-Host' -SettleMs 900 -ClickX $clickX -ClickY $clickY)
[void](Invoke-PsCommand -Hwnd $window.Hwnd -Command '([string][char]0x4E00)*8' -SettleMs 1600 -ClickX $clickX -ClickY $clickY)
$wideSpan = 0
$bitmap = Get-DeskBitmap -Hwnd $window.Hwnd -W $window.W -H $window.H
try {
    Save-DeskBitmapAs -Bitmap $bitmap -Path (Join-Path $evidenceDir 'ui-05-cjk-wide.png')
    $clusters = @(Get-InkClusters -Bitmap $bitmap -X0 $consoleX -X1 ($consoleX + $consoleW) -Y0 $consoleY -Y1 ($consoleY + $consoleH) -MinLum 110 | Sort-Object Y0)
    if ($clusters.Count -ge 2) { $wideSpan = $clusters[1].Span }
} finally { $bitmap.Dispose() }
[void](Add-DeskCheck $result '8 CJK glyphs span two cells each' ($wideSpan -ge 95 -and $wideSpan -le 118) ("8 x U+4E00 ink span = $wideSpan px (expect ~14 px per glyph)"))

[void](Invoke-PsCommand -Hwnd $window.Hwnd -Command 'Clear-Host' -SettleMs 900 -ClickX $clickX -ClickY $clickY)
[void](Invoke-PsCommand -Hwnd $window.Hwnd -Command '"H"*8' -SettleMs 1600 -ClickX $clickX -ClickY $clickY)
$asciiSpan = 0
$bitmap = Get-DeskBitmap -Hwnd $window.Hwnd -W $window.W -H $window.H
try {
    Save-DeskBitmapAs -Bitmap $bitmap -Path (Join-Path $evidenceDir 'ui-06-ascii-single-cell.png')
    # Row 0 is the prompt + echoed command, row 1 is the command output.
    $clusters = @(Get-InkClusters -Bitmap $bitmap -X0 $consoleX -X1 ($consoleX + $consoleW) -Y0 $consoleY -Y1 ($consoleY + $consoleH) -MinLum 110 | Sort-Object Y0)
    if ($clusters.Count -ge 2) { $asciiSpan = $clusters[1].Span }
} finally { $bitmap.Dispose() }
[void](Add-DeskCheck $result '8 ASCII glyphs span one cell each' ($asciiSpan -ge 46 -and $asciiSpan -le 64) ("8 x H ink span = $asciiSpan px (expect ~7 px per cell)"))
$ratio = 0
if ($asciiSpan -gt 0) { $ratio = [math]::Round($wideSpan / $asciiSpan, 2) }
[void](Add-DeskCheck $result 'CJK glyph advances twice an ASCII glyph' ($ratio -gt 1.6) ("wide/ascii ink span ratio = $ratio"))

# --- wheel scrollback + BOTTOM button --------------------------------------
[void](Invoke-PsCommand -Hwnd $window.Hwnd -Command 'Clear-Host' -SettleMs 700 -ClickX $clickX -ClickY $clickY)
$applied = Invoke-PsCommand -Hwnd $window.Hwnd -Command '1..60|%{"L$_"}' -SettleMs 3000 -ClickX $clickX -ClickY $clickY
[void](Add-DeskCheck $result 'typing reaches the terminal (60-line output)' $applied)
$before = Get-DeskBitmap -Hwnd $window.Hwnd -W $window.W -H $window.H
Save-DeskBitmapAs -Bitmap $before -Path (Join-Path $evidenceDir 'ui-07-tail.png')

$wheelScreenX = $window.X + $psX + 200
$wheelScreenY = $window.Y + $psY + 38 + 100
for ($i = 0; $i -lt 6; $i++) {
    [void](Send-DeskWheel -Hwnd $window.Hwnd -ScreenX $wheelScreenX -ScreenY $wheelScreenY -Delta 120)
    Start-Sleep -Milliseconds 120
}
Start-Sleep -Milliseconds 500
$bitmap = Get-DeskBitmap -Hwnd $window.Hwnd -W $window.W -H $window.H
# BOTTOM button rect (ps_view::BottomButton: body.right()-78, body.bottom()-16, 72x12)
$buttonX = $psX + $psW - 78
$buttonY = $psY + 38 + ($psH - 48) - 16
$chipX = $psX + $psW - 150
$chipY = $psY + 4
$buttonWarmAfterScroll = 0
try {
    Save-DeskBitmapAs -Bitmap $bitmap -Path (Join-Path $evidenceDir 'ui-08-scrolled-back.png')
    $scrollDiff = [DeskTestImage]::DiffFraction($before, $bitmap, $consoleX, $consoleY, $consoleW, $consoleH, 30)
    $buttonWarmAfterScroll = [DeskTestImage]::CountWarmPixels($bitmap, $buttonX, $buttonY, 72, 12)
    $chipWarmBefore = [DeskTestImage]::CountWarmPixels($before, $chipX, $chipY, 120, 20)
    $chipWarmAfter = [DeskTestImage]::CountWarmPixels($bitmap, $chipX, $chipY, 120, 20)
} finally {
    $before.Dispose()
    $bitmap.Dispose()
}
[void](Add-DeskCheck $result 'wheel scrolls the terminal history back' ($scrollDiff -gt 0.002) ("console diff = {0:P2}" -f $scrollDiff))
[void](Add-DeskCheck $result 'scrolled-back card shows the yellow status chip' ($chipWarmAfter -gt ($chipWarmBefore + 5)) ("warm px before=$chipWarmBefore after=$chipWarmAfter"))
[void](Add-DeskCheck $result 'scrolled-back card paints the BOTTOM button' ($buttonWarmAfterScroll -gt 5) ("warm px in button rect = $buttonWarmAfterScroll"))

$buttonClientX = $psX + $psW - 42
$buttonClientY = $psY + 38 + ($psH - 48) - 10
[void](Send-DeskClick -Hwnd $window.Hwnd -X $buttonClientX -Y $buttonClientY)
Start-Sleep -Milliseconds 700
$buttonWarmNow = 0
$bitmap = Get-DeskBitmap -Hwnd $window.Hwnd -W $window.W -H $window.H
try {
    Save-DeskBitmapAs -Bitmap $bitmap -Path (Join-Path $evidenceDir 'ui-09-bottom-returned.png')
    $buttonWarmNow = [DeskTestImage]::CountWarmPixels($bitmap, $buttonX, $buttonY, 72, 12)
} finally { $bitmap.Dispose() }
[void](Add-DeskCheck $result 'BOTTOM click returns to the live tail' ($buttonWarmNow -lt 3) ("warm px in button rect after click = $buttonWarmNow"))

# --- close and safety -----------------------------------------------------
$close = Close-DeskBoard -ProcessId $processId -Hwnd $window.Hwnd -Root $TestRoot
[void](Add-DeskCheck $result 'board closed gracefully' $close.Graceful ("killed=$($close.Killed)"))
$runKeyAfter = Get-DeskRunKeySnapshot
$changes = Compare-DeskRunKey -Before $runKeyBefore -After $runKeyAfter
[void](Add-DeskCheck $result 'HKCU Run autostart unchanged' ($changes.Count -eq 0) ($changes -join '; '))

Add-DeskNote $result 'manual checks left to a human: IME composition (Chinese candidate window), drag-select and Ctrl+Shift+C copy, global hotkeys, tray menu, visual review on a real desktop.'
[void](Save-DeskResult -Result $result -Path (Join-Path $evidenceDir 'result.json'))
$failed = @($result.Checks | Where-Object { -not $_.Pass })
foreach ($check in $failed) { Write-Host ("FAIL: {0}  {1}" -f $check.Name, $check.Detail) }
Write-Host ("ui checks: {0}, failed: {1}" -f $result.Checks.Count, $failed.Count)
Write-Host ("evidence: {0}" -f $evidenceDir)
if ($failed.Count -gt 0) { exit 1 }
exit 0
