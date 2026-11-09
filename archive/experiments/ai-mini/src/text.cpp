#include "text.h"

#include <windows.h>

#include <algorithm>
#include <cctype>

namespace text {

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((std::size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), n);
    return out;
}

std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0,
                                nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((std::size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::string Ascii(const std::string& utf8, char fallback) {
    const std::wstring wide = Widen(utf8);
    std::string out;
    out.reserve(wide.size());
    for (const wchar_t c : wide) {
        if (c == L'\n' || c == L'\r' || c == L'\t') out.push_back(' ');
        else if (c >= 32 && c < 127) out.push_back((char)c);
        else out.push_back(fallback);
    }
    return out;
}

std::string Trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const std::size_t first = s.find_first_not_of(ws);
    if (first == std::string::npos) return std::string();
    const std::size_t last = s.find_last_not_of(ws);
    return s.substr(first, last - first + 1);
}

std::string CollapseWhitespace(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool pendingSpace = false;
    for (const char c : s) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back(c);
    }
    return out;
}

std::string Truncate(const std::string& utf8, std::size_t maxChars) {
    if (utf8.size() <= maxChars) return utf8;
    if (maxChars <= 3) return utf8.substr(0, maxChars);
    // Cheap UTF-8 safety: do not cut in the middle of a multi-byte sequence.
    std::size_t cut = maxChars - 3;
    while (cut > 0 && ((unsigned char)utf8[cut] & 0xC0) == 0x80) --cut;
    return utf8.substr(0, cut) + "...";
}

std::vector<std::string> Wrap(const std::string& text, std::size_t maxChars) {
    if (maxChars < 1) maxChars = 1;
    std::vector<std::string> lines;
    const std::string flat = CollapseWhitespace(text);

    std::string line;
    std::size_t pos = 0;
    while (pos < flat.size()) {
        const std::size_t space = flat.find(' ', pos);
        std::string word = flat.substr(pos, space == std::string::npos ? std::string::npos
                                                                       : space - pos);
        pos = (space == std::string::npos) ? flat.size() : space + 1;

        while (word.size() > maxChars) {
            if (!line.empty()) {
                lines.push_back(line);
                line.clear();
            }
            lines.push_back(word.substr(0, maxChars));
            word.erase(0, maxChars);
        }

        if (word.empty()) continue;
        if (line.empty()) {
            line = word;
        } else if (line.size() + 1 + word.size() <= maxChars) {
            line.push_back(' ');
            line += word;
        } else {
            lines.push_back(line);
            line = word;
        }
    }
    if (!line.empty()) lines.push_back(line);
    if (lines.empty()) lines.emplace_back();
    return lines;
}

} // namespace text
