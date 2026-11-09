#include "data/ps/vt.h"

#include <algorithm>

namespace ps {
namespace {

std::uint32_t Palette(std::uint32_t index) { return kColorPalette | (index & 0xFFu); }
std::uint32_t Rgb(std::uint32_t r, std::uint32_t g, std::uint32_t b) {
    return kColorRgb | ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu);
}

} // namespace

void Parser::Reset() {
    state_ = State::Ground;
    params_.clear();
    current_ = -1;
    private_ = false;
    intermediate_ = 0;
    oscEscape_ = false;
}

void Parser::StartCsi() {
    state_ = State::Csi;
    params_.clear();
    current_ = -1;
    private_ = false;
    intermediate_ = 0;
    final_ = 0;
}

int Parser::Param(std::size_t index, int fallback) const {
    if (index >= params_.size()) return fallback;
    return params_[index] < 0 ? fallback : params_[index];
}

void Parser::Feed(std::wstring_view text, Screen& screen) {
    for (const wchar_t c : text) {
        switch (state_) {
        case State::Ground:
            Ground(c, screen);
            break;
        case State::Escape:
            Escape(c, screen);
            break;
        case State::EscapeIntermediate:
            // ESC ( B, ESC ) 0 ...: one designation byte, then back to normal.
            state_ = State::Ground;
            break;
        case State::Csi:
            Csi(c, screen);
            break;
        case State::Osc:
            if (oscEscape_) {
                oscEscape_ = false;
                if (c == L'\\') {
                    state_ = State::Ground;
                    break;
                }
            }
            if (c == 0x07) {
                state_ = State::Ground;
            } else if (c == 0x1B) {
                oscEscape_ = true;
            }
            break;
        case State::Dcs:
            // Device control strings are ignored; ESC \ ends them.
            if (c == 0x1B) {
                state_ = State::Escape;
            }
            break;
        }
    }
}

void Parser::Ground(wchar_t c, Screen& screen) {
    switch (c) {
    case 0x1B:
        state_ = State::Escape;
        return;
    case 0x00:
    case 0x07:
        return;   // NUL / bell
    case 0x08:
        screen.Backspace();
        return;
    case 0x09:
        screen.Tab();
        return;
    case 0x0A:
    case 0x0B:
    case 0x0C:
        screen.LineFeed();
        return;
    case 0x0D:
        screen.CarriageReturn();
        return;
    case 0x0E:
    case 0x0F:
        return;   // charset shift out/in
    case 0x7F:
        return;
    default:
        break;
    }
    if (c >= 0x20) screen.Print(c);
}

void Parser::Escape(wchar_t c, Screen& screen) {
    switch (c) {
    case L'[':
        StartCsi();
        return;
    case L']':
        state_ = State::Osc;
        oscEscape_ = false;
        return;
    case L'P':
        state_ = State::Dcs;
        return;
    case L'(':
    case L')':
    case L'*':
    case L'+':
    case L'#':
    case L'%':
        state_ = State::EscapeIntermediate;
        return;
    case L'7':
        screen.SaveCursor();
        state_ = State::Ground;
        return;
    case L'8':
        screen.RestoreCursor();
        state_ = State::Ground;
        return;
    case L'D':
        screen.LineFeed();
        state_ = State::Ground;
        return;
    case L'E':
        screen.NextLine();
        state_ = State::Ground;
        return;
    case L'M':
        screen.ReverseIndex();
        state_ = State::Ground;
        return;
    case L'c': {
        // RIS: hard reset.
        screen.SetAlternateScreen(false);
        screen.EraseInDisplay(3);
        screen.ResetScrollRegion();
        screen.ResetAttr();
        screen.SetAutoWrap(true);
        screen.SetOriginMode(false);
        screen.ResetMouseModes();
        screen.SetCursorVisible(true);
        screen.MoveTo(0, 0);
        state_ = State::Ground;
        return;
    }
    case L'=':
    case L'>':
        state_ = State::Ground;
        return;
    default:
        state_ = State::Ground;
        return;
    }
}

void Parser::Csi(wchar_t c, Screen& screen) {
    if (c >= L'0' && c <= L'9') {
        current_ = (current_ < 0 ? 0 : current_) * 10 + (int)(c - L'0');
        if (current_ > 65535) current_ = 65535;
        return;
    }
    if (c == L';') {
        params_.push_back(current_ < 0 ? 0 : current_);
        current_ = -1;
        return;
    }
    if (c == L'?' || c == L'>' || c == L'<' || c == L'=') {
        private_ = (c == L'?');
        return;
    }
    if (c >= 0x20 && c <= 0x2F) {
        intermediate_ = c;
        return;
    }
    if (c >= 0x40 && c <= 0x7E) {
        params_.push_back(current_ < 0 ? 0 : current_);
        final_ = c;
        ExecuteCsi(screen);
        state_ = State::Ground;
        return;
    }
    // Anything else (control bytes inside CSI) restarts the sequence.
    state_ = State::Ground;
}

int Parser::Param1(std::size_t index) const {
    const int v = Param(index, 1);
    return v > 0 ? v : 1;
}

void Parser::SetModes(Screen& s, bool on) {
    for (const int raw : params_) {
        const int p = raw < 0 ? 0 : raw;
        if (!private_) continue;   // ANSI modes (insert, newline) are ignored
        switch (p) {
        case 1:
            s.SetApplicationCursorKeys(on);
            break;
        case 6:
            s.SetOriginMode(on);
            break;
        case 7:
            s.SetAutoWrap(on);
            break;
        case 25:
            s.SetCursorVisible(on);
            break;
        case 1000:
        case 1002:
        case 1003:
            s.SetMouseTracking(p, on);
            break;
        case 1006:
            s.SetSgrMouse(on);
            break;
        case 47:
        case 1047:
        case 1049:
            s.SetAlternateScreen(on);
            break;
        case 2004:
            s.SetBracketedPaste(on);
            break;
        default:
            break;   // mouse reporting and friends: recorded as unsupported
        }
    }
}

void Parser::ExecuteCsi(Screen& s) {
    const int n = Param1(0);
    switch (final_) {
    case L'A':
        s.MoveUp(n);
        break;
    case L'B':
        s.MoveDown(n);
        break;
    case L'C':
        s.MoveRight(n);
        break;
    case L'D':
        s.MoveLeft(n);
        break;
    case L'E':
        s.MoveDown(n);
        s.MoveToColumn(0);
        break;
    case L'F':
        s.MoveUp(n);
        s.MoveToColumn(0);
        break;
    case L'G':
        s.MoveToColumn(Param1(0) - 1);
        break;
    case L'd':
        s.MoveToRow(Param1(0) - 1);
        break;
    case L'H':
    case L'f':
        s.MoveTo(Param1(0) - 1, Param1(1) - 1);
        break;
    case L'J':
        s.EraseInDisplay(Param(0, 0));
        break;
    case L'K':
        s.EraseInLine(Param(0, 0));
        break;
    case L'L':
        s.InsertLines(n);
        break;
    case L'M':
        s.DeleteLines(n);
        break;
    case L'P':
        s.DeleteChars(n);
        break;
    case L'@':
        s.InsertChars(n);
        break;
    case L'X':
        s.EraseChars(n);
        break;
    case L'S':
        s.ScrollUp(n);
        break;
    case L'T':
        s.ScrollDown(n);
        break;
    case L'm':
        ExecuteSgr(s);
        break;
    case L'r': {
        int bottom = Param(1, 0);
        if (bottom <= 0) bottom = s.rows();
        s.SetScrollRegion(Param1(0) - 1, bottom - 1);
        break;
    }
    case L'h':
        SetModes(s, true);
        break;
    case L'l':
        SetModes(s, false);
        break;
    case L's':
        s.SaveCursor();
        break;
    case L'u':
        s.RestoreCursor();
        break;
    default:
        // 'c'/'n'/'q'/'b' and anything unknown: no effect.
        break;
    }
}
void Parser::ExecuteSgr(Screen& s) {
    Attr attr = s.attr();
    for (std::size_t i = 0; i < params_.size(); ++i) {
        const int p = params_[i] < 0 ? 0 : params_[i];
        switch (p) {
        case 0:
            attr = Attr{};
            break;
        case 1:
            attr.flags |= kFlagBold;
            attr.flags &= (std::uint8_t)~kFlagFaint;
            break;
        case 2:
            attr.flags |= kFlagFaint;
            attr.flags &= (std::uint8_t)~kFlagBold;
            break;
        case 4:
        case 21:
            attr.flags |= kFlagUnderline;
            break;
        case 7:
            attr.flags |= kFlagInverse;
            break;
        case 22:
            attr.flags &= (std::uint8_t)~(kFlagBold | kFlagFaint);
            break;
        case 24:
            attr.flags &= (std::uint8_t)~kFlagUnderline;
            break;
        case 27:
            attr.flags &= (std::uint8_t)~kFlagInverse;
            break;
        case 39:
            attr.fg = kColorDefault;
            break;
        case 49:
            attr.bg = kColorDefault;
            break;
        case 38:
        case 48: {
            const bool fg = p == 38;
            if (i + 1 >= params_.size()) break;
            const int kind = params_[i + 1];
            if (kind == 5 && i + 2 < params_.size()) {
                const std::uint32_t idx = (std::uint32_t)std::max(0, params_[i + 2]);
                if (fg) attr.fg = Palette(idx);
                else attr.bg = Palette(idx);
                i += 2;
            } else if (kind == 2 && i + 4 < params_.size()) {
                const std::uint32_t r = (std::uint32_t)std::max(0, params_[i + 2]);
                const std::uint32_t g = (std::uint32_t)std::max(0, params_[i + 3]);
                const std::uint32_t b = (std::uint32_t)std::max(0, params_[i + 4]);
                if (fg) attr.fg = Rgb(r, g, b);
                else attr.bg = Rgb(r, g, b);
                i += 4;
            } else {
                i += 1;
            }
            break;
        }
        default:
            if (p >= 30 && p <= 37) attr.fg = Palette((std::uint32_t)(p - 30));
            else if (p >= 90 && p <= 97) attr.fg = Palette((std::uint32_t)(p - 90 + 8));
            else if (p >= 40 && p <= 47) attr.bg = Palette((std::uint32_t)(p - 40));
            else if (p >= 100 && p <= 107) attr.bg = Palette((std::uint32_t)(p - 100 + 8));
            break;
        }
    }
    s.SetAttr(attr);
}

} // namespace ps
