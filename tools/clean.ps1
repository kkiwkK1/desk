# removes build artifacts that are safe to regenerate.
#
# usage:
#   tools\clean.ps1                 # list what -Artifacts would delete
#   tools\clean.ps1 -Artifacts -Force
#   tools\clean.ps1 -All -Force     # also bin/obj/sdk (full rebuild needed)
param(
    [switch]$Artifacts,
    [switch]$All,
    [switch]$Force,
    [string]$Root = ""
)

$ErrorActionPreference = "Stop"
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }
$Root = (Resolve-Path $Root).Path
$build = Join-Path $Root "build"
if (-not (Test-Path $build)) { Write-Host "clean: no build directory"; exit 0 }

# named directories are regenerated; qa/ is kept (evidence), bin/obj/sdk only with -All.
$names = @("testrun", "gh", "tmp", "sdk-consumer-check")
$patterns = @("sdk-consumer-final-*", "plan-independent-check", "*.old", "*.exe.old*")

$targets = @()
foreach ($n in $names) {
    $p = Join-Path $build $n
    if (Test-Path $p) { $targets += (Get-Item $p) }
}
foreach ($pat in $patterns) {
    Get-ChildItem $build -Filter $pat -Force -ErrorAction SilentlyContinue | ForEach-Object { $targets += $_ }
}
if ($All) {
    foreach ($n in @("bin", "obj", "sdk")) {
        $p = Join-Path $build $n
        if (Test-Path $p) { $targets += (Get-Item $p) }
    }
}

if ($targets.Count -eq 0) { Write-Host "clean: nothing to remove"; exit 0 }

$total = 0
foreach ($t in $targets) {
    $size = 0
    if ($t.PSIsContainer) {
        $size = (Get-ChildItem $t.FullName -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    } else {
        $size = $t.Length
    }
    $total += $size
    $rel = $t.FullName.Substring($Root.Length + 1)
    Write-Host ("{0,10:N1} MB  {1}" -f ($size / 1MB), $rel)
}
Write-Host ("total: {0:N1} MB" -f ($total / 1MB))

if (-not $Force) {
    Write-Host "dry run: pass -Force to delete"
    exit 0
}

foreach ($t in $targets) {
    try {
        Remove-Item $t.FullName -Recurse -Force -ErrorAction Stop
    } catch {
        Write-Host ("clean: cannot remove " + $t.FullName + ": " + $_.Exception.Message)
    }
}
Write-Host "clean: done"
