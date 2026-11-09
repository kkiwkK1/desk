#requires -version 5.1
<#
.SYNOPSIS
    M1-P acceptance: real installed Pi, offline, zero tools after resuming a
    tool-bearing session - with a sensitivity control.

.DESCRIPTION
    Runs the installed Pi CLI three times against tests/pi_policy_provider.ts
    (a local `faux` provider extension: no network, no credentials, no tool
    execution). The extension records the tool set Pi actually declared to the
    model, read from the normalized transcript with getCurrentTools(), and
    answers with the fixed token POLICY_OK.

    Phase 1 "baseline":  fresh session, --tools read    -> capture must contain read
    Phase 2 "hardened":  formal fork of phase 1 + --no-tools --exclude-tools * -> capture must be empty
    Phase 3 "control":   separate fork of the SAME ORIGINAL phase-1 session,
                         no tool flags                   -> capture must contain read again

    Phase 3 never forks the hardened session, so an empty capture cannot be
    explained by a mutated fixture. The phase-1 file hash is re-checked at the
    end to prove forking did not modify the source session.

    Every artifact (private agent dir, fixture cwd, session dirs, captures,
    logs, result.json) lives under build/qa/pi-policy/<runid>/ and is created by
    this script. The script itself only writes under that fixture directory; the
    installed Pi package and the persistent global Pi configuration are never
    modified, although Pi's own startup may briefly create and delete install
    lock files under its agent root.

    No key value is ever logged: the child environment is copied without
    name-matching canaries (keys/tokens/secrets/session vars) and without
    inherited PI_* configuration.

    Not part of build.bat: the regular fake-RPC regression does not need an
    installed Pi. Run this separately, ideally from the verifier role.

.PARAMETER RunId
    Fixture directory name under build/qa/pi-policy/. Must be new.

.PARAMETER Root
    Repository root. Default: parent of this script's directory.

.PARAMETER PiAgentRoot
    Pi agent root that owns the `install` directory. Default: %USERPROFILE%\.pi\agent.

.PARAMETER NodeExe
    node.exe used to launch the Pi CLI entry. Default: node from PATH.

.PARAMETER TimeoutSeconds
    Per-phase wall clock limit. Default 240.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\check-pi-policy.ps1

.OUTPUTS
    build/qa/pi-policy/<runid>/result.json

    Exit codes:
      0  pass
      2  usage or fixture-path conflict (nothing ran)
      3  environment cannot host the probe (node/Pi/extension missing)
      10 policy failure - the hardened phase still declared tools
      11 harness invalid - the instrument itself did not behave
#>
[CmdletBinding()]
param(
    [string]$RunId,
    [string]$Root,
    [string]$PiAgentRoot,
    [string]$NodeExe,
    [int]$TimeoutSeconds = 240
)

$ErrorActionPreference = 'Stop'

# Must match the exported constants in tests/pi_policy_provider.ts.
$policyProviderId = 'pi-policy-faux'
$policyModelId = 'pi-policy-model'
$policyReply = 'POLICY_OK'
$captureEnvName = 'PI_POLICY_CAPTURE_FILE'

# Pi, node and this script all write UTF-8. Windows PowerShell 5.1's Get-Content
# without -Encoding falls back to the ANSI code page (GBK on zh-CN hosts), which
# corrupts non-ASCII repository paths in session transcripts and captures. Every
# machine-readable input below is decoded explicitly as UTF-8 instead.
$utf8Read = New-Object System.Text.UTF8Encoding($false)
# Everything this script writes as machine-readable output (stdout/stderr logs and
# result.json) must be UTF-8 without a BOM: Node's JSON.parse rejects a leading
# BOM. Defined up front so an early internal error can still produce a readable
# harness-invalid result.json.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Stop-Script {
    param([int]$Code, [string]$Message)
    [Console]::Error.WriteLine('check-pi-policy: ' + $Message)
    exit $Code
}

function ConvertTo-ArgumentLine {
    param([string[]]$Parts)
    $builder = New-Object System.Text.StringBuilder
    foreach ($part in $Parts) {
        if ($builder.Length -gt 0) { [void]$builder.Append(' ') }
        $escaped = ([string]$part) -replace '"', '\"'
        [void]$builder.Append('"').Append($escaped).Append('"')
    }
    return $builder.ToString()
}

# ---------------------------------------------------------------------------
# Resolve inputs (read-only; fail closed before creating anything)
# ---------------------------------------------------------------------------

$scriptRoot = $PSScriptRoot
if (-not $scriptRoot) { Stop-Script 2 'cannot determine the script directory' }

if (-not $Root) { $Root = Split-Path -Parent $scriptRoot }
if (-not (Test-Path -LiteralPath $Root -PathType Container)) { Stop-Script 2 ('repository root not found: ' + $Root) }
$Root = (Resolve-Path -LiteralPath $Root).Path

$extensionPath = Join-Path $Root 'tests\pi_policy_provider.ts'
if (-not (Test-Path -LiteralPath $extensionPath -PathType Leaf)) {
    Stop-Script 2 ('missing provider extension: ' + $extensionPath)
}
$extensionText = [System.IO.File]::ReadAllText($extensionPath, $utf8Read)
foreach ($literal in @($policyProviderId, $policyModelId, $policyReply)) {
    if ($extensionText -notmatch [regex]::Escape($literal)) {
        Stop-Script 3 ('provider extension does not declare ' + $literal + '; contract drift')
    }
}

if (-not $RunId) { $RunId = Get-Date -Format 'yyyyMMdd-HHmmss' }
if ($RunId -notmatch '^[0-9A-Za-z][0-9A-Za-z._-]*$') { Stop-Script 2 ('invalid -RunId: ' + $RunId) }

if (-not $NodeExe) {
    $nodeCommand = Get-Command node -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $nodeCommand) { Stop-Script 3 'node.exe not found on PATH; pass -NodeExe' }
    $NodeExe = $nodeCommand.Source
}
if (-not (Test-Path -LiteralPath $NodeExe -PathType Leaf)) { Stop-Script 3 ('node.exe not found: ' + $NodeExe) }
$NodeExe = (Resolve-Path -LiteralPath $NodeExe).Path

if (-not $PiAgentRoot) { $PiAgentRoot = Join-Path $env:USERPROFILE '.pi\agent' }
if (-not (Test-Path -LiteralPath $PiAgentRoot -PathType Container)) { Stop-Script 3 ('Pi agent root not found: ' + $PiAgentRoot) }
$PiAgentRoot = (Resolve-Path -LiteralPath $PiAgentRoot).Path

$installRoot = Join-Path $PiAgentRoot 'install'
$currentVersionFile = Join-Path $installRoot 'current-version'
if (-not (Test-Path -LiteralPath $currentVersionFile -PathType Leaf)) {
    Stop-Script 3 ('Pi install marker not found: ' + $currentVersionFile)
}
$piRelease = ([System.IO.File]::ReadAllText($currentVersionFile, $utf8Read)).Trim()
if ($piRelease -notmatch '^[0-9A-Za-z._+-]+$') { Stop-Script 3 ('invalid Pi release marker: ' + $currentVersionFile) }

$packageDir = Join-Path $installRoot ('releases\' + $piRelease + '\node_modules\@earendil-works\pi-coding-agent')
$packageJsonPath = Join-Path $packageDir 'package.json'
if (-not (Test-Path -LiteralPath $packageJsonPath -PathType Leaf)) {
    Stop-Script 3 ('installed Pi package not found: ' + $packageDir)
}
$packageJson = [System.IO.File]::ReadAllText($packageJsonPath, $utf8Read) | ConvertFrom-Json
$binField = $packageJson.bin
if ($binField -is [string]) { $binRelative = $binField } else { $binRelative = $binField.pi }
if (-not $binRelative) { Stop-Script 3 'installed Pi package does not declare a pi bin' }
$piEntry = Join-Path $packageDir ([string]$binRelative)
if (-not (Test-Path -LiteralPath $piEntry -PathType Leaf)) { Stop-Script 3 ('Pi CLI entry not found: ' + $piEntry) }
$piVersion = [string]$packageJson.version
if (-not $piVersion) { $piVersion = $piRelease }

# ---------------------------------------------------------------------------
# Fixture layout (never reuse an existing run directory)
# ---------------------------------------------------------------------------

$fixtureRoot = Join-Path $Root ('build\qa\pi-policy\' + $RunId)
if (Test-Path -LiteralPath $fixtureRoot) {
    Stop-Script 2 ('refusing to reuse an existing fixture directory: ' + $fixtureRoot)
}

$agentDir = Join-Path $fixtureRoot 'agent'
$fixtureCwd = Join-Path $fixtureRoot 'cwd'
$logDir = Join-Path $fixtureRoot 'logs'
$sessionBase = Join-Path $fixtureRoot 'sessions'
$captureDir = Join-Path $fixtureRoot 'captures'
$resultFile = Join-Path $fixtureRoot 'result.json'
$phaseNames = @('baseline', 'hardened', 'control')

foreach ($directory in @($fixtureRoot, $agentDir, $fixtureCwd, $logDir, $sessionBase, $captureDir)) {
    [void](New-Item -ItemType Directory -Path $directory -Force)
}
foreach ($phaseName in $phaseNames) {
    [void](New-Item -ItemType Directory -Path (Join-Path $sessionBase $phaseName) -Force)
}
Set-Content -LiteralPath (Join-Path $fixtureCwd 'fixture-marker.txt') -Value 'pi policy fixture cwd' -Encoding ASCII

$startedAt = (Get-Date).ToString('o')

# ---------------------------------------------------------------------------
# Child environment: parent copy minus credential-shaped names and PI_* state
# ---------------------------------------------------------------------------

$stripPattern = '(?i)(key|token|secret|password|passwd|credential|bearer|sess)'
$parentEnvironment = [System.Environment]::GetEnvironmentVariables()
$baseEnvironment = @{}
$strippedCount = 0
foreach ($entry in $parentEnvironment.GetEnumerator()) {
    $name = [string]$entry.Key
    $isPiOwned = ($name -match '(?i)^PI_') -or ($name -eq 'AI_AGENT')
    if ($isPiOwned -or ($name -match $stripPattern)) { $strippedCount++; continue }
    $baseEnvironment[$name] = [string]$entry.Value
}

function New-ChildEnvironment {
    param([string]$CaptureFile)
    $map = @{}
    foreach ($name in $baseEnvironment.Keys) { $map[$name] = $baseEnvironment[$name] }
    $map['PI_CODING_AGENT_DIR'] = $agentDir
    $map['PI_OFFLINE'] = '1'
    $map['PI_SKIP_VERSION_CHECK'] = '1'
    $map['PI_TELEMETRY'] = '0'
    $map['PI_MANAGED_INSTALL_ROOT'] = $installRoot
    $map[$captureEnvName] = $CaptureFile
    return $map
}

# ---------------------------------------------------------------------------
# Phase runner
# ---------------------------------------------------------------------------

function Invoke-PiPhase {
    param(
        [string]$Name,
        [string[]]$Arguments,
        [string]$CaptureFile
    )

    $stdoutFile = Join-Path $logDir ($Name + '.stdout.txt')
    $stderrFile = Join-Path $logDir ($Name + '.stderr.txt')
    $argumentLine = ConvertTo-ArgumentLine (@($piEntry) + $Arguments)

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $NodeExe
    $startInfo.Arguments = $argumentLine
    $startInfo.WorkingDirectory = $fixtureCwd
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = $utf8NoBom
    $startInfo.StandardErrorEncoding = $utf8NoBom

    $childEnvironment = New-ChildEnvironment -CaptureFile $CaptureFile
    $startInfo.EnvironmentVariables.Clear()
    foreach ($name in $childEnvironment.Keys) {
        $startInfo.EnvironmentVariables[$name] = $childEnvironment[$name]
    }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    [void]$process.Start()
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()

    $timedOut = $false
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $timedOut = $true
        try { $process.Kill() } catch { }
        [void]$process.WaitForExit(15000)
    }

    $exitCode = -1
    try { $exitCode = $process.ExitCode } catch { $exitCode = -1 }
    $stdout = ''
    $stderr = ''
    try { $stdout = $stdoutTask.Result } catch { $stdout = '' }
    try { $stderr = $stderrTask.Result } catch { $stderr = '' }
    try { $process.Dispose() } catch { }

    [System.IO.File]::WriteAllText($stdoutFile, $stdout, $utf8NoBom)
    [System.IO.File]::WriteAllText($stderrFile, $stderr, $utf8NoBom)

    return [pscustomobject]@{
        Name       = $Name
        ExitCode   = $exitCode
        TimedOut   = $timedOut
        Stdout     = $stdout
        Stderr     = $stderr
        StdoutFile = $stdoutFile
        StderrFile = $stderrFile
        Arguments  = $Arguments
    }
}

function Get-PhaseSessionFile {
    param([string]$Name)
    $directory = Join-Path $sessionBase $Name
    $files = @(Get-ChildItem -LiteralPath $directory -Filter '*.jsonl' -File -ErrorAction SilentlyContinue)
    if ($files.Count -ne 1) { return $null }
    return $files[0].FullName
}

function Read-SessionHeader {
    param([string]$Path)
    try {
        $firstLine = [System.IO.File]::ReadLines($Path, $utf8Read) | Select-Object -First 1
        return ($firstLine | ConvertFrom-Json)
    } catch {
        return $null
    }
}

function Read-Capture {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    try {
        return ([System.IO.File]::ReadAllText($Path, $utf8Read) | ConvertFrom-Json)
    } catch {
        return $null
    }
}

function Get-CaptureRequests {
    param($Capture)
    if (-not $Capture) { return }
    if (-not $Capture.requests) { return }
    return $Capture.requests
}

# Union of tool names over every request of one phase. A retry or an internal
# summarization request cannot hide a declaration behind a later empty one.
function Get-CaptureToolUnion {
    param($Requests)
    $names = New-Object System.Collections.Generic.List[string]
    foreach ($request in @($Requests)) {
        foreach ($name in @($request.toolNames)) {
            $text = [string]$name
            if ($text -and -not $names.Contains($text)) { [void]$names.Add($text) }
        }
    }
    if ($names.Count -eq 0) { return }
    return $names.ToArray()
}

function Get-RequestSummaries {
    param($Requests)
    $list = New-Object System.Collections.Generic.List[object]
    foreach ($request in @($Requests)) {
        $names = @($request.toolNames | ForEach-Object { [string]$_ })
        [void]$list.Add([ordered]@{ index = $request.index; toolCount = $names.Count; toolNames = $names })
    }
    if ($list.Count -eq 0) { return }
    return $list.ToArray()
}

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

$checks = New-Object System.Collections.Generic.List[object]
function Add-Check {
    param([string]$Name, [string]$Class, [bool]$Ok, [string]$Detail)
    [void]$checks.Add([pscustomobject]@{ name = $Name; class = $Class; ok = $Ok; detail = $Detail })
}

# Minimal, always-serializable result used when the primary result.json cannot be
# produced. Keeps classification=harness-invalid, passed=false and exit 11.
function Write-FallbackResult {
    param([string]$Message)
    $minimal = [ordered]@{
        schemaVersion  = 1
        runId          = $RunId
        startedAt      = $startedAt
        finishedAt     = (Get-Date).ToString('o')
        piVersion      = $piVersion
        extension      = $extensionPath
        fixtureRoot    = $fixtureRoot
        provider       = $policyProviderId
        model          = $policyModelId
        failures       = @([ordered]@{ class = 'harness'; name = 'internal-error'; detail = $Message })
        classification = 'harness-invalid'
        passed         = $false
    }
    try {
        [System.IO.File]::WriteAllText($resultFile, ($minimal | ConvertTo-Json -Depth 6), $utf8NoBom)
        return $true
    } catch {
        try {
            $text = '{"schemaVersion":1,"runId":"' + $RunId + '","classification":"harness-invalid","passed":false}'
            [System.IO.File]::WriteAllText($resultFile, $text, $utf8NoBom)
            return $true
        } catch {
            return $false
        }
    }
}

$phaseRecords = [ordered]@{}
$baselineSession = $null
$baselineHashBefore = $null
$baselineHashAfter = $null
$baselineTools = @()
$hardenedTools = @()
$controlTools = @()

# Any unexpected terminating error still has to leave behind a harness-invalid
# result and a non-zero exit instead of a bare PowerShell stack trace. The error
# message is reported, never swallowed; assertions are not relaxed.
trap {
    $trapMessage = $_.Exception.Message
    $trapChecks = @()
    if ($checks -and $checks.Count -gt 0) { $trapChecks = $checks.ToArray() }
    $trapPhases = [ordered]@{}
    if ($phaseRecords -and $phaseRecords.Count -gt 0) { $trapPhases = $phaseRecords }

    $trapPayload = [ordered]@{
        schemaVersion      = 1
        runId              = $RunId
        startedAt          = $startedAt
        finishedAt         = (Get-Date).ToString('o')
        piVersion          = $piVersion
        piEntry            = $piEntry
        extension          = $extensionPath
        fixtureRoot        = $fixtureRoot
        provider           = $policyProviderId
        model              = $policyModelId
        baselineToolNames  = @($baselineTools)
        hardenedToolNames  = @($hardenedTools)
        controlToolNames   = @($controlTools)
        phases             = $trapPhases
        checks             = $trapChecks
        failures           = @([ordered]@{ class = 'harness'; name = 'internal-error'; detail = $trapMessage })
        classification     = 'harness-invalid'
        passed             = $false
        note               = 'unexpected internal failure before the normal verdict'
    }

    $trapWritten = $false
    try {
        [System.IO.File]::WriteAllText($resultFile, ($trapPayload | ConvertTo-Json -Depth 10), $utf8NoBom)
        $trapWritten = $true
    } catch {
        $trapWriteMessage = $_.Exception.Message
        try { Set-Content -LiteralPath (Join-Path $logDir 'trap-error.txt') -Value $trapWriteMessage -Encoding UTF8 } catch { }
        if (Write-FallbackResult -Message ('failed to serialize the trap result: ' + $trapWriteMessage)) { $trapWritten = $true }
    }

    [Console]::Error.WriteLine('check-pi-policy: harness-invalid (internal error)')
    [Console]::Error.WriteLine('  ! ' + $trapMessage)
    if ($trapWritten) { [Console]::Error.WriteLine('result: ' + $resultFile) }
    exit 11
}

$commonArgs = @(
    '--print',
    '--provider', $policyProviderId,
    '--model', $policyModelId,
    '--thinking', 'off',
    '--extension', $extensionPath,
    '--no-extensions',
    '--no-mcp',
    '--no-skills',
    '--no-prompt-templates',
    '--no-themes',
    '--no-context-files',
    '--no-approve',
    '--offline'
)
$probePrompt = 'Pi policy probe: answer with the fixed probe token only.'

# --- Phase 1: baseline session that really declares "read" ------------------
$baselineCapture = Join-Path $captureDir 'baseline.json'
$baselineSessionDir = Join-Path $sessionBase 'baseline'
$baselineArgs = $commonArgs + @('--tools', 'read', '--session-dir', $baselineSessionDir) + @($probePrompt)
$baselineRun = Invoke-PiPhase -Name 'baseline' -Arguments $baselineArgs -CaptureFile $baselineCapture

Add-Check 'baseline-exit-zero' 'harness' ($baselineRun.ExitCode -eq 0) ('exit=' + $baselineRun.ExitCode + ' timedOut=' + $baselineRun.TimedOut)
Add-Check 'baseline-reply-token' 'harness' ($baselineRun.Stdout -match [regex]::Escape($policyReply)) 'stdout must contain the fixed provider token'
$baselineSession = Get-PhaseSessionFile -Name 'baseline'
Add-Check 'baseline-session-file' 'harness' ($null -ne $baselineSession) 'exactly one .jsonl expected in the baseline session dir'

$baselineCaptureData = Read-Capture -Path $baselineCapture
$baselineRequests = @(Get-CaptureRequests -Capture $baselineCaptureData)
Add-Check 'baseline-capture-file' 'harness' ($baselineRequests.Count -gt 0) ('capture missing or unreadable: ' + $baselineCapture)
$baselineTools = @(Get-CaptureToolUnion -Requests $baselineRequests)
Add-Check 'baseline-declares-read' 'harness' ($baselineTools -contains 'read') ('baseline tools: ' + ($baselineTools -join ', '))

if ($baselineSession) {
    $baselineHashBefore = (Get-FileHash -LiteralPath $baselineSession -Algorithm SHA256).Hash
}
$baselineHeader = $null
if ($baselineSession) { $baselineHeader = Read-SessionHeader -Path $baselineSession }

$baselineRecord = [ordered]@{
    exitCode       = $baselineRun.ExitCode
    timedOut       = $baselineRun.TimedOut
    sessionDir     = $baselineSessionDir
    sessionFile    = $baselineSession
    sessionId      = if ($baselineHeader) { $baselineHeader.id } else { $null }
    parentSession  = if ($baselineHeader) { $baselineHeader.parentSession } else { $null }
    captureFile    = $baselineCapture
    requestCount   = $baselineRequests.Count
    requests       = @(Get-RequestSummaries -Requests $baselineRequests)
    toolCount      = $baselineTools.Count
    toolNames      = $baselineTools
    replyTokenSeen = [bool]($baselineRun.Stdout -match [regex]::Escape($policyReply))
    stdoutFile     = $baselineRun.StdoutFile
    stderrFile     = $baselineRun.StderrFile
    arguments      = $baselineRun.Arguments
}
$phaseRecords['baseline'] = $baselineRecord

if ($baselineSession) {
    # --- Phase 2: formal fork of the baseline session, hard zero tools ------
    $hardenedCapture = Join-Path $captureDir 'hardened.json'
    $hardenedSessionDir = Join-Path $sessionBase 'hardened'
    $hardenedArgs = $commonArgs + @('--fork', $baselineSession, '--no-tools', '--exclude-tools', '*', '--session-dir', $hardenedSessionDir) + @($probePrompt)
    $hardenedRun = Invoke-PiPhase -Name 'hardened' -Arguments $hardenedArgs -CaptureFile $hardenedCapture

    Add-Check 'hardened-exit-zero' 'harness' ($hardenedRun.ExitCode -eq 0) ('exit=' + $hardenedRun.ExitCode + ' timedOut=' + $hardenedRun.TimedOut)
    Add-Check 'hardened-reply-token' 'harness' ($hardenedRun.Stdout -match [regex]::Escape($policyReply)) 'stdout must contain the fixed provider token'
    $hardenedSession = Get-PhaseSessionFile -Name 'hardened'
    Add-Check 'hardened-session-file' 'harness' ($null -ne $hardenedSession) 'exactly one .jsonl expected in the hardened session dir'

    $hardenedHeader = $null
    if ($hardenedSession) { $hardenedHeader = Read-SessionHeader -Path $hardenedSession }
    if ($hardenedHeader -and $baselineHeader) {
        Add-Check 'hardened-is-fork-of-baseline' 'harness' (
            ([string]$hardenedHeader.parentSession).Equals([string]$baselineSession, [System.StringComparison]::OrdinalIgnoreCase) -and
            ([string]$hardenedHeader.id) -ne ([string]$baselineHeader.id)
        ) ('parentSession=' + $hardenedHeader.parentSession)
    } else {
        Add-Check 'hardened-is-fork-of-baseline' 'harness' $false 'missing hardened or baseline session header'
    }

    $hardenedCaptureData = Read-Capture -Path $hardenedCapture
    $hardenedRequests = @(Get-CaptureRequests -Capture $hardenedCaptureData)
    Add-Check 'hardened-capture-file' 'harness' ($hardenedRequests.Count -gt 0) ('capture missing or unreadable: ' + $hardenedCapture)
    $hardenedTools = @(Get-CaptureToolUnion -Requests $hardenedRequests)

    # The actual M1-P assertion: a tool-bearing history resumed with a hard
    # zero-tool loadout must never declare a tool to the model, on any request.
    # Evaluated only when a capture exists; a missing capture is a harness
    # failure, never a pass.
    if ($hardenedRequests.Count -gt 0) {
        Add-Check 'hardened-declares-no-tools' 'policy' ($hardenedTools.Count -eq 0) ('hardened tools: [' + ($hardenedTools -join ', ') + '] over ' + $hardenedRequests.Count + ' request(s)')
    }

    $phaseRecords['hardened'] = [ordered]@{
        exitCode       = $hardenedRun.ExitCode
        timedOut       = $hardenedRun.TimedOut
        sessionDir     = $hardenedSessionDir
        sessionFile    = $hardenedSession
        sessionId      = if ($hardenedHeader) { $hardenedHeader.id } else { $null }
        parentSession  = if ($hardenedHeader) { $hardenedHeader.parentSession } else { $null }
        captureFile    = $hardenedCapture
        requestCount   = $hardenedRequests.Count
        requests       = @(Get-RequestSummaries -Requests $hardenedRequests)
        toolCount      = $hardenedTools.Count
        toolNames      = $hardenedTools
        replyTokenSeen = [bool]($hardenedRun.Stdout -match [regex]::Escape($policyReply))
        stdoutFile     = $hardenedRun.StdoutFile
        stderrFile     = $hardenedRun.StderrFile
        arguments      = $hardenedRun.Arguments
    }

    # --- Phase 3: sensitivity control, forked from the ORIGINAL baseline ----
    # Deliberately forks $baselineSession, not the hardened session: the
    # instrument must reproduce "read" from an untouched source.
    $controlCapture = Join-Path $captureDir 'control.json'
    $controlSessionDir = Join-Path $sessionBase 'control'
    $controlArgs = $commonArgs + @('--fork', $baselineSession, '--session-dir', $controlSessionDir) + @($probePrompt)
    $controlRun = Invoke-PiPhase -Name 'control' -Arguments $controlArgs -CaptureFile $controlCapture

    Add-Check 'control-exit-zero' 'harness' ($controlRun.ExitCode -eq 0) ('exit=' + $controlRun.ExitCode + ' timedOut=' + $controlRun.TimedOut)
    Add-Check 'control-reply-token' 'harness' ($controlRun.Stdout -match [regex]::Escape($policyReply)) 'stdout must contain the fixed provider token'
    $controlSession = Get-PhaseSessionFile -Name 'control'
    Add-Check 'control-session-file' 'harness' ($null -ne $controlSession) 'exactly one .jsonl expected in the control session dir'

    $controlHeader = $null
    if ($controlSession) { $controlHeader = Read-SessionHeader -Path $controlSession }
    if ($controlHeader) {
        Add-Check 'control-is-fork-of-baseline' 'harness' (
            ([string]$controlHeader.parentSession).Equals([string]$baselineSession, [System.StringComparison]::OrdinalIgnoreCase)
        ) ('parentSession=' + $controlHeader.parentSession + ' (must be the original baseline session)')
    } else {
        Add-Check 'control-is-fork-of-baseline' 'harness' $false 'missing control session header'
    }

    $controlCaptureData = Read-Capture -Path $controlCapture
    $controlRequests = @(Get-CaptureRequests -Capture $controlCaptureData)
    Add-Check 'control-capture-file' 'harness' ($controlRequests.Count -gt 0) ('capture missing or unreadable: ' + $controlCapture)
    $controlTools = @(Get-CaptureToolUnion -Requests $controlRequests)

    # Sensitivity control: the UNTOUCHED baseline must still restore exactly the
    # recorded loadout ([read]). A default tool set here would mean the session
    # never recorded a loadout, which would make the hardened emptiness trivial.
    $controlExact = ($controlTools.Count -eq 1) -and ($controlTools[0] -eq 'read')
    Add-Check 'control-restores-recorded-loadout' 'harness' $controlExact ('control tools: [' + ($controlTools -join ', ') + '] (expected exactly [read], the baseline recorded loadout)')

    $phaseRecords['control'] = [ordered]@{
        exitCode       = $controlRun.ExitCode
        timedOut       = $controlRun.TimedOut
        sessionDir     = $controlSessionDir
        sessionFile    = $controlSession
        sessionId      = if ($controlHeader) { $controlHeader.id } else { $null }
        parentSession  = if ($controlHeader) { $controlHeader.parentSession } else { $null }
        captureFile    = $controlCapture
        requestCount   = $controlRequests.Count
        requests       = @(Get-RequestSummaries -Requests $controlRequests)
        toolCount      = $controlTools.Count
        toolNames      = $controlTools
        replyTokenSeen = [bool]($controlRun.Stdout -match [regex]::Escape($policyReply))
        stdoutFile     = $controlRun.StdoutFile
        stderrFile     = $controlRun.StderrFile
        arguments      = $controlRun.Arguments
    }

    # --- Baseline session must be byte-identical after both forks -----------
    $baselineHashAfter = (Get-FileHash -LiteralPath $baselineSession -Algorithm SHA256).Hash
    Add-Check 'baseline-session-unmodified' 'harness' ($baselineHashBefore -eq $baselineHashAfter) 'forking must not rewrite the source session'
} else {
    Add-Check 'hardened-skipped' 'harness' $false 'baseline session missing; hardened/control phases could not run'
}

# Every artifact this run depends on must live inside the fixture directory.
$fixturePaths = @()
foreach ($record in $phaseRecords.Values) {
    if ($record.sessionDir) { $fixturePaths += [string]$record.sessionDir }
    if ($record.sessionFile) { $fixturePaths += [string]$record.sessionFile }
    if ($record.captureFile) { $fixturePaths += [string]$record.captureFile }
}
$outsideFixture = @($fixturePaths | Where-Object { -not $_.StartsWith($fixtureRoot, [System.StringComparison]::OrdinalIgnoreCase) })
Add-Check 'paths-inside-fixture' 'harness' ($outsideFixture.Count -eq 0) ('outside fixture: ' + ($outsideFixture -join '; '))

# ---------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------

$harnessFailures = @($checks.ToArray() | Where-Object { -not $_.ok -and $_.class -eq 'harness' })
$policyFailures = @($checks.ToArray() | Where-Object { -not $_.ok -and $_.class -eq 'policy' })

if ($harnessFailures.Count -gt 0) {
    $classification = 'harness-invalid'
    $exitCode = 11
} elseif ($policyFailures.Count -gt 0) {
    $classification = 'policy-failure'
    $exitCode = 10
} else {
    $classification = 'pass'
    $exitCode = 0
}

$failedChecks = @($checks.ToArray() | Where-Object { -not $_.ok })

$result = [ordered]@{
    schemaVersion           = 1
    runId                   = $RunId
    startedAt               = $startedAt
    finishedAt              = (Get-Date).ToString('o')
    piVersion               = $piVersion
    piRelease               = $piRelease
    piEntry                 = $piEntry
    nodeExe                 = $NodeExe
    extension               = $extensionPath
    fixtureRoot             = $fixtureRoot
    provider                = $policyProviderId
    model                   = $policyModelId
    captureEnv              = $captureEnvName
    replyToken              = $policyReply
    baselineToolNames       = @($baselineTools)
    hardenedToolNames       = @($hardenedTools)
    controlToolNames        = @($controlTools)
    toolSource              = 'getCurrentTools(context.messages) in the installed faux provider response factory'
    network                 = 'none (faux provider, PI_OFFLINE=1, --offline)'
    environment             = [ordered]@{
        privateAgentDir        = $agentDir
        fixtureCwd             = $fixtureCwd
        strippedVariableCount  = $strippedCount
        strippedNamePattern    = $stripPattern
        note                   = 'parent environment copied without credential-shaped names and without inherited PI_* state; no env values are logged'
    }
    baselineSessionSha256   = [ordered]@{ before = $baselineHashBefore; after = $baselineHashAfter }
    phases                  = $phaseRecords
    checks                  = $checks.ToArray()
    failures                = @($failedChecks)
    classification          = $classification
    passed                  = ($exitCode -eq 0)
}

$serializeError = $null
try {
    [System.IO.File]::WriteAllText($resultFile, ($result | ConvertTo-Json -Depth 10), $utf8NoBom)
} catch {
    $serializeError = $_.Exception.Message
}

if ($serializeError) {
    # A result.json is mandatory; fall back to the minimal harness-invalid record.
    $serializeMessage = 'failed to write result.json: ' + $serializeError
    [Console]::Error.WriteLine('check-pi-policy: harness-invalid (result serialization failed)')
    [Console]::Error.WriteLine('  ! ' + $serializeMessage)
    if (Write-FallbackResult -Message $serializeMessage) {
        [Console]::Error.WriteLine('result: ' + $resultFile)
    }
    exit 11
}

Write-Output ('check-pi-policy: ' + $classification)
Write-Output ('pi ' + $piVersion + ' | baseline=[' + ($baselineTools -join ', ') + '] hardened=[' + ($hardenedTools -join ', ') + '] control=[' + ($controlTools -join ', ') + ']')
foreach ($failure in $failedChecks) {
    Write-Output ('  ! [' + $failure.class + '] ' + $failure.name + ': ' + $failure.detail)
}
Write-Output ('result: ' + $resultFile)

exit $exitCode
