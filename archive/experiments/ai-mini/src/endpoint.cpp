#include "endpoint.h"

#include "text.h"

#include <windows.h>

#include <string>

namespace aimini {
namespace {

std::wstring TrimWide(std::wstring value) {
    const wchar_t* ws = L" \t\r\n";
    const std::size_t first = value.find_first_not_of(ws);
    if (first == std::wstring::npos) return std::wstring();
    const std::size_t last = value.find_last_not_of(ws);
    return value.substr(first, last - first + 1);
}

} // namespace

Endpoint ParseEndpoint(const std::wstring& url, const std::string& model) {
    Endpoint endpoint;
    if (!model.empty()) endpoint.model = model;

    std::wstring rest = TrimWide(url);
    if (rest.empty()) return endpoint;

    if (rest.rfind(L"https://", 0) == 0) rest.erase(0, 8);
    else if (rest.rfind(L"http://", 0) == 0) rest.erase(0, 7);
    while (!rest.empty() && rest.back() == L'/') rest.pop_back();

    const std::size_t slash = rest.find(L'/');
    if (slash == std::wstring::npos) {
        endpoint.host = rest;
        endpoint.path = L"/chat/completions";
    } else {
        endpoint.host = rest.substr(0, slash);
        endpoint.path = rest.substr(slash);
    }
    if (endpoint.host.empty()) {
        endpoint.host = L"api.deepseek.com";
        endpoint.path = L"/chat/completions";
    }
    return endpoint;
}

Endpoint FromEnv() {
    Endpoint endpoint;

    wchar_t buffer[2048]{};
    DWORD length = GetEnvironmentVariableW(L"AIMINI_ENDPOINT", buffer, 2048);
    if (length > 0 && length < 2048) {
        endpoint = ParseEndpoint(buffer, endpoint.model);
    }

    length = GetEnvironmentVariableW(L"AIMINI_MODEL", buffer, 2048);
    if (length > 0 && length < 2048) {
        const std::string model = text::Narrow(std::wstring(buffer, length));
        if (!model.empty()) endpoint.model = model;
    }
    return endpoint;
}

} // namespace aimini
