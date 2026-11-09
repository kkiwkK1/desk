#include "token_usage.h"

#include <windows.h>

#include <climits>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace aimeter {
namespace {

constexpr long long kUnixToFileTimeMs = 11644473600000LL;

bool FindNumber(const std::string& text, const char* key, size_t from, size_t limit,
                unsigned long long& out) {
    const std::string pattern = std::string("\"") + key + "\"";
    size_t p = text.find(pattern, from);
    if (p == std::string::npos || p >= limit) return false;

    p = text.find(':', p + pattern.size());
    if (p == std::string::npos || p >= limit) return false;
    ++p;

    while (p < limit) {
        const char c = text[p];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++p;
    }
    if (p >= limit || text[p] < '0' || text[p] > '9') return false;

    unsigned long long value = 0;
    while (p < limit && text[p] >= '0' && text[p] <= '9') {
        const unsigned digit = (unsigned)(text[p] - '0');
        if (value > (ULLONG_MAX - digit) / 10) return false;
        value = value * 10 + digit;
        ++p;
    }
    out = value;
    return true;
}

bool SameLocalDay(long long epochMs, const std::wstring& today) {
    if (epochMs <= 0) return false;

    const unsigned long long ticks =
        (unsigned long long)(epochMs + kUnixToFileTimeMs) * 10000ULL;
    FILETIME ft{};
    ft.dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFFULL);
    ft.dwHighDateTime = (DWORD)(ticks >> 32);

    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&ft, &utc)) return false;
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return false;

    wchar_t day[16];
    swprintf(day, 16, L"%04d-%02d-%02d", local.wYear, local.wMonth, local.wDay);
    return today == day;
}

TokenUsage ParseUsage(const std::wstring& value) {
    TokenUsage u;
    unsigned long long* slots[4] = {&u.uncachedInput, &u.cacheRead, &u.cacheWrite, &u.output};
    int slot = 0;
    size_t pos = 0;
    while (slot < 4 && pos <= value.size()) {
        const size_t comma = value.find(L',', pos);
        const std::wstring part = value.substr(pos, comma == std::wstring::npos
                                                        ? std::wstring::npos
                                                        : comma - pos);
        *slots[slot++] = wcstoull(part.c_str(), nullptr, 10);
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    return u;
}

void SaveUsage(wg::Store& store, const std::wstring& key, const TokenUsage& u) {
    wchar_t buf[128];
    swprintf(buf, 128, L"%llu,%llu,%llu,%llu", u.uncachedInput, u.cacheRead, u.cacheWrite,
             u.output);
    store.Set(key.c_str(), buf);
}

void AddDelta(TokenUsage& sum, const TokenUsage& current, const TokenUsage& previous) {
    if (current.uncachedInput > previous.uncachedInput)
        sum.uncachedInput += current.uncachedInput - previous.uncachedInput;
    if (current.cacheRead > previous.cacheRead)
        sum.cacheRead += current.cacheRead - previous.cacheRead;
    if (current.cacheWrite > previous.cacheWrite)
        sum.cacheWrite += current.cacheWrite - previous.cacheWrite;
    if (current.output > previous.output)
        sum.output += current.output - previous.output;
}

void AddAll(TokenUsage& sum, const TokenUsage& u) {
    sum.uncachedInput += u.uncachedInput;
    sum.cacheRead += u.cacheRead;
    sum.cacheWrite += u.cacheWrite;
    sum.output += u.output;
}

} // namespace

bool ReadDshSessions(std::vector<DshSessionUsage>& out) {
    wchar_t profile[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) return false;

    const fs::path dir = fs::path(profile) / L".dsh" / L"storages" / L"session_projcache" /
                         L"sessions";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;

    bool sawFile = false;
    bool parsedAny = false;

    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != L".json") continue;
        sawFile = true;

        std::ifstream in(it->path(), std::ios::binary);
        if (!in) continue;
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        if (text.size() < 64 || text.size() > 4 * 1024 * 1024) continue;

        const size_t identity = text.find("\"identity\"");
        if (identity == std::string::npos) continue;
        unsigned long long created = 0;
        FindNumber(text, "createdAt", identity, text.size(), created);

        const size_t usage = text.find("\"tokenUsage\"");
        if (usage == std::string::npos) continue;
        const size_t totals = text.find("\"totals\"", usage);
        if (totals == std::string::npos) continue;
        const size_t totalsEnd = text.find('}', totals);
        if (totalsEnd == std::string::npos) continue;

        DshSessionUsage session;
        session.id = it->path().stem().wstring();
        session.createdAtMs = (long long)created;

        bool any = false;
        any |= FindNumber(text, "uncachedInputTokens", totals, totalsEnd,
                          session.totals.uncachedInput);
        any |= FindNumber(text, "cacheReadTokens", totals, totalsEnd, session.totals.cacheRead);
        any |= FindNumber(text, "cacheWriteTokens", totals, totalsEnd, session.totals.cacheWrite);
        any |= FindNumber(text, "outputTokens", totals, totalsEnd, session.totals.output);
        if (!any) continue;

        parsedAny = true;
        out.push_back(std::move(session));
    }
    // Files that exist but never parse mean the schema changed: report "no
    // source" instead of a misleading zero.
    return parsedAny || !sawFile;
}

TokenUsage UpdateTodayTokens(const std::vector<DshSessionUsage>& sessions, wg::Store& store,
                             const std::wstring& today) {
    const bool newDay = store.Get(L"tok.day") != today;
    TokenUsage sum;

    if (newDay) {
        store.Set(L"tok.day", today);
        store.RemovePrefix(L"tok.s.");
        store.Set(L"tok.sum", L"0,0,0,0");
    } else {
        sum = ParseUsage(store.Get(L"tok.sum", L"0,0,0,0"));
    }

    for (const auto& session : sessions) {
        const std::wstring key = L"tok.s." + session.id;
        if (!store.Has(key.c_str())) {
            // A session created today contains only today's tokens. An older
            // session is baselined, because its totals may include yesterday.
            if (SameLocalDay(session.createdAtMs, today)) AddAll(sum, session.totals);
        } else {
            AddDelta(sum, session.totals, ParseUsage(store.Get(key.c_str(), L"")));
        }
        SaveUsage(store, key, session.totals);
    }

    SaveUsage(store, L"tok.sum", sum);
    return sum;
}

std::wstring LocalDay() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buf[16];
    swprintf(buf, 16, L"%04d-%02d-%02d", now.wYear, now.wMonth, now.wDay);
    return buf;
}

std::wstring FormatTokens(unsigned long long count) {
    wchar_t buf[32];
    if (count >= 999500000ULL) {
        const double v = (double)count / 1e9;
        if (v >= 100) swprintf(buf, 32, L"%.0fB", v);
        else if (v >= 10) swprintf(buf, 32, L"%.1fB", v);
        else swprintf(buf, 32, L"%.2fB", v);
    } else if (count >= 999500ULL) {
        const double v = (double)count / 1e6;
        if (v >= 100) swprintf(buf, 32, L"%.0fM", v);
        else if (v >= 10) swprintf(buf, 32, L"%.1fM", v);
        else swprintf(buf, 32, L"%.2fM", v);
    } else if (count >= 1000ULL) {
        const double v = (double)count / 1e3;
        if (v >= 100) swprintf(buf, 32, L"%.0fK", v);
        else if (v >= 10) swprintf(buf, 32, L"%.1fK", v);
        else swprintf(buf, 32, L"%.2fK", v);
    } else {
        swprintf(buf, 32, L"%llu", count);
    }
    return buf;
}

} // namespace aimeter
