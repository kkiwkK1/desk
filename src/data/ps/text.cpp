#include "data/ps/text.h"

#include <windows.h>

#include <string>

namespace ps {
namespace {

std::wstring Decode(std::string_view bytes, UINT codePage, DWORD flags) {
    if (bytes.empty()) return std::wstring();
    const int want = MultiByteToWideChar(codePage, flags, bytes.data(), (int)bytes.size(),
                                         nullptr, 0);
    if (want <= 0) return std::wstring();
    std::wstring out((std::size_t)want, L'\0');
    const int got = MultiByteToWideChar(codePage, flags, bytes.data(), (int)bytes.size(),
                                        out.data(), want);
    if (got <= 0) return std::wstring();
    out.resize((std::size_t)got);
    return out;
}

} // namespace

std::wstring DecodeChunk(std::string_view bytes) {
    // MB_ERR_INVALID_CHARS is what separates real UTF-8 from the GBK bytes the
    // console host emits on a zh-CN system.
    std::wstring text = Decode(bytes, CP_UTF8, MB_ERR_INVALID_CHARS);
    if (!text.empty() || bytes.empty()) return text;
    return Decode(bytes, CP_ACP, 0);
}

std::string EncodeUtf8(std::wstring_view text) {
    if (text.empty()) return std::string();
    const int want = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0,
                                         nullptr, nullptr);
    if (want <= 0) return std::string();
    std::string out((std::size_t)want, '\0');
    const int got = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(),
                                        want, nullptr, nullptr);
    if (got <= 0) return std::string();
    out.resize((std::size_t)got);
    return out;
}

} // namespace ps
