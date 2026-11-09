<#
.SYNOPSIS
    Samples working set, private bytes, threads, GDI/USER objects and idle CPU
    for one or more processes.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\bench-desktop.ps1 -ProcessName desk -Seconds 10

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\bench-desktop.ps1 -ProcessName aimeter,server
#>
param(
    [string[]]$ProcessName = @('desk'),
    [int]$Seconds = 10
)

if ($Seconds -lt 1) { $Seconds = 1 }

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class BenchNative {
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr h, uint flags);
}
"@

$rows = @()
foreach ($name in $ProcessName) {
    $procs = Get-Process -Name $name -ErrorAction SilentlyContinue
    if (-not $procs) {
        Write-Host "[bench] no process named $name"
        continue
    }
    foreach ($p in $procs) {
        $proc = [System.Diagnostics.Process]::GetProcessById($p.Id)
        $cpu0 = $proc.TotalProcessorTime.TotalMilliseconds
        Start-Sleep -Seconds $Seconds
        $proc.Refresh()
        $cpu1 = $proc.TotalProcessorTime.TotalMilliseconds
        $delta = $cpu1 - $cpu0

        $gdi = $null
        $usr = $null
        $h = [BenchNative]::OpenProcess(0x0400, $false, [uint32]$p.Id)
        if ($h -ne [IntPtr]::Zero) {
            $gdi = [BenchNative]::GetGuiResources($h, 0)
            $usr = [BenchNative]::GetGuiResources($h, 1)
            [void][BenchNative]::CloseHandle($h)
        }

        $rows += [pscustomobject]@{
            Name      = $p.ProcessName
            PID       = $p.Id
            'WS MB'   = [math]::Round($p.WorkingSet64 / 1MB, 1)
            'Private MB' = [math]::Round($p.PrivateMemorySize64 / 1MB, 1)
            Threads   = $p.Threads.Count
            GDI       = $gdi
            USER      = $usr
            'CPU ms'  = [math]::Round($delta, 1)
            'CPU %'   = [math]::Round(100.0 * $delta / ($Seconds * 1000.0), 1)
        }
    }
}

if ($rows.Count -eq 0) {
    Write-Host '[bench] nothing measured'
    exit 1
}

$rows | Format-Table -AutoSize
Write-Host ("[bench] sampled over {0}s; CPU % is one core" -f $Seconds)
