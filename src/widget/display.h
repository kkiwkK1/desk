// Display/work-area/DPI source used by the board.
//
// The board never calls SystemParametersInfo / GetMonitorInfo / GetDpiForWindow
// directly any more: it asks a DisplaySource. The system implementation is the
// default, and a scripted implementation lets the test harness simulate
// monitor/work-area/DPI changes on a single physical display.
#pragma once

#include "widget/geom.h"

#include <windows.h>

#include <memory>
#include <string>

namespace wg {

class DisplaySource {
public:
    virtual ~DisplaySource() = default;

    // Work area of the primary monitor, in screen pixels.
    virtual RECT PrimaryWorkArea() const = 0;
    // Work area of the monitor containing (or nearest to) a screen point.
    virtual RECT WorkAreaForPoint(POINT screenPoint) const = 0;
    // DPI of the monitor that hosts the window. The window may be null before
    // the board window exists; implementations then return SystemDpi().
    virtual int DpiForWindow(HWND window) const = 0;
    // System DPI, usable before any window exists.
    virtual int SystemDpi() const = 0;
};

// Reads the live desktop through the Windows APIs.
std::shared_ptr<DisplaySource> CreateSystemDisplaySource();

// Test-only source driven by a small ASCII file that is re-read on every query:
//   work=x,y,w,h    primary work area (required for the source to be usable)
//   dpi=N           DPI returned by SystemDpi()/DpiForWindow()
//   rect=x,y,w,h    optional suggested rectangle for the DPI-change path
// Invalid lines are ignored; a missing file keeps the last valid values.
class ScriptedDisplaySource : public DisplaySource {
public:
    static std::shared_ptr<ScriptedDisplaySource> Create(const std::wstring& file);
    // Suggested rectangle from the latest rect= line, if any.
    virtual bool SuggestedRect(RECT* out) const = 0;
};

} // namespace wg
