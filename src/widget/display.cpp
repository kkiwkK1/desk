// GetDpiForWindow/GetDpiForSystem need the Windows 10 SDK target (same guard
// canvas.h uses, so the include order does not matter).
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "widget/display.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace wg {
namespace {

class SystemDisplaySource final : public DisplaySource {
public:
    RECT PrimaryWorkArea() const override {
        RECT work{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) && ValidWork(work)) {
            return work;
        }
        // Headless/odd sessions: fall back to the primary screen metrics.
        RECT fallback{};
        fallback.right = GetSystemMetrics(SM_CXSCREEN);
        fallback.bottom = GetSystemMetrics(SM_CYSCREEN);
        if (!ValidWork(fallback)) {
            fallback.right = fallback.left + 1;
            fallback.bottom = fallback.top + 1;
        }
        return fallback;
    }

    RECT WorkAreaForPoint(POINT screenPoint) const override {
        const HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (monitor && GetMonitorInfoW(monitor, &info) && ValidWork(info.rcWork)) {
            return info.rcWork;
        }
        return PrimaryWorkArea();
    }

    int DpiForWindow(HWND window) const override {
        if (window) {
            const UINT dpi = GetDpiForWindow(window);
            if (dpi) return static_cast<int>(dpi);
        }
        return SystemDpi();
    }

    int SystemDpi() const override {
        const UINT dpi = GetDpiForSystem();
        return dpi ? static_cast<int>(dpi) : 96;
    }

private:
    static bool ValidWork(const RECT& work) {
        return work.right > work.left && work.bottom > work.top;
    }
};

// Shortest/longest integers parsed from the script are clamped into the band
// the rest of the framework accepts; anything outside the int range is invalid.
bool ParseInt(const char** cursor, int* out) {
    const char* p = *cursor;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '\0' || *p == ',') return false;
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(p, &end, 10);
    if (end == p || errno == ERANGE || value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
        return false;
    }
    *cursor = end;
    *out = static_cast<int>(value);
    return true;
}

bool ParseNumberList(const std::string& line, const char* prefix, int* values, int count) {
    const std::size_t prefixLength = std::char_traits<char>::length(prefix);
    if (line.size() < prefixLength || line.compare(0, prefixLength, prefix) != 0) return false;
    const char* cursor = line.c_str() + prefixLength;
    for (int i = 0; i < count; ++i) {
        if (!ParseInt(&cursor, &values[i])) return false;
        if (i + 1 < count) {
            if (*cursor != ',') return false;
            ++cursor;
        } else {
            // Only trailing spaces/tabs are allowed after the last number.
            while (*cursor == ' ' || *cursor == '\t') ++cursor;
            if (*cursor != '\0') return false;
        }
    }
    return true;
}

bool RectangleFromValues(const int* values, RECT* out) {
    if (values[2] <= 0 || values[3] <= 0) return false;
    const std::int64_t right = static_cast<std::int64_t>(values[0]) + values[2];
    const std::int64_t bottom = static_cast<std::int64_t>(values[1]) + values[3];
    if (right > std::numeric_limits<int>::max() || bottom > std::numeric_limits<int>::max() ||
        right < std::numeric_limits<int>::min() || bottom < std::numeric_limits<int>::min()) {
        return false;
    }
    out->left = values[0];
    out->top = values[1];
    out->right = static_cast<int>(right);
    out->bottom = static_cast<int>(bottom);
    return true;
}

// Reads the whole script file as ASCII. Returns false when the file is missing
// or cannot be read; a partially read file still counts as read.
bool ReadScript(const std::wstring& file, std::string* text) {
    if (file.empty()) return false;
    const HANDLE handle =
        CreateFileW(file.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    constexpr std::size_t kMaxBytes = 64 * 1024;
    std::string data;
    char buffer[4096];
    for (;;) {
        if (data.size() >= kMaxBytes) break;
        DWORD read = 0;
        if (!ReadFile(handle, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) ||
            read == 0) {
            break;
        }
        const std::size_t remaining = kMaxBytes - data.size();
        data.append(buffer, read < remaining ? read : remaining);
    }
    CloseHandle(handle);
    *text = std::move(data);
    return true;
}

void Trim(std::string* text) {
    std::size_t begin = 0;
    std::size_t end = text->size();
    while (begin < end && std::isspace(static_cast<unsigned char>((*text)[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>((*text)[end - 1]))) --end;
    text->erase(end);
    text->erase(0, begin);
}

class ScriptedSource final : public ScriptedDisplaySource {
public:
    explicit ScriptedSource(std::wstring file) : file_(std::move(file)) {}

    RECT PrimaryWorkArea() const override {
        Refresh();
        return work_;
    }

    RECT WorkAreaForPoint(POINT) const override {
        // One scripted display: every point resolves to the primary work area.
        Refresh();
        return work_;
    }

    int DpiForWindow(HWND) const override {
        Refresh();
        return dpi_;
    }

    int SystemDpi() const override {
        Refresh();
        return dpi_;
    }

    bool SuggestedRect(RECT* out) const override {
        Refresh();
        if (!out || !hasSuggested_) return false;
        *out = suggested_;
        return true;
    }

private:
    // Re-reads the script on every query. A readable file updates the fields it
    // contains and leaves the others at their last valid value; a missing or
    // unreadable file keeps every last valid value. The suggested rectangle is
    // per-read: it is only reported when the current file has a valid rect=
    // line (a stale suggestion must not outlive the scenario that wrote it).
    void Refresh() const {
        std::string text;
        if (!ReadScript(file_, &text)) return;

        RECT work = work_;
        int dpi = dpi_;
        RECT suggested{};
        bool hasSuggested = false;

        std::size_t begin = 0;
        while (begin <= text.size()) {
            std::size_t end = text.find('\n', begin);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(begin, end - begin);
            Trim(&line);
            if (!line.empty()) {
                int values[4] = {0, 0, 0, 0};
                if (ParseNumberList(line, "work=", values, 4)) {
                    RECT parsed{};
                    if (RectangleFromValues(values, &parsed)) work = parsed;
                } else if (ParseNumberList(line, "dpi=", values, 1)) {
                    int parsed = values[0];
                    if (parsed < 48) parsed = 48;
                    if (parsed > 960) parsed = 960;
                    dpi = parsed;
                } else if (ParseNumberList(line, "rect=", values, 4)) {
                    RECT parsed{};
                    if (RectangleFromValues(values, &parsed)) {
                        suggested = parsed;
                        hasSuggested = true;
                    }
                }
                // Anything else is an invalid line and is ignored.
            }
            if (end == text.size()) break;
            begin = end + 1;
        }

        work_ = work;
        dpi_ = dpi;
        suggested_ = suggested;
        hasSuggested_ = hasSuggested;
    }

    const std::wstring file_;
    mutable RECT work_{};
    mutable int dpi_ = 96;
    mutable RECT suggested_{};
    mutable bool hasSuggested_ = false;
};

} // namespace

std::shared_ptr<DisplaySource> CreateSystemDisplaySource() {
    return std::make_shared<SystemDisplaySource>();
}

std::shared_ptr<ScriptedDisplaySource> ScriptedDisplaySource::Create(const std::wstring& file) {
    return std::make_shared<ScriptedSource>(file);
}

} // namespace wg
