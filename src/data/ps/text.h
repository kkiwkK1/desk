// Byte <-> text for the terminal pipe.
//
// ConPTY writes VT text in the console's code page (the system OEM/ANSI page
// until something switches it to 65001), so each chunk is decoded as UTF-8 and
// falls back to the ANSI code page when those bytes are not valid UTF-8. VT
// escapes are ASCII and survive either way.
#pragma once

#include <string>
#include <string_view>

namespace ps {

std::wstring DecodeChunk(std::string_view bytes);
std::string EncodeUtf8(std::wstring_view text);

} // namespace ps
