// Cross-widget placement: keep desktop widgets from covering each other.
//
// Widgets discover each other through their window classes ("wg.<id>"), so no
// shared config file is involved and a crashed widget leaves no stale entry.
// Placement is serialized with a session-local mutex so two widgets starting at
// the same time cannot pick the same empty slot.
#pragma once

#include "widget/geom.h"

#include <windows.h>

#include <vector>

namespace wg {

// Position for a w x h window near (x, y) that keeps at least `gap` pixels from
// every occupied rect and stays inside `work`. Same-row left/right placement is
// preferred, then above/below, then a bottom-right grid. Returns the clamped
// requested position when nothing fits.
POINT FindFreeSpot(int x, int y, int w, int h, const std::vector<Rect>& occupied,
                   const RECT& work, int gap = 8);

// Holds "Local\wgWidgetLayout" while a widget enumerates and repositions itself.
// If the mutex cannot be acquired the caller still proceeds (best effort).
class LayoutGuard {
public:
    LayoutGuard();
    ~LayoutGuard();

    LayoutGuard(const LayoutGuard&) = delete;
    LayoutGuard& operator=(const LayoutGuard&) = delete;

private:
    HANDLE mutex_ = nullptr;
    bool owned_ = false;
};

// Main windows of the other widgets on this desktop, in screen coordinates.
// Details panels ("wg.<id>.details") and the prompt ("wg.Prompt") are ignored.
std::vector<Rect> OtherWidgetRects(HWND self);

} // namespace wg
