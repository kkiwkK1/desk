#include "providers.h"

#include "widget/http.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace aimeter {

long long ParseCents(const std::string& s) {
    long long whole = 0, frac = 0;
    int digits = 0;
    size_t i = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) whole = whole * 10 + (s[i] - '0');
    if (i < s.size() && s[i] == '.') {
        for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 2; ++i, ++digits)
            frac = frac * 10 + (s[i] - '0');
    }
    while (digits < 2) {
        frac *= 10;
        ++digits;
    }
    return whole * 100 + frac;
}

std::wstring Money(long long cents) {
    wchar_t buf[32];
    const long long a = cents < 0 ? -cents : cents;
    swprintf(buf, 32, L"%ls%lld.%02lld", cents < 0 ? L"-" : L"", a / 100, a % 100);
    return buf;
}

Balance FetchDeepSeek(const std::wstring& key) {
    Balance r;
    if (key.empty()) {
        r.text = L"NO KEY";
        return r;
    }

    const wg::HttpResponse res = wg::HttpGet(L"api.deepseek.com", L"/user/balance", key,
                                             L"AiMeter/1.0");
    if (!res.transport) {
        r.text = L"NET ERR";
        return r;
    }
    if (res.status != 200) {
        r.text = res.status == 401 ? L"BAD KEY" : L"ERR " + std::to_wstring(res.status);
        return r;
    }

    size_t p = res.body.find("\"total_balance\":\"");
    if (p == std::string::npos) {
        r.text = L"NO DATA";
        return r;
    }
    p += 17;
    const size_t end = res.body.find('"', p);
    if (end == std::string::npos) {
        r.text = L"NO DATA";
        return r;
    }

    std::string amount = res.body.substr(p, end - p);
    amount.erase(std::remove(amount.begin(), amount.end(), ','), amount.end());

    r.ok = true;
    r.cents = ParseCents(amount);
    r.text = L"CNY " + std::wstring(amount.begin(), amount.end());
    return r;
}

namespace {

double NumberAfter(const std::string& text, const char* key, size_t from) {
    size_t p = text.find(key, from);
    if (p == std::string::npos) return -1.0;
    p += std::strlen(key);
    return std::strtod(text.c_str() + p, nullptr);
}

} // namespace

bool ReadCodex(float& primaryPercent, float& secondaryPercent) {
    wchar_t profile[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) return false;

    const fs::path base = fs::path(profile) / L".codex" / L"sessions";
    std::error_code ec;
    if (!fs::exists(base, ec)) return false;

    fs::path newest;
    fs::file_time_type best{};
    for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != L".jsonl") continue;
        const auto stamp = it->last_write_time(ec);
        if (ec) continue;
        if (newest.empty() || stamp > best) {
            best = stamp;
            newest = it->path();
        }
    }
    if (newest.empty()) return false;

    std::ifstream in(newest, std::ios::binary);
    if (!in) return false;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    const size_t p = text.rfind("\"rate_limits\"");
    if (p == std::string::npos) return false;

    const double primary = NumberAfter(text, "\"primary\":{\"used_percent\":", p);
    const double secondary = NumberAfter(text, "\"secondary\":{\"used_percent\":", p);
    if (primary < 0 || secondary < 0) return false;

    primaryPercent = (float)primary;
    secondaryPercent = (float)secondary;
    return true;
}

} // namespace aimeter
