#pragma once

#include "cli.h"

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

namespace srv {

enum class Color { None, Red, Green, Yellow, Cyan, Dim, Bold };

struct Cell {
    std::wstring text;
    Color color = Color::None;
    bool right = false;
};

// Display width in terminal cells (CJK / emoji are two, combining marks zero).
std::size_t DisplayWidth(const std::wstring& text);
std::wstring TruncateTo(const std::wstring& text, std::size_t maxWidth);
std::wstring Paint(const std::wstring& text, Color color, bool enabled);

// Lays out a plain-text table, shrinking or dropping trailing columns to fit
// maxWidth. Returns lines joined with '\n' (no trailing newline).
std::wstring RenderTable(const std::vector<std::wstring>& headers,
                         const std::vector<std::vector<Cell>>& rows, bool color,
                         std::size_t maxWidth);

std::string Utf8(const std::wstring& text);
std::wstring Widen(const std::string& text);
std::string JsonString(const std::wstring& text);
std::string JsonDouble(double value);
std::string JsonIntOrNull(int value);
std::string JsonLongOrNull(long long value);

std::wstring FormatPercent(int value);
std::wstring FormatRtt(int ms);
std::wstring FormatUptimeValue(long long seconds);
std::wstring FormatLoadValue(double load);
std::wstring TimestampNow();
long long UnixNow();

// Console-aware writer: WriteConsoleW for the console, UTF-8 for pipes/files.
class Output {
public:
    explicit Output(ColorMode mode);

    bool color() const { return color_; }
    bool console() const { return console_; }
    std::size_t width() const { return width_; }

    void Line(const std::wstring& text = L"");
    void Raw(const std::wstring& text);
    void LineUtf8(const std::string& text);
    void Err(const std::wstring& text = L"");
    void Clear();

private:
    void WriteStd(const std::wstring& text);
    void WriteErr(const std::wstring& text);

    HANDLE out_ = INVALID_HANDLE_VALUE;
    HANDLE err_ = INVALID_HANDLE_VALUE;
    bool console_ = false;
    bool errConsole_ = false;
    bool vt_ = false;
    bool color_ = false;
    std::size_t width_ = 120;
};

} // namespace srv
