<#
.SYNOPSIS
    Serializes builds across parallel agents.

.DESCRIPTION
    Several agents share this working tree. build.bat writes shared object
    files, so two concurrent builds would corrupt each other. This wrapper
    takes a session-local named mutex, runs build.bat, and releases the mutex.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-lock.ps1 -Target test
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-lock.ps1
#>
param(
    [ValidateSet('framework', 'example', 'test', 'all')]
    [string]$Target = 'all',
    [int]$TimeoutSeconds = 1200
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$mutex = New-Object System.Threading.Mutex($false, 'Local\wg-build-lock')

$acquired = $false
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
try {
    while (-not $acquired) {
        try {
            $acquired = $mutex.WaitOne(5000)
        } catch [System.Threading.AbandonedMutexException] {
            # A previous build holder died; the mutex is ours now.
            $acquired = $true
        }
        if (-not $acquired -and (Get-Date) -gt $deadline) {
            throw "timed out waiting for the build lock after $TimeoutSeconds s"
        }
    }

    Write-Host "[build-lock] acquired; running build.bat $Target"
    Push-Location $root
    try {
        if ($Target -eq 'all') {
            & cmd /c "build.bat"
        } else {
            & cmd /c "build.bat $Target"
        }
        $code = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    exit $code
} finally {
    if ($acquired) {
        try { $mutex.ReleaseMutex() } catch { }
    }
    $mutex.Dispose()
}
