<#
.SYNOPSIS
    Creates a new desktop-board module from the framework template.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\new-panel.ps1 -Name weather -Title WEATHER

.NOTES
    build.bat discovers src\panels\* automatically. Add the factory line printed
    below to src\apps\desk\main.cpp.
#>
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidatePattern('^[a-z][a-z0-9]*(-[a-z0-9]+)*$')]
    [string]$Name,

    [string]$Title = ''
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root "src\panels\$Name"
$cpp = Join-Path $dir "$Name.cpp"
$hdr = Join-Path $dir "$Name.h"

if (Test-Path $cpp) { throw "module already exists: $cpp" }
if (-not $Title) { $Title = $Name.ToUpperInvariant() }

$class = (($Name -split '[-_]') | ForEach-Object {
    if ($_.Length -gt 0) { $_.Substring(0, 1).ToUpper() + $_.Substring(1) }
}) -join ''

$header = @"
#pragma once

#include "widget/module.h"

#include <memory>

std::unique_ptr<wg::Module> Create@CLASS@Module();
"@

$source = @"
// @TITLE@ - a desktop-board module.
//
// Contract: LoadData -> RefreshData (worker thread) -> PaintBodyOf (UI thread).
// RefreshData may block; it can never touch the window or Canvas.
//
// Sizing (see docs/framework.md, "Component size and position"): sizePreset
// picks the width.
// Standard is 340px wide with a 112..640px outer-height range. BodyHeightOf()
// declares only the content height; the framework adds wg::kCardChromeHeight
// (48px of title bar and card padding), rounds up to the 8px card step and
// floors at the preset minimum. That range is a validation boundary, never a
// clipping instruction: keep the body inside it, or the board clips the extra
// pixels. A data-driven row count must therefore be capped to what fits, with
// the remainder reported through Status().
#include "@NAME@.h"

#include "widget/size.h"
#include "widget/skin.h"

#include <cwchar>
#include <memory>
#include <string>
#include <vector>

namespace {

// Height of the inline details block drawn while the card is expanded. A named
// constant keeps BodyHeightOf() and PaintBodyOf() from drifting apart.
constexpr int kDetailsHeight = 24;

// Compile-time guard: the largest card this module can ask for must stay inside
// the Standard preset (340px wide, 112..640px outer).
static_assert(wg::kCardChromeHeight + wg::skin::kRowPitch + wg::skin::kDetailsGap +
                      kDetailsHeight <= 640,
              "generated module exceeds the Standard preset height");

struct @CLASS@Data {
    std::vector<wg::Row> rows;
};

class @CLASS@Module : public wg::ModuleT<@CLASS@Data> {
public:
    wg::ModuleSpec Describe() const override {
        wg::ModuleSpec spec;
        spec.id = L"@NAME@";
        spec.title = L"@TITLE@";
        spec.configFile = L"@NAME@.ini";
        spec.sizePreset = wg::SizePreset::Standard;   // 340px wide
        spec.refreshSeconds = 60;
        spec.hasDetails = true;
        return spec;
    }

    wg::StatusChip Status() const override {
        return wg::StatusChip{L"LIVE", theme_.ok};
    }

protected:
    void LoadData(@CLASS@Data& data) override {
        data.rows = {
            wg::Row{L"@TITLE@", L"...", L"", -1.0f, 0, L"EDIT src/panels/@NAME@/@NAME@.cpp", 0},
        };
    }

    void RefreshData(@CLASS@Data& data) override {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t clock[32];
        swprintf(clock, 32, L"%02d:%02d:%02d", now.wHour, now.wMinute, now.wSecond);
        data.rows = {
            wg::Row{L"@TITLE@", clock, L"", -1.0f, 0,
                    L"EDIT src/panels/@NAME@/@NAME@.cpp", 0},
        };
    }

    // The only place this module declares its height: rows plus, when the card
    // is expanded, the details block. PaintBodyOf() draws into exactly this
    // many pixels, and the host frames it with the preset width and step.
    int BodyHeightOf(const @CLASS@Data& data) const override {
        const int rows = wg::skin::RowsHeight((int)data.rows.size());
        return rows + (expanded() ? wg::skin::kDetailsGap + kDetailsHeight : 0);
    }

    void PaintBodyOf(const @CLASS@Data& data, wg::Canvas& canvas,
                     const wg::Rect& body) override {
        wg::skin::Rows(canvas, body, data.rows, theme_);
        if (!expanded()) return;

        const int y = body.y + wg::skin::RowsHeight((int)data.rows.size()) +
                      wg::skin::kDetailsGap;
        const int left = body.x + wg::skin::kCardPadX;
        const int right = body.right() - wg::skin::kCardPadX;
        wg::skin::Divider(canvas, left, right, y, theme_);
        canvas.Text(L"FIRST ROW", (float)left, (float)(y + 9), theme_.faint, 1.0f);
        canvas.TextRight(L"EDIT ME", right, (float)(y + 9), theme_.label, 1.0f);
    }

private:
    wg::Theme theme_;
};

} // namespace

std::unique_ptr<wg::Module> Create@CLASS@Module() {
    return std::make_unique<@CLASS@Module>();
}
"@

$header = $header.Replace('@CLASS@', $class)
$source = $source.Replace('@NAME@', $Name).Replace('@CLASS@', $class).Replace('@TITLE@', $Title)

New-Item -ItemType Directory -Force -Path $dir | Out-Null
[IO.File]::WriteAllText($hdr, $header, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($cpp, $source, [Text.UTF8Encoding]::new($false))

Write-Host "[new-panel] $cpp"
Write-Host "[new-panel] add to src\apps\desk\main.cpp:"
Write-Host "  #include `"panels/$Name/$Name.h`""
Write-Host "  modules.push_back(Create$($class)Module());"
Write-Host "[new-panel] then:"
Write-Host "  cmd /c build.bat"
Write-Host "  build\bin\desk.exe"
