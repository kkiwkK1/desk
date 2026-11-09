# checks that every source file is either built or intentionally excluded,
# and every framework header is referenced somewhere.
#
# usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\check-sources.ps1 [-Root <repo>]
param([string]$Root = "")

$ErrorActionPreference = "Stop"
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }
$Root = (Resolve-Path $Root).Path
$buildBat = Join-Path $Root "build.bat"
if (-not (Test-Path $buildBat)) { Write-Host "check-sources: build.bat not found"; exit 2 }

$buildText = Get-Content $buildBat -Raw
$problems = 0

$globbed = @(
    "src\apps\desk\*.cpp",
    "src\panels\*\*.cpp",
    "src\cli\*.cpp"
)
$isGlobbed = {
    param([string]$rel)
    return ($rel -like "src\apps\desk\*.cpp") -or
           ($rel -like "src\panels\*\*.cpp") -or
           ($rel -like "src\cli\*.cpp")
}

$cppFiles = Get-ChildItem (Join-Path $Root "src") -Recurse -File -Filter *.cpp
foreach ($f in $cppFiles) {
    $rel = $f.FullName.Substring($Root.Length + 1)
    $name = $f.BaseName
    if ($isGlobbed.Invoke($rel)) { continue }
    if ($buildText -match ('(?<![A-Za-z0-9_])' + [regex]::Escape($name) + '(?![A-Za-z0-9_])')) { continue }
    Write-Host ("DEAD-CPP   " + $rel)
    $problems++
}

$headerFiles = Get-ChildItem (Join-Path $Root "src") -Recurse -File -Filter *.h
$allSources = Get-ChildItem (Join-Path $Root "src") -Recurse -File -Include *.cpp, *.h | Get-Content -Raw
foreach ($h in $headerFiles) {
    $rel = $h.FullName.Substring($Root.Length + 1)
    $name = $h.Name
    $pattern = [regex]::Escape($name)
    $hits = 0
    foreach ($s in $allSources) {
        if ($s -match $pattern) { $hits++ }
    }
    if ($hits -eq 0) {  # no other source mentions the header name
        Write-Host ("UNREF-H    " + $rel)
        $problems++
    }
}

if ($problems -gt 0) {
    Write-Host ("check-sources: " + $problems + " problem(s)")
    exit 1
}
Write-Host "check-sources: ok"
