// wg::Run - turns a Widget into a desktop program.
//
// Host responsibilities: single instance, layered always-on-desktop window,
// tray icon, global hotkeys, drag/edit mode, position + widget state
// persistence, autostart, refresh timer and the worker thread.
#pragma once

#include "widget/widget.h"

#include <memory>

namespace wg {

struct Options {
    int  refreshSeconds = 300;                 // 0 disables the periodic refresh
    bool startWithWindows = true;              // applied on first run only
    bool clickThrough = true;                  // pass mouse input to the desktop
    bool focusable = false;                    // click to focus; route keys to the widget
    UINT editHotkeyMods = MOD_CONTROL | MOD_ALT;
    UINT editHotkeyVk = 'W';
    UINT quitHotkeyMods = MOD_CONTROL | MOD_ALT;
    UINT quitHotkeyVk = 'Q';
    UINT detailsHotkeyMods = MOD_CONTROL | MOD_ALT;  // only when Widget::HasDetails()
    UINT detailsHotkeyVk = 'D';
};

// Runs the message loop until the user exits. Returns the process exit code.
int Run(std::unique_ptr<Widget> widget, Options options = {});

} // namespace wg
