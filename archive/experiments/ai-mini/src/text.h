// UTF-8 / UTF-16 helpers + the ASCII line wrapper used by the pixel canvas.
// The 5x7 font in src/widget is ASCII-only, so anything shown on the widget is
// folded here (one '?' per non-ASCII code point) instead of silently blanking.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace text {

std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& wide);

// Fold to printable ASCII; newlines and tabs become spaces.
std::string Ascii(const std::string& utf8, char fallback = '?');

std::string Trim(const std::string& s);
std::string CollapseWhitespace(const std::string& s);
std::string Truncate(const std::string& utf8, std::size_t maxChars);

// Greedy word wrap by character count. Never returns an empty vector; a long
// word is hard-split so a single token cannot overflow the widget.
std::vector<std::string> Wrap(const std::string& text, std::size_t maxChars);

} // namespace text
