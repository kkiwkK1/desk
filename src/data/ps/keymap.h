// Turning Windows keyboard input into the bytes a VT application expects.
//
// The card no longer edits a line itself: PSReadLine (or vim, or pi) does, and
// it needs the real control bytes - arrows, Home/End, Ctrl+letter, Alt prefixes.
#pragma once

#include "widget/module.h"

#include <string>
#include <string_view>

namespace ps {

struct KeyMode {
    bool appCursor = false;   // DECCKM: arrows as SS3 instead of CSI
};

// ConPTY can suppress application mouse-mode sequences. Auto never assumes
// mouse support unless a trusted hint is supplied; the other modes let a user
// explicitly choose history or send SGR to the current application.
enum class WheelMode { Auto, History, Application };
enum class WheelRoute { None, History, LegacyMouse, SgrMouse };
WheelMode ParseWheelMode(std::wstring_view value);
const wchar_t* WheelModeName(WheelMode mode);
WheelRoute ChooseWheelRoute(WheelMode mode, bool mouseTracking, bool sgrMouse,
                            bool alternate, bool piFullScreen);

// Preserve fractional high-resolution wheel deltas between Windows messages.
class WheelAccumulator {
public:
    int Add(int delta) {
        remainder_ += delta;
        const int steps = remainder_ / 120;
        remainder_ %= 120;
        return steps;
    }
    void Reset() { remainder_ = 0; }
private:
    int remainder_ = 0;
};

// One wheel notch encoded as an xterm mouse press. Coordinates are 1-based.
std::string EncodeWheel(bool up, int col, int row, bool sgr,
                        bool shift = false, bool alt = false, bool ctrl = false);

// One keystroke, as bytes. Empty when the key has no VT meaning.
std::string EncodeKey(const wg::KeyEvent& key, const KeyMode& mode);

// Clipboard text for the PTY. With bracketed paste the app gets the 200~/201~
// wrapper so it can tell a paste from typing; newlines become CR either way.
std::string EncodePaste(std::wstring_view text, bool bracketed);

} // namespace ps
