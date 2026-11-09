<#
.SYNOPSIS
    Install desk and its existing configuration into a normal user directory.
.DESCRIPTION
    Run this script from a normal PowerShell session, not a restricted or low
    integrity session. Existing configuration at the destination is retained.
    The application is not started and Windows startup settings are not changed.
#>
param(
    [string]$SourceDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\bin'),
    [string]$Destination = (Join-Path $env:LOCALAPPDATA 'Programs\Desktop Desk')
)

$ErrorActionPreference = 'Stop'

# Creating files from a low integrity process can produce another unusable
# installation. Detect this before touching the destination.
$groups = & "$env:SystemRoot\System32\whoami.exe" /groups /fo csv /nh
if ($LASTEXITCODE -ne 0) { throw 'Cannot determine process integrity level.' }
$integrity = @($groups | Select-String -Pattern 'S-1-16-(\d+)' | ForEach-Object {
    [int]$_.Matches[0].Groups[1].Value
})
if ($integrity.Count -ne 1 -or $integrity[0] -lt 8192) {
    throw 'Open a normal PowerShell window outside the sandbox and run this script again (medium integrity or higher is required).'
}

$sourceExe = Join-Path $SourceDir 'desk.exe'
if (-not (Test-Path -LiteralPath $sourceExe -PathType Leaf)) {
    throw "desk.exe not found: $sourceExe. Build the project first."
}
$sourceFull = [System.IO.Path]::GetFullPath($SourceDir).TrimEnd('\')
$destFull = [System.IO.Path]::GetFullPath($Destination).TrimEnd('\')
if ($sourceFull -ieq $destFull) { throw 'Source and destination must be different directories.' }

New-Item -ItemType Directory -Path $destFull -Force | Out-Null

# Write new files in the destination instead of copying source ACLs: the
# working tree may have an inherited low-integrity label. Preserve file bytes,
# including the executable signature. Config files are copied only once.
function Write-NewFile([string]$From, [string]$To) {
    $inputStream = [System.IO.File]::Open($From, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        $outputStream = [System.IO.File]::Open($To, [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
        try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose() }
    } finally { $inputStream.Dispose() }
}

$targetExe = Join-Path $destFull 'desk.exe'
$stagedExe = Join-Path $destFull ('.desk-' + [guid]::NewGuid().ToString('N') + '.tmp')
$backupExe = $stagedExe + '.bak'
try {
    Write-NewFile $sourceExe $stagedExe
    if (Test-Path -LiteralPath $targetExe) {
        [System.IO.File]::Replace($stagedExe, $targetExe, $backupExe)
        # Windows can keep the previous image locked while desk is running.
        # The replacement has succeeded; leave that backup for later cleanup.
        try {
            Remove-Item -LiteralPath $backupExe -Force -ErrorAction Stop
        } catch {
            Write-Warning "The old executable is still in use; delete $backupExe after exiting the old desk instance."
        }
    } else {
        [System.IO.File]::Move($stagedExe, $targetExe)
    }
} finally {
    if (Test-Path -LiteralPath $stagedExe) { Remove-Item -LiteralPath $stagedExe -Force }
}

foreach ($name in @('desk.ini', 'aimeter.ini', 'server.ini', 'ps.ini', 'ps-history.txt')) {
    $source = Join-Path $sourceFull $name
    $target = Join-Path $destFull $name
    if ((Test-Path -LiteralPath $source -PathType Leaf) -and
        -not (Test-Path -LiteralPath $target)) {
        Write-NewFile $source $target
        Write-Host "[install] copied $name"
    }
}

Write-Host "[install] installed $targetExe"
Write-Host '[install] Exit the old desk instance, then start this executable from Explorer.'
