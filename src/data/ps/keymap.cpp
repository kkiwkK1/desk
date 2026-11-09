#include "data/ps/keymap.h"

#include "data/ps/text.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace ps {
namespace {

// xterm modifier parameter: 1 + (shift 1 | alt 2 | ctrl 4).
int Modifier(const wg::KeyEvent& key) {
    int m = 1;
    if (key.shift) m += 1;
    if (key.alt) m += 2;
    if (key.ctrl) m += 4;
    return m;
}

std::string Csi(const char* body) { return std::string("\x1b[") + body; }

std::string CursorKey(char final, const wg::KeyEvent& key, const KeyMode& mode) {
    const int m = Modifier(key);
    if (m > 1) {
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "1;%d%c", m, final);
        return Csi(buffer);
    }
    if (mode.appCursor) return std::string("\x1bO") + final;
    return std::string("\x1b[") + final;
}

std::string Tilde(int code, const wg::KeyEvent& key) {
    const int m = Modifier(key);
    char buffer[16];
    if (m > 1) snprintf(buffer, sizeof(buffer), "%d;%d~", code, m);
    else snprintf(buffer, sizeof(buffer), "%d~", code);
    return Csi(buffer);
}

} // namespace

WheelMode ParseWheelMode(std::wstring_view value) {
    if (value == L"history") return WheelMode::History;
    if (value == L"application") return WheelMode::Application;
    return WheelMode::Auto;
}

const wchar_t* WheelModeName(WheelMode mode) {
    switch (mode) {
    case WheelMode::History: return L"history";
    case WheelMode::Application: return L"application";
    case WheelMode::Auto: return L"auto";
    }
    return L"auto";
}

WheelRoute ChooseWheelRoute(WheelMode mode, bool mouseTracking, bool sgrMouse,
                            bool alternate, bool piFullScreen) {
    if (mode == WheelMode::History)
        return alternate ? WheelRoute::None : WheelRoute::History;
    if (mode == WheelMode::Application) return WheelRoute::SgrMouse;
    if (mouseTracking) return sgrMouse ? WheelRoute::SgrMouse : WheelRoute::LegacyMouse;
    if (piFullScreen) return WheelRoute::SgrMouse;
    return alternate ? WheelRoute::None : WheelRoute::History;
}

std::string EncodeWheel(bool up, int col, int row, bool sgr, bool shift, bool alt, bool ctrl) {
    const int button = (up ? 64 : 65) + (shift ? 4 : 0) + (alt ? 8 : 0) + (ctrl ? 16 : 0);
    col = std::max(1, col);
    row = std::max(1, row);
    if (sgr) {
        return "\x1b[<" + std::to_string(button) + ";" + std::to_string(col) + ";" +
               std::to_string(row) + "M";
    }
    // Legacy X10 encoding has one byte per coordinate and cannot exceed 223.
    std::string result = "\x1b[M";
    result.push_back((char)(button + 32));
    result.push_back((char)(std::min(col, 223) + 32));
    result.push_back((char)(std::min(row, 223) + 32));
    return result;
}

std::string EncodeKey(const wg::KeyEvent& key, const KeyMode& mode) {
    // Control bytes first: Ctrl+letter is the shell's bread and butter.
    if (key.ctrl) {
        if (key.vk >= 'A' && key.vk <= 'Z') {
            const char c = (char)(key.vk - 'A' + 1);
            return key.alt ? std::string("\x1b") + c : std::string(1, c);
        }
        if (key.vk == VK_SPACE) return std::string(1, '\0');
        if (key.vk == VK_OEM_4) return std::string(1, '\x1b');   // Ctrl+[
        if (key.vk == VK_OEM_5) return std::string(1, '\x1c');   // Ctrl + backslash
        if (key.vk == VK_OEM_6) return std::string(1, '\x1d');   // Ctrl+]
    }

    switch (key.vk) {
    case VK_RETURN:
        return std::string(1, '\r');
    case VK_TAB:
        return key.shift ? std::string("\x1b[Z") : std::string(1, '\t');
    case VK_ESCAPE:
        return std::string(1, '\x1b');
    case VK_BACK:
        // VT terminals send DEL; PSReadLine and vim both accept it.
        return std::string(1, '\x7f');
    case VK_UP:
        return CursorKey('A', key, mode);
    case VK_DOWN:
        return CursorKey('B', key, mode);
    case VK_RIGHT:
        return CursorKey('C', key, mode);
    case VK_LEFT:
        return CursorKey('D', key, mode);
    case VK_HOME:
        return CursorKey('H', key, mode);
    case VK_END:
        return CursorKey('F', key, mode);
    case VK_INSERT:
        return Tilde(2, key);
    case VK_DELETE:
        return Tilde(3, key);
    case VK_PRIOR:
        return Tilde(5, key);
    case VK_NEXT:
        return Tilde(6, key);
    case VK_F1:
        return key.shift || key.ctrl || key.alt ? Tilde(11, key) : std::string("\x1bOP");
    case VK_F2:
        return key.shift || key.ctrl || key.alt ? Tilde(12, key) : std::string("\x1bOQ");
    case VK_F3:
        return key.shift || key.ctrl || key.alt ? Tilde(13, key) : std::string("\x1bOR");
    case VK_F4:
        return key.shift || key.ctrl || key.alt ? Tilde(14, key) : std::string("\x1bOS");
    case VK_F5:
        return Tilde(15, key);
    case VK_F6:
        return Tilde(17, key);
    case VK_F7:
        return Tilde(18, key);
    case VK_F8:
        return Tilde(19, key);
    case VK_F9:
        return Tilde(20, key);
    case VK_F10:
        return Tilde(21, key);
    case VK_F11:
        return Tilde(23, key);
    case VK_F12:
        return Tilde(24, key);
    default:
        break;
    }

    // Alt+letter is ESC + letter (Windows Terminal's default).
    if (key.alt && key.vk >= 'A' && key.vk <= 'Z') {
        return std::string("\x1b") + (char)key.vk;
    }
    if (key.alt && key.vk >= '0' && key.vk <= '9') {
        return std::string("\x1b") + (char)key.vk;
    }
    return std::string();
}

std::string EncodePaste(std::wstring_view text, bool bracketed) {
    std::wstring normalized;
    normalized.reserve(text.size() + 16);
    for (const wchar_t c : text) {
        if (c == L'\r') continue;              // CRLF -> CR below
        if (c == L'\n') normalized.push_back(L'\r');
        else if (c == L'\t') normalized.push_back(L'\t');
        else if (c >= 32 || c == L'\r') normalized.push_back(c);
    }
    std::string bytes = EncodeUtf8(normalized);
    if (!bracketed) return bytes;
    return std::string("\x1b[200~") + bytes + "\x1b[201~";
}

} // namespace ps
