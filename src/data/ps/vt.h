// A VT/xterm sequence parser: decoded text in, screen mutations out.
//
// Scope is "what a TUI needs": cursor addressing, erase, scroll regions, SGR
// colours (16 / 256 / truecolor), the alternate screen and the handful of modes
// Windows Terminal-style apps set. Unknown sequences are swallowed.
#pragma once

#include "data/ps/screen.h"

#include <string>
#include <string_view>
#include <vector>

namespace ps {

class Parser {
public:
    void Feed(std::wstring_view text, Screen& screen);
    void Reset();

private:
    enum class State { Ground, Escape, EscapeIntermediate, Csi, Osc, Dcs };

    void Ground(wchar_t c, Screen& s);
    void Escape(wchar_t c, Screen& s);
    void Csi(wchar_t c, Screen& s);
    void ExecuteCsi(Screen& s);
    void ExecuteSgr(Screen& s);
    void SetModes(Screen& s, bool on);
    int Param(std::size_t index, int fallback) const;
    int Param1(std::size_t index) const;   // 1-based params, 0 means 1
    void StartCsi();

    State state_ = State::Ground;
    std::vector<int> params_;
    int current_ = -1;
    bool private_ = false;
    wchar_t intermediate_ = 0;
    wchar_t final_ = 0;
    bool oscEscape_ = false;   // saw ESC inside an OSC, waiting for '\'
};

} // namespace ps
