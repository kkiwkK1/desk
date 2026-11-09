#include "apikey.h"

#include "widget/secrets.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace aimini {
namespace {

std::string TrimAscii(std::string s) {
    const char* ws = " \t\r\n";
    const std::size_t first = s.find_first_not_of(ws);
    if (first == std::string::npos) return std::string();
    const std::size_t last = s.find_last_not_of(ws);
    return s.substr(first, last - first + 1);
}

// ~/.dsh/.credentials.yaml is a flat "refs:" document (key: value lines).
std::wstring FromDshCredentials() {
    wchar_t profile[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) return std::wstring();

    std::ifstream in(fs::path(profile) / L".dsh" / L".credentials.yaml", std::ios::binary);
    if (!in) return std::wstring();
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());

    const std::string prefix = "DEEPSEEK_API_KEY:";
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = TrimAscii(text.substr(pos, end - pos));
        pos = end + 1;

        if (line.compare(0, prefix.size(), prefix) != 0) continue;
        std::string value = TrimAscii(line.substr(prefix.size()));
        if (value.empty() || value[0] == '|' || value[0] == '>') return std::wstring();
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                  (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        if (value.size() < 8) return std::wstring();
        return std::wstring(value.begin(), value.end());
    }
    return std::wstring();
}

} // namespace

std::wstring LoadApiKey() {
    wchar_t env[2048]{};
    DWORD n = GetEnvironmentVariableW(L"AIMINI_API_KEY", env, 2048);
    if (n > 0 && n < 2048) return std::wstring(env, n);

    n = GetEnvironmentVariableW(L"DEEPSEEK_API_KEY", env, 2048);
    if (n > 0 && n < 2048) return std::wstring(env, n);

    std::wstring key = wg::LoadSecret(L"AiMini/DeepSeek");
    if (!key.empty()) return key;

    return FromDshCredentials();
}

bool SaveApiKey(const std::wstring& key) {
    if (key.empty()) return false;
    return wg::SaveSecret(L"AiMini/DeepSeek", L"apikey", key);
}

} // namespace aimini
