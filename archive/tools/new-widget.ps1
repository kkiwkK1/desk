<#
.SYNOPSIS
    Creates a new desktop widget from the framework template.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\new-widget.ps1 -Name weather -Title "Weather"

.NOTES
    The generated widget is picked up automatically by build.bat.
#>
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidatePattern('^[a-z][a-z0-9]*(-[a-z0-9]+)*$')]
    [string]$Name,

    [string]$Title = ''
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root "src\widgets\$Name"
$file = Join-Path $dir "$Name.cpp"

if (Test-Path $file) { throw "widget already exists: $file" }
if (-not $Title) { $Title = $Name }

$class = (($Name -split '[-_]') | ForEach-Object {
    if ($_.Length -gt 0) { $_.Substring(0, 1).ToUpper() + $_.Substring(1) }
}) -join ''

$template = @'
// @TITLE@ - a desktop widget built on src/widget.
//
// Contract: LoadData -> RefreshData (worker thread) -> PaintData (UI thread).
// RefreshData may block; it can never touch the window.
#include "widget/host.h"

#include <cwchar>
#include <memory>
#include <string>

namespace {

struct @CLASS@Data {
    std::wstring headline = L"HELLO";
    std::wstring detail = L"EDIT ME";
};

class @CLASS@ : public wg::WidgetT<@CLASS@Data> {
public:
    wg::Desc Describe() override { return wg::Desc{L"@NAME@", L"@TITLE@", 280}; }

protected:
    void LoadData(@CLASS@Data& data) override {
        (void)data;  // read store() / wg::LoadSecret() here (UI thread, once)
    }

    void RefreshData(@CLASS@Data& data) override {
        // Worker thread: HTTP, file scans, anything slow goes here.
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t clock[32];
        swprintf(clock, 32, L"%02d:%02d:%02d", now.wHour, now.wMinute, now.wSecond);
        data.detail = clock;
    }

    int HeightOf(const @CLASS@Data&) const override { return 78; }

    void PaintData(const @CLASS@Data& data, wg::Canvas& canvas) override {
        canvas.Panel(theme_, editing());
        canvas.TextCentered(data.headline, canvas.width() / 2, 12.0f, theme_.text, 3.0f);
        canvas.TextCentered(data.detail, canvas.width() / 2, 52.0f, theme_.faint, 1.0f);
    }

private:
    wg::Theme theme_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wg::Options options;
    options.refreshSeconds = 5;
    return wg::Run(std::make_unique<@CLASS@>(), options);
}
'@

$code = $template.Replace('@NAME@', $Name).Replace('@CLASS@', $class).Replace('@TITLE@', $Title)

New-Item -ItemType Directory -Force -Path $dir | Out-Null
[IO.File]::WriteAllText($file, $code, [Text.UTF8Encoding]::new($false))

Write-Host "[new-widget] $file"
Write-Host "[new-widget] next:"
Write-Host "  cmd /c build.bat"
Write-Host "  build\bin\$Name.exe"
