#include "output.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>

namespace srv {
namespace {

const wchar_t* Ansi(Color color) {
    switch (color) {
        case Color::Red: return L"\x1b[31m";
        case Color::Green: return L"\x1b[32m";
        case Color::Yellow: return L"\x1b[33m";
        case Color::Cyan: return L"\x1b[36m";
        case Color::Dim: return L"\x1b[2m";
        case Color::Bold: return L"\x1b[1m";
        case Color::None: break;
    }
    return L"";
}

bool IsConsoleHandle(HANDLE h) {
    DWORD mode = 0;
    return h != nullptr && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) != FALSE;
}

bool EnableVt(HANDLE h) {
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return false;
    if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) return true;
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != FALSE;
}

std::wstring EnvW(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(needed, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, value.data(), needed);
    if (got == 0 || got >= needed) return {};
    value.resize(got);
    return value;
}

std::size_t Decode(const std::wstring& text, std::size_t i, unsigned int& cp) {
    const wchar_t c = text[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size()) {
        const wchar_t low = text[i + 1];
        if (low >= 0xDC00 && low <= 0xDFFF) {
            cp = 0x10000u + (((unsigned int)(c - 0xD800) << 10) | (unsigned int)(low - 0xDC00));
            return 2;
        }
    }
    cp = (unsigned int)(unsigned short)c;
    return 1;
}

bool IsCombining(unsigned int cp) {
    return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F) ||
           (cp >= 0xE0100 && cp <= 0xE01EF) || cp == 0x200B || cp == 0x200D;
}

bool IsWide(unsigned int cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2329 && cp <= 0x232A) ||
           (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xA000 && cp <= 0xA4CF) || (cp >= 0xA960 && cp <= 0xA97F) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE10 && cp <= 0xFE19) || (cp >= 0xFE30 && cp <= 0xFE6F) ||
           (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) ||
           (cp >= 0x1F300 && cp <= 0x1F64F) || (cp >= 0x1F900 && cp <= 0x1F9FF) ||
           (cp >= 0x1FA70 && cp <= 0x1FAFF) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

std::size_t CodePointWidth(unsigned int cp) {
    if (cp == 0 || cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
    if (IsCombining(cp)) return 0;
    return IsWide(cp) ? 2 : 1;
}

std::wstring RTrim(std::wstring text) {
    while (!text.empty() && text.back() == L' ') text.pop_back();
    return text;
}

void WriteBytes(HANDLE h, const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD wrote = 0;
        const DWORD chunk = (DWORD)std::min<std::size_t>(bytes.size() - offset, 8192);
        if (!WriteFile(h, bytes.data() + offset, chunk, &wrote, nullptr) || wrote == 0) break;
        offset += wrote;
    }
}

} // namespace

std::size_t DisplayWidth(const std::wstring& text) {
    std::size_t width = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        unsigned int cp = 0;
        const std::size_t used = Decode(text, i, cp);
        width += CodePointWidth(cp);
        i += used;
    }
    return width;
}

std::wstring TruncateTo(const std::wstring& text, std::size_t maxWidth) {
    if (DisplayWidth(text) <= maxWidth) return text;
    if (maxWidth == 0) return {};
    const bool ellipsis = maxWidth >= 4;
    const std::size_t budget = ellipsis ? maxWidth - 3 : maxWidth;

    std::wstring out;
    std::size_t used = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        unsigned int cp = 0;
        const std::size_t units = Decode(text, i, cp);
        const std::size_t w = CodePointWidth(cp);
        if (used + w > budget) break;
        out.append(text, i, units);
        used += w;
        i += units;
    }
    if (ellipsis) out += L"...";
    return out;
}

std::wstring Paint(const std::wstring& text, Color color, bool enabled) {
    if (!enabled || color == Color::None) return text;
    return std::wstring(Ansi(color)) + text + L"\x1b[0m";
}

std::wstring RenderTable(const std::vector<std::wstring>& headers,
                         const std::vector<std::vector<Cell>>& rows, bool color,
                         std::size_t maxWidth) {
    if (headers.empty()) return {};
    if (maxWidth < 20) maxWidth = 20;

    std::size_t active = headers.size();
    std::vector<std::size_t> widths;

    for (;;) {
        widths.assign(active, 0);
        for (std::size_t j = 0; j < active; ++j) {
            widths[j] = DisplayWidth(headers[j]);
            for (const auto& row : rows) {
                if (j < row.size()) widths[j] = std::max(widths[j], DisplayWidth(row[j].text));
            }
        }

        std::size_t total = 2 * (active - 1);
        for (std::size_t w : widths) total += w;
        if (total <= maxWidth || active <= 2) {
            // Still too wide with two columns: squeeze the right one.
            if (total > maxWidth && active == 2) {
                if (widths[0] + 2 >= maxWidth) {
                    widths[0] = maxWidth > 2 ? maxWidth - 2 : 1;
                    widths[1] = 0;
                } else {
                    widths[1] = maxWidth - widths[0] - 2;
                }
            }
            break;
        }

        std::size_t base = 2 * (active - 1);
        for (std::size_t j = 0; j + 1 < active; ++j) base += widths[j];
        const std::size_t available = maxWidth > base ? maxWidth - base : 0;
        if (available >= 8) {
            widths[active - 1] = available;
            break;
        }
        --active;
    }

    auto buildLine = [&](const std::vector<Cell>& cells) {
        std::wstring line;
        for (std::size_t j = 0; j < active; ++j) {
            const Cell empty;
            const Cell& cell = j < cells.size() ? cells[j] : empty;
            std::wstring text = TruncateTo(cell.text, widths[j]);
            const std::size_t pad = widths[j] - DisplayWidth(text);
            if (cell.right) {
                line.append(pad, L' ');
                line += Paint(text, cell.color, color);
            } else {
                line += Paint(text, cell.color, color);
                if (j + 1 < active) line.append(pad, L' ');
            }
            if (j + 1 < active) line += L"  ";
        }
        return RTrim(line);
    };

    std::vector<Cell> headerCells;
    headerCells.reserve(active);
    for (std::size_t j = 0; j < active; ++j) {
        headerCells.push_back(Cell{headers[j], Color::Bold, false});
    }

    std::wstring out = buildLine(headerCells);
    out += L'\n';

    std::wstring rule;
    for (std::size_t j = 0; j < active; ++j) {
        if (j) rule += L"  ";
        rule.append(widths[j], L'-');
    }
    out += RTrim(rule);
    out += L'\n';

    for (std::size_t r = 0; r < rows.size(); ++r) {
        out += buildLine(rows[r]);
        if (r + 1 < rows.size()) out += L'\n';
    }
    return out;
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out((std::size_t)needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), needed, nullptr,
                        nullptr);
    return out;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(),
                                           (int)text.size(), nullptr, 0);
    if (needed <= 0) {
        const int fallback = MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(),
                                                 nullptr, 0);
        if (fallback <= 0) return {};
        std::wstring out((std::size_t)fallback, L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(), out.data(), fallback);
        return out;
    }
    std::wstring out((std::size_t)needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), needed);
    return out;
}

std::string JsonString(const std::wstring& text) {
    const std::string raw = Utf8(text);
    std::string out;
    out.reserve(raw.size() + 2);
    out.push_back('"');
    static const char* kHex = "0123456789abcdef";
    for (unsigned char c : raw) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(kHex[(c >> 4) & 0xF]);
                    out.push_back(kHex[c & 0xF]);
                } else {
                    out.push_back((char)c);
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

std::string JsonDouble(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", value);
    return buf;
}

std::string JsonIntOrNull(int value) {
    return value < 0 ? std::string("null") : std::to_string(value);
}

std::string JsonLongOrNull(long long value) {
    return value < 0 ? std::string("null") : std::to_string(value);
}

std::wstring FormatPercent(int value) {
    if (value < 0) return L"--";
    wchar_t buf[8];
    std::swprintf(buf, 8, L"%d%%", value);
    return buf;
}

std::wstring FormatRtt(int ms) {
    wchar_t buf[16];
    if (ms >= 1000) std::swprintf(buf, 16, L"%.1fs", ms / 1000.0);
    else std::swprintf(buf, 16, L"%dms", ms);
    return buf;
}

std::wstring FormatUptimeValue(long long seconds) {
    if (seconds < 0) return L"--";
    wchar_t buf[24];
    if (seconds >= 86400) std::swprintf(buf, 24, L"%lldD", seconds / 86400);
    else if (seconds >= 3600) std::swprintf(buf, 24, L"%lldH", seconds / 3600);
    else std::swprintf(buf, 24, L"%lldM", seconds / 60);
    return buf;
}

std::wstring FormatLoadValue(double load) {
    if (load < 0.0) return L"--";
    wchar_t buf[24];
    std::swprintf(buf, 24, load >= 100.0 ? L"%.0f" : L"%.2f", load);
    return buf;
}

std::wstring TimestampNow() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buf[32];
    std::swprintf(buf, 32, L"%04d-%02d-%02d %02d:%02d:%02d", now.wYear, now.wMonth, now.wDay,
                  now.wHour, now.wMinute, now.wSecond);
    return buf;
}

long long UnixNow() {
    return (long long)std::time(nullptr);
}

Output::Output(ColorMode mode) {
    out_ = GetStdHandle(STD_OUTPUT_HANDLE);
    err_ = GetStdHandle(STD_ERROR_HANDLE);
    console_ = IsConsoleHandle(out_);
    errConsole_ = IsConsoleHandle(err_);
    vt_ = console_ && EnableVt(out_);

    bool color = false;
    if (mode == ColorMode::Always) {
        color = true;
    } else if (mode == ColorMode::Auto) {
        const std::wstring noColor = EnvW(L"NO_COLOR");
        const std::wstring term = EnvW(L"TERM");
        color = console_ && vt_ && noColor.empty() && term != L"dumb";
    }
    color_ = color;

    if (console_) {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(out_, &info)) {
            const int cols = info.srWindow.Right - info.srWindow.Left + 1;
            if (cols > 0) width_ = (std::size_t)cols;
        }
    }
}

void Output::WriteStd(const std::wstring& text) {
    if (console_) {
        std::size_t offset = 0;
        while (offset < text.size()) {
            DWORD wrote = 0;
            const DWORD chunk = (DWORD)std::min<std::size_t>(text.size() - offset, 8192);
            if (!WriteConsoleW(out_, text.data() + offset, chunk, &wrote, nullptr) || wrote == 0) {
                break;
            }
            offset += wrote;
        }
    } else {
        WriteBytes(out_, Utf8(text));
    }
}

void Output::WriteErr(const std::wstring& text) {
    if (errConsole_) {
        std::size_t offset = 0;
        while (offset < text.size()) {
            DWORD wrote = 0;
            const DWORD chunk = (DWORD)std::min<std::size_t>(text.size() - offset, 8192);
            if (!WriteConsoleW(err_, text.data() + offset, chunk, &wrote, nullptr) || wrote == 0) {
                break;
            }
            offset += wrote;
        }
    } else {
        WriteBytes(err_, Utf8(text));
    }
}

void Output::Line(const std::wstring& text) {
    if (!console_) {
        WriteStd(text + L"\n");
        return;
    }
    // A block (table, help) arrives with bare '\n'; the Windows console needs
    // CR before every LF or each line starts where the previous one ended.
    std::wstring fixed;
    fixed.reserve(text.size() + 16);
    for (wchar_t c : text) {
        if (c == L'\n' && (fixed.empty() || fixed.back() != L'\r')) fixed.push_back(L'\r');
        fixed.push_back(c);
    }
    WriteStd(fixed + L"\r\n");
}

void Output::Raw(const std::wstring& text) {
    WriteStd(text);
}

void Output::LineUtf8(const std::string& text) {
    if (console_) {
        WriteStd(Widen(text) + L"\r\n");
    } else {
        WriteBytes(out_, text);
        WriteBytes(out_, "\n");
    }
}

void Output::Err(const std::wstring& text) {
    WriteErr(text + (errConsole_ ? L"\r\n" : L"\n"));
}

void Output::Clear() {
    if (!console_) return;
    if (vt_) {
        WriteStd(L"\x1b[2J\x1b[H");
        return;
    }
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(out_, &info)) return;
    const DWORD cells = (DWORD)(info.dwSize.X * info.dwSize.Y);
    DWORD written = 0;
    COORD home{0, 0};
    FillConsoleOutputCharacterW(out_, L' ', cells, home, &written);
    FillConsoleOutputAttribute(out_, info.wAttributes, cells, home, &written);
    SetConsoleCursorPosition(out_, home);
}

} // namespace srv
