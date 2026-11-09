<#
.SYNOPSIS
    Signs the locally built widget binaries with a per-user self-signed
    code-signing certificate.

.DESCRIPTION
    Windows 11 Smart App Control can block freshly built, unsigned exes with
    "An Application Control policy has blocked this file". Signing the build
    with a local certificate is enough to let it run on this machine (the
    certificate chain stays untrusted, so Get-AuthenticodeSignature reports
    UnknownError - that is expected).

    The certificate is created in Cert:\CurrentUser\My on first use and reused
    afterwards. No admin rights are required. Nothing is uploaded.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\sign-local.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\sign-local.ps1 -Path build\bin\server.exe

.NOTES
    build.bat rewrites the exes, so re-run this after every build.
#>
param(
    [string[]]$Path = @(),
    [string]$Subject = 'CN=Win11 Desktop Widgets (Local)',
    [switch]$NewCertificate
)

$ErrorActionPreference = 'Stop'
# Use the module shipped with this PowerShell version; inherited PSModulePath
# may contain modules for a different version when called from cmd.exe.
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1')

$root = Split-Path -Parent $PSScriptRoot

if (-not $Path -or $Path.Count -eq 0) {
    $Path = @(
        (Join-Path $root 'build\bin\*.exe'),
        (Join-Path $root 'build\*-test.exe')
    )
}

$files = @()
foreach ($candidate in $Path) {
    $files += Get-ChildItem -Path $candidate -File -ErrorAction SilentlyContinue
}
$files = @($files | Sort-Object FullName -Unique)

if ($files.Count -eq 0) {
    throw 'no binaries found; run build.bat first'
}

$cert = $null
if (-not $NewCertificate) {
    $cert = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object {
            $_.Subject -eq $Subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date)
        } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1
}

if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject `
        -KeyUsage DigitalSignature -CertStoreLocation Cert:\CurrentUser\My `
        -HashAlgorithm SHA256 -KeyLength 2048 -NotAfter (Get-Date).AddYears(3)
    Write-Host "[sign] created certificate $($cert.Thumbprint)"
}

$failed = 0
foreach ($file in $files) {
    $target = $file.FullName
    $temp = "$target.signing.tmp"
    try {
        # Sign a private copy and swap it in, so a launch can never observe a
        # half-written signature (Windows reports 0xc0000142 in that case).
        Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue
        Copy-Item -LiteralPath $target -Destination $temp -Force
        $result = Set-AuthenticodeSignature -FilePath $temp -Certificate $cert `
            -HashAlgorithm SHA256 -Confirm:$false -ErrorAction Stop
        $check = Get-AuthenticodeSignature $temp
        if (-not $check.SignerCertificate) {
            throw "signature missing ($($result.Status))"
        }
        Move-Item -LiteralPath $temp -Destination $target -Force
        Write-Host ("[sign] {0}  ({1})" -f $file.Name, $check.Status)
    } catch {
        ++$failed
        Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue
        Write-Warning ("[sign] {0}: {1}" -f $file.Name, $_.Exception.Message)
    }
}

if ($failed -gt 0) { exit 1 }

Write-Host "[sign] done: $($cert.Subject), thumbprint $($cert.Thumbprint)"
Write-Host '[sign] UnknownError is expected for a self-signed chain; Smart App Control accepts it.'
Write-Host '[sign] re-run after build.bat because linking rewrites the exes.'
