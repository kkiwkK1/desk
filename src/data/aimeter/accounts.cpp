#include "accounts.h"

#include "widget/diag.h"
#include "widget/http.h"
#include "widget/secrets.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace aimeter {
namespace {

constexpr const wchar_t* kLogCategory = L"aimeter";
constexpr const wchar_t* kCredPrefix = L"AiMeter/CommandCode";
constexpr const char* kRefBase = "COMMANDCODE_API_KEY";
constexpr const char* kCredHost = "api.commandcode.ai";
constexpr const wchar_t* kCredHostW = L"api.commandcode.ai";
constexpr const wchar_t* kCredPath = L"/alpha/billing/credits";
constexpr const wchar_t* kUserAgent = L"AiMeter/1.0";

std::string TrimAscii(std::string s) {
    const char* ws = " \t\r\n";
    const size_t first = s.find_first_not_of(ws);
    if (first == std::string::npos) return {};
    const size_t last = s.find_last_not_of(ws);
    return s.substr(first, last - first + 1);
}

std::wstring TrimWide(std::wstring s) {
    const wchar_t* ws = L" \t\r\n";
    const size_t first = s.find_first_not_of(ws);
    if (first == std::wstring::npos) return {};
    const size_t last = s.find_last_not_of(ws);
    return s.substr(first, last - first + 1);
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) {
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    }
    return s;
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out((size_t)size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), size);
    return out;
}

double ParseDouble(const std::wstring& text, double fallback = 0.0) {
    if (text.empty()) return fallback;
    wchar_t* end = nullptr;
    const double value = std::wcstod(text.c_str(), &end);
    if (end == text.c_str()) return fallback;
    return value;
}

std::wstring FormatDouble(double value) {
    wchar_t buffer[64];
    std::swprintf(buffer, 64, L"%.6g", value);
    return buffer;
}

std::wstring DefaultLabel(int index) { return L"ACC" + std::to_wstring(index); }

std::wstring DefaultRef(int index) {
    if (index <= 1) return L"COMMANDCODE_API_KEY";
    return L"COMMANDCODE_API_KEY_" + std::to_wstring(index);
}

std::wstring AccountField(int index, const wchar_t* field) {
    return L"account." + std::to_wstring(index) + L"." + field;
}

// --- DSH credentials (~/.dsh/.credentials.yaml) --------------------------
//
// The file is a tiny YAML document:
//   refs:
//     COMMANDCODE_API_KEY: <secret>
//     COMMANDCODE_API_KEY_2: <secret>
// Only names are ever extracted for configuration; values are read on demand
// and handed straight to WinHTTP (never logged, never written anywhere).

bool ReadFileText(const fs::path& path, std::string& out) {
    if (path.empty()) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

fs::path DshCredentialsPath() {
    wchar_t profile[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return fs::path(std::wstring(profile, length)) / L".dsh" / L".credentials.yaml";
}

// Splits "  name: value" into its trimmed parts, unwrapping simple quotes.
bool SplitYamlEntry(const std::string& line, std::string& name, std::string& value) {
    std::string text = TrimAscii(line);
    if (text.empty() || text[0] == '#') return false;
    const size_t colon = text.find(':');
    if (colon == std::string::npos) return false;

    name = TrimAscii(text.substr(0, colon));
    value = TrimAscii(text.substr(colon + 1));
    if (!name.empty() && name[0] == '-') name = TrimAscii(name.substr(1));

    auto unquote = [](std::string& s) {
        if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                              (s.front() == '\'' && s.back() == '\''))) {
            s = s.substr(1, s.size() - 2);
        }
    };
    unquote(name);
    unquote(value);
    return !name.empty();
}

bool IsCcRefName(const std::string& name) {
    const std::string base = kRefBase;
    if (name == base) return true;
    if (name.size() <= base.size() + 1) return false;
    if (name.compare(0, base.size(), base) != 0) return false;
    if (name[base.size()] != '_') return false;
    for (size_t i = base.size() + 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

int RefRank(const std::wstring& name) {
    const std::wstring base = L"COMMANDCODE_API_KEY";
    if (name == base) return 1;
    return (int)std::wcstol(name.c_str() + base.size() + 1, nullptr, 10);
}

// --- JSON-ish scanning ---------------------------------------------------
//
// The /alpha endpoint returns a small JSON object. A full JSON parser would be
// overkill (and another dependency); this scanner only needs object spans and
// scalar values, with two safeguards against the response shape changing:
//   1. keys only match when followed by ':' (so "exceeded":"weekly" cannot be
//      mistaken for the weekly object), and
//   2. values are searched inside the matched object span only.

size_t SkipSpace(const std::string& body, size_t pos, size_t limit) {
    while (pos < limit) {
        const char c = body[pos];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++pos;
    }
    return pos;
}

bool FindKeyColon(const std::string& body, const char* key, size_t from, size_t limit,
                  size_t& colon) {
    const std::string pattern = std::string("\"") + key + "\"";
    size_t pos = body.find(pattern, from);
    while (pos != std::string::npos && pos < limit) {
        size_t after = SkipSpace(body, pos + pattern.size(), limit);
        if (after < limit && body[after] == ':') {
            colon = after;
            return true;
        }
        pos = body.find(pattern, pos + 1);
    }
    return false;
}

// Span of the object value of "key": [open, close] including both braces.
bool ObjectSpan(const std::string& body, const char* key, size_t from, size_t limit,
                size_t& open, size_t& close) {
    size_t colon = 0;
    if (!FindKeyColon(body, key, from, limit, colon)) return false;
    const size_t start = SkipSpace(body, colon + 1, limit);
    if (start >= limit || body[start] != '{') return false;

    int depth = 0;
    bool inString = false;
    for (size_t i = start; i < limit; ++i) {
        const char c = body[i];
        if (inString) {
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) {
                open = start;
                close = i;
                return true;
            }
        }
    }
    return false;
}

bool NumberIn(const std::string& body, const char* key, size_t from, size_t limit,
              double& out) {
    size_t colon = 0;
    if (!FindKeyColon(body, key, from, limit, colon)) return false;
    size_t pos = SkipSpace(body, colon + 1, limit);
    if (pos >= limit) return false;
    char* end = nullptr;
    const double value = std::strtod(body.c_str() + pos, &end);
    if (end == body.c_str() + pos) return false;
    out = value;
    return true;
}

bool BoolIn(const std::string& body, const char* key, size_t from, size_t limit, bool& out) {
    size_t colon = 0;
    if (!FindKeyColon(body, key, from, limit, colon)) return false;
    const size_t pos = SkipSpace(body, colon + 1, limit);
    if (pos >= limit) return false;
    if (body.compare(pos, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (body.compare(pos, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

// Epoch seconds are normalized to milliseconds; 0 stays "no window".
long long NormalizeEpoch(long long value) {
    if (value <= 0) return 0;
    if (value < 100000000000LL) return value * 1000;  // seconds
    return value;                                     // already milliseconds
}

// Howard Hinnant's days_from_civil, kept local so the resetAt parser accepts
// ISO-8601 timestamps without pulling in <chrono> calendar support.
long long DaysFromCivil(int year, int month, int day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy =
        (153u * (unsigned)(month + (month > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (long long)era * 146097LL + (long long)doe - 719468LL;
}

bool ReadDigits(const std::string& text, size_t& pos, int count, int& out) {
    if (pos + (size_t)count > text.size()) return false;
    int value = 0;
    for (int i = 0; i < count; ++i) {
        const char c = text[pos + (size_t)i];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    pos += (size_t)count;
    out = value;
    return true;
}

// Accepts "YYYY-MM-DDTHH:MM:SS[.fff][Z|+HH:MM]" (or a space instead of T).
// No zone is treated as UTC: the endpoint is a global service.
bool ParseIsoUtcMs(const std::string& text, long long& out) {
    size_t pos = 0;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!ReadDigits(text, pos, 4, year)) return false;
    if (pos >= text.size() || text[pos] != '-') return false;
    ++pos;
    if (!ReadDigits(text, pos, 2, month)) return false;
    if (pos >= text.size() || text[pos] != '-') return false;
    ++pos;
    if (!ReadDigits(text, pos, 2, day)) return false;
    if (pos >= text.size() || (text[pos] != 'T' && text[pos] != ' ')) return false;
    ++pos;
    if (!ReadDigits(text, pos, 2, hour)) return false;
    if (pos >= text.size() || text[pos] != ':') return false;
    ++pos;
    if (!ReadDigits(text, pos, 2, minute)) return false;
    if (pos < text.size() && text[pos] == ':') {
        ++pos;
        if (!ReadDigits(text, pos, 2, second)) return false;
    }

    long long millis = 0;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        int digits = 0;
        while (pos < text.size() && digits < 3 && text[pos] >= '0' && text[pos] <= '9') {
            millis = millis * 10 + (text[pos] - '0');
            ++digits;
            ++pos;
        }
        while (digits < 3) {
            millis *= 10;
            ++digits;
        }
        // Skip extra fractional digits, if any.
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    }

    long long offsetSeconds = 0;
    if (pos < text.size()) {
        if (text[pos] == 'Z' || text[pos] == 'z') {
            ++pos;
        } else if (text[pos] == '+' || text[pos] == '-') {
            const int sign = text[pos] == '-' ? -1 : 1;
            ++pos;
            int offsetHour = 0, offsetMinute = 0;
            if (!ReadDigits(text, pos, 2, offsetHour)) return false;
            if (pos < text.size() && text[pos] == ':') ++pos;
            if (pos < text.size() && !ReadDigits(text, pos, 2, offsetMinute)) return false;
            offsetSeconds = (long long)sign * (offsetHour * 3600LL + offsetMinute * 60LL);
        } else {
            return false;
        }
    }
    if (pos != text.size()) return false;

    if (month < 1 || month > 12 || day < 1 || day > 31) return false;
    if (hour > 23 || minute > 59 || second > 59) return false;

    const long long days = DaysFromCivil(year, month, day);
    const long long seconds = days * 86400LL + hour * 3600LL + minute * 60LL + second -
                              offsetSeconds;
    out = seconds * 1000LL + millis;
    return true;
}

bool ResetAtIn(const std::string& body, const char* key, size_t from, size_t limit,
               long long& out) {
    size_t colon = 0;
    if (!FindKeyColon(body, key, from, limit, colon)) return false;
    size_t pos = SkipSpace(body, colon + 1, limit);
    if (pos >= limit) return false;

    if (body[pos] == '"') {
        const size_t end = body.find('"', pos + 1);
        if (end == std::string::npos || end > limit) return false;
        const std::string text = body.substr(pos + 1, end - pos - 1);
        if (text.empty()) return false;
        if (text == "null") {
            out = 0;
            return true;
        }
        bool numeric = true;
        for (char c : text) {
            if (c < '0' || c > '9') {
                numeric = false;
                break;
            }
        }
        if (numeric) {
            out = NormalizeEpoch(std::strtoll(text.c_str(), nullptr, 10));
            return true;
        }
        return ParseIsoUtcMs(text, out);
    }
    if (pos + 4 <= limit && body.compare(pos, 4, "null") == 0) {
        out = 0;
        return true;
    }
    char* end = nullptr;
    const long long value = std::strtoll(body.c_str() + pos, &end, 10);
    if (end == body.c_str() + pos) return false;
    out = NormalizeEpoch(value);
    return true;
}

bool ParseWindowBody(const std::string& body, const char* name, size_t from, size_t limit,
                     QuotaWindow& window) {
    size_t open = 0, close = 0;
    if (!ObjectSpan(body, name, from, limit, open, close)) return false;

    double used = 0.0, cap = 0.0;
    const bool hasUsed = NumberIn(body, "used", open, close, used);
    const bool hasCap = NumberIn(body, "cap", open, close, cap);
    if (!hasUsed && !hasCap) return false;

    window.used = used;
    window.cap = cap;
    BoolIn(body, "exceeded", open, close, window.exceeded);
    ResetAtIn(body, "resetAt", open, close, window.resetAtMs);
    return true;
}

// --- Configuration -------------------------------------------------------

std::vector<AccountConfig> NormalizeAccounts(const std::vector<AccountConfig>& accounts) {
    std::vector<AccountConfig> out;
    const size_t count = std::min<size_t>(accounts.size(), (size_t)kMaxAccounts);
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        AccountConfig account = accounts[i];
        account.index = (int)i + 1;
        if (account.label.empty()) account.label = DefaultLabel(account.index);
        if (account.keyRef.empty()) account.keyRef = DefaultRef(account.index);
        if (account.monthlyLimit < 0.0) account.monthlyLimit = 0.0;
        out.push_back(std::move(account));
    }
    return out;
}

} // namespace

// --- Source helpers ------------------------------------------------------

const wchar_t* SourceKey(Source source) {
    return source == Source::Credman ? L"credman" : L"dsh";
}

const wchar_t* SourceTag(Source source) {
    return source == Source::Credman ? L"CRED" : L"DSH REF";
}

Source ParseSource(const std::wstring& text) {
    const std::wstring lowered = Lower(TrimWide(text));
    if (lowered == L"credman" || lowered == L"cred" || lowered == L"credential") {
        return Source::Credman;
    }
    return Source::Dsh;
}

// --- DSH credential file -------------------------------------------------

std::vector<std::wstring> DshCcRefNames(const std::string& credentialsYaml) {
    std::vector<std::wstring> names;
    size_t pos = 0;
    while (pos <= credentialsYaml.size()) {
        size_t end = credentialsYaml.find('\n', pos);
        if (end == std::string::npos) end = credentialsYaml.size();
        std::string name, value;
        if (SplitYamlEntry(credentialsYaml.substr(pos, end - pos), name, value) &&
            IsCcRefName(name)) {
            names.push_back(ToWide(name));
        }
        pos = end + 1;
    }

    std::sort(names.begin(), names.end(),
              [](const std::wstring& a, const std::wstring& b) { return RefRank(a) < RefRank(b); });
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::vector<std::wstring> LoadDshRefNames() {
    std::string text;
    if (!ReadFileText(DshCredentialsPath(), text)) return {};
    return DshCcRefNames(text);
}

std::wstring ReadDshRef(const std::wstring& refName) {
    if (refName.empty()) return {};

    std::string text;
    if (!ReadFileText(DshCredentialsPath(), text)) return {};

    const std::string wanted(refName.begin(), refName.end());
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string name, value;
        if (SplitYamlEntry(text.substr(pos, end - pos), name, value) && name == wanted) {
            // Block scalars and obvious placeholders are not usable keys.
            if (value.empty() || value[0] == '|' || value[0] == '>') return {};
            if (value.size() < 8) return {};
            return ToWide(value);
        }
        pos = end + 1;
    }
    return {};
}

// --- Configuration -------------------------------------------------------

bool HasAccountConfig(const wg::Store& config) { return config.Has(L"accounts"); }

std::vector<AccountConfig> ReadAccounts(const wg::Store& config) {
    if (!config.Has(L"accounts")) return {};

    int count = (int)config.GetInt(L"accounts", 0);
    if (count <= 0) return {};
    if (count > kMaxAccounts) count = kMaxAccounts;

    std::vector<AccountConfig> accounts;
    accounts.reserve((size_t)count);
    for (int index = 1; index <= count; ++index) {
        AccountConfig account;
        account.index = index;
        account.label = TrimWide(config.Get(AccountField(index, L"label").c_str(), L""));
        if (account.label.empty()) account.label = DefaultLabel(index);
        account.keyRef = TrimWide(config.Get(AccountField(index, L"keyref").c_str(), L""));
        if (account.keyRef.empty()) account.keyRef = DefaultRef(index);
        account.source = ParseSource(config.Get(AccountField(index, L"source").c_str(), L"dsh"));
        account.monthlyLimit =
            ParseDouble(config.Get(AccountField(index, L"monthly_limit").c_str(), L""), 0.0);
        if (account.monthlyLimit < 0.0) account.monthlyLimit = 0.0;
        accounts.push_back(std::move(account));
    }
    return accounts;
}

std::vector<AccountConfig> ImportAccounts(const std::string& credentialsYaml) {
    const std::vector<std::wstring> refs = DshCcRefNames(credentialsYaml);
    std::vector<AccountConfig> accounts;
    const size_t count = std::min<size_t>(refs.size(), (size_t)kMaxAccounts);
    accounts.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        AccountConfig account;
        account.index = (int)i + 1;
        account.label = DefaultLabel(account.index);
        account.keyRef = refs[i];
        account.source = Source::Dsh;
        accounts.push_back(std::move(account));
    }
    return accounts;
}

std::vector<AccountConfig> LoadAccounts(wg::Store& config) {
    if (config.Has(L"accounts")) return ReadAccounts(config);

    // First run: adopt the CommandCode refs already present in the DSH
    // credential store. The written accounts= key makes this a one-time
    // migration; refs added later need an explicit config edit.
    std::vector<AccountConfig> imported;
    std::string yaml;
    if (ReadFileText(DshCredentialsPath(), yaml)) imported = ImportAccounts(yaml);
    if (imported.empty()) {
        // No refs found: stay unconfigured instead of persisting accounts=0,
        // so a later ~/.dsh update can still be imported.
        return {};
    }

    SaveAccounts(config, imported);
    if (!config.Save()) {
        wg::Log(wg::LogLevel::Warn, kLogCategory, L"could not persist imported account config");
    } else {
        wg::Log(wg::LogLevel::Info, kLogCategory,
                L"imported " + std::to_wstring(imported.size()) + L" CommandCode account(s)");
    }
    return imported;
}

void SaveAccounts(wg::Store& config, const std::vector<AccountConfig>& accounts) {
    config.RemovePrefix(L"account.");
    const std::vector<AccountConfig> normalized = NormalizeAccounts(accounts);
    config.SetInt(L"accounts", (long long)normalized.size());
    for (size_t i = 0; i < normalized.size(); ++i) {
        const AccountConfig& account = normalized[i];
        const int index = (int)i + 1;
        config.Set(AccountField(index, L"label").c_str(), account.label);
        config.Set(AccountField(index, L"keyref").c_str(), account.keyRef);
        config.Set(AccountField(index, L"source").c_str(), SourceKey(account.source));
        config.Set(AccountField(index, L"monthly_limit").c_str(),
                   account.monthlyLimit > 0.0 ? FormatDouble(account.monthlyLimit) : L"");
    }
}

// --- Credentials and fetch ----------------------------------------------

std::wstring CredentialTarget(const std::wstring& label) {
    return std::wstring(kCredPrefix) + L"/" + label;
}

std::wstring ResolveAccountKey(const AccountConfig& account, bool legacyFallback) {
    std::wstring label = account.label;
    if (label.empty()) label = DefaultLabel(account.index > 0 ? account.index : 1);

    const std::wstring dsh = account.keyRef.empty() ? std::wstring() : ReadDshRef(account.keyRef);
    const std::wstring cred = wg::LoadSecret(CredentialTarget(label));

    std::wstring key = account.source == Source::Dsh
                           ? (dsh.empty() ? cred : dsh)
                           : (cred.empty() ? dsh : cred);

    // Account 1 only: the pre-multi-account build stored the key here.
    if (key.empty() && legacyFallback && account.index <= 1) {
        key = wg::LoadSecret(kCredPrefix);
    }
    return key;
}

AccountSnapshot FetchAccount(const AccountConfig& account, const std::wstring& key,
                             long long nowMs) {
    AccountSnapshot snapshot;
    snapshot.label = account.label;
    snapshot.keyRef = account.keyRef;
    snapshot.source = account.source;
    snapshot.monthlyLimit = account.monthlyLimit;

    if (key.empty()) {
        snapshot.error = kErrNoKey;
        return snapshot;
    }

    const wg::HttpResponse response =
        wg::HttpGet(kCredHostW, kCredPath, key, kUserAgent);
    if (!response.transport) {
        snapshot.error = kErrNet;
        return snapshot;
    }
    if (response.status == 401 || response.status == 403) {
        snapshot.error = kErrBadKey;
        return snapshot;
    }
    if (response.status != 200) {
        snapshot.error = std::wstring(L"ERR ") + std::to_wstring((unsigned long long)response.status);
        return snapshot;
    }

    AccountSnapshot parsed = ParseAccountBody(response.body, nowMs);
    if (!parsed.ok) {
        snapshot.error = parsed.error;
        return snapshot;
    }
    parsed.label = account.label;
    parsed.keyRef = account.keyRef;
    parsed.source = account.source;
    parsed.monthlyLimit = account.monthlyLimit;
    return parsed;
}

AccountBoardData FetchAccounts(const std::vector<AccountConfig>& accounts, long long nowMs) {
    std::vector<AccountSnapshot> snapshots(accounts.size());
    if (!accounts.empty()) {
        const size_t workerCount = std::min<size_t>(accounts.size(), (size_t)kMaxAccounts);
        std::atomic<size_t> next{0};

        auto worker = [&]() {
            for (;;) {
                const size_t index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= accounts.size()) return;
                const AccountConfig& account = accounts[index];
                const std::wstring key = ResolveAccountKey(account);
                snapshots[index] = FetchAccount(account, key, nowMs);
                if (!snapshots[index].ok) {
                    // Label + source only: no credential material is ever logged.
                    wg::Log(wg::LogLevel::Warn, kLogCategory,
                            L"account " + account.label + L" (" + SourceTag(account.source) +
                                L"): " + snapshots[index].error);
                }
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (size_t i = 0; i < workerCount; ++i) workers.emplace_back(worker);
        for (std::thread& thread : workers) thread.join();
    }
    return Aggregate(std::move(snapshots));
}

// --- Parsing -------------------------------------------------------------

AccountSnapshot ParseAccountBody(const std::string& body, long long nowMs) {
    AccountSnapshot snapshot;
    snapshot.asOfMs = nowMs;

    const size_t size = body.size();
    size_t limitsOpen = 0, limitsClose = 0;
    const bool haveLimits = ObjectSpan(body, "windowLimits", 0, size, limitsOpen, limitsClose);
    const size_t windowFrom = haveLimits ? limitsOpen : 0;
    const size_t windowTo = haveLimits ? limitsClose : size;

    const bool hasFive = ParseWindowBody(body, "fiveHour", windowFrom, windowTo,
                                         snapshot.fiveHour) ||
                         ParseWindowBody(body, "five_hour", windowFrom, windowTo,
                                         snapshot.fiveHour);
    const bool hasWeek =
        ParseWindowBody(body, "weekly", windowFrom, windowTo, snapshot.weekly);
    if (!hasFive && !hasWeek) {
        snapshot.error = kErrNoData;
        return snapshot;
    }

    size_t creditsOpen = 0, creditsClose = 0;
    if (ObjectSpan(body, "credits", 0, size, creditsOpen, creditsClose)) {
        double value = 0.0;
        if (NumberIn(body, "monthlyCredits", creditsOpen, creditsClose, value) ||
            NumberIn(body, "monthly_credits", creditsOpen, creditsClose, value)) {
            snapshot.monthlyCredits = value;
        }
        if (NumberIn(body, "creditThreshold", creditsOpen, creditsClose, value)) {
            snapshot.creditThreshold = value;
        }
        if (!BoolIn(body, "belowThreshold", creditsOpen, creditsClose,
                    snapshot.belowThreshold) &&
            snapshot.creditThreshold > 0.0) {
            snapshot.belowThreshold = snapshot.monthlyCredits < snapshot.creditThreshold;
        }
    } else {
        double value = 0.0;
        if (NumberIn(body, "monthlyCredits", 0, size, value)) {
            snapshot.monthlyCredits = value;
        }
    }

    size_t sandboxOpen = 0, sandboxClose = 0;
    if (ObjectSpan(body, "sandboxMinutes", 0, size, sandboxOpen, sandboxClose)) {
        double value = 0.0;
        if (NumberIn(body, "usedMinutes", sandboxOpen, sandboxClose, value)) {
            snapshot.sandboxUsedMinutes = (long long)std::llround(value);
        }
        if (NumberIn(body, "limitMinutes", sandboxOpen, sandboxClose, value)) {
            snapshot.sandboxLimitMinutes = (long long)std::llround(value);
        }
        if (NumberIn(body, "overageUsd", sandboxOpen, sandboxClose, value)) {
            snapshot.sandboxOverageUsd = value;
        }
        ResetAtIn(body, "resetAt", sandboxOpen, sandboxClose, snapshot.sandboxResetMs);
    }

    snapshot.ok = true;
    return snapshot;
}

// --- Aggregation ---------------------------------------------------------

AccountBoardData Aggregate(std::vector<AccountSnapshot> snapshots) {
    AccountBoardData data;
    data.accounts = std::move(snapshots);

    for (const AccountSnapshot& account : data.accounts) {
        if (account.ok) data.anyOk = true;
        // A failed account that kept its last good numbers still contributes:
        // hiding it would understate the tightest window and the monthly total.
        if (account.asOfMs <= 0) continue;

        if (account.fiveHour.cap > 0.0) {
            const double percent = account.fiveHour.percent();
            if (percent > data.tightestPct) data.tightestPct = percent;
        }
        if (account.weekly.cap > 0.0) {
            const double percent = account.weekly.percent();
            if (percent > data.tightestPct) data.tightestPct = percent;
        }
        data.monthlyCreditsTotal += account.monthlyCredits;
    }
    return data;
}

AccountSnapshot CarryForward(const AccountSnapshot& fresh, const AccountSnapshot& previous) {
    if (fresh.ok || previous.asOfMs <= 0) return fresh;

    AccountSnapshot carried = previous;
    // Identity and config may have changed (renamed label, new manual limit);
    // the numbers stay those of the last successful fetch.
    carried.label = fresh.label;
    carried.keyRef = fresh.keyRef;
    carried.source = fresh.source;
    carried.monthlyLimit = fresh.monthlyLimit;
    carried.ok = false;
    carried.error = fresh.error;
    return carried;
}

// --- UI helpers ----------------------------------------------------------

bool AllAccountsOk(const AccountBoardData& data) {
    if (data.accounts.empty()) return false;
    for (const AccountSnapshot& account : data.accounts) {
        if (!account.ok) return false;
    }
    return true;
}

bool IsStale(long long asOfMs, long long nowMs, long long staleAfterMs) {
    if (asOfMs <= 0) return true;  // never fetched successfully
    const long long age = nowMs - asOfMs;
    if (age < 0) return false;  // clock skew: the sample is in the future
    return age > staleAfterMs;
}

std::wstring AgeText(long long ageMs) {
    if (ageMs < 0) ageMs = 0;
    const long long seconds = ageMs / 1000;
    if (seconds < 60) return L"<1M";
    const long long minutes = seconds / 60;
    if (minutes < 60) return std::to_wstring(minutes) + L"M";
    const long long hours = minutes / 60;
    if (hours < 24) {
        return std::to_wstring(hours) + L"H " + std::to_wstring(minutes % 60) + L"M";
    }
    const long long days = hours / 24;
    return std::to_wstring(days) + L"D " + std::to_wstring(hours % 24) + L"H";
}

bool HasReset(const QuotaWindow& window) { return window.resetAtMs > 0; }

std::wstring ResetCountdown(long long resetAtMs, long long nowMs) {
    if (resetAtMs <= 0) return {};  // no window: the UI must not say "reset"

    long long millis = resetAtMs - (nowMs > 0 ? nowMs : 0);
    if (millis <= 0) return L"NOW";
    const long long seconds = millis / 1000;
    if (seconds < 60) return L"<1M";

    const long long minutes = seconds / 60;
    if (minutes < 60) return std::to_wstring(minutes) + L"M";

    const long long hours = minutes / 60;
    if (hours < 24) {
        return std::to_wstring(hours) + L"H " + std::to_wstring(minutes % 60) + L"M";
    }
    return std::to_wstring(hours / 24) + L"D " + std::to_wstring(hours % 24) + L"H";
}

std::wstring PercentText(const QuotaWindow& window) {
    if (window.cap <= 0.0) return L"--";
    const int percent = (int)std::floor(window.percent() + 1e-9);
    return std::to_wstring(percent) + L"%";
}

std::wstring CreditsText(double credits) {
    wchar_t buffer[64];
    std::swprintf(buffer, 64, L"%.2f", credits);
    return buffer;
}

std::wstring AggregateText(const AccountBoardData& data) {
    if (data.accounts.empty()) return L"NO ACCOUNTS";
    const std::wstring count = std::to_wstring(data.accounts.size());
    if (!data.anyOk) return count + L" ACCTS / ERROR";

    const std::wstring tight =
        data.tightestPct > 0.0
            ? std::to_wstring((int)std::floor(data.tightestPct + 1e-9)) + L"%"
            : std::wstring(L"--");
    return count + L" ACCTS / TIGHT " + tight + L" / MONTH " +
           CreditsText(data.monthlyCreditsTotal);
}

} // namespace aimeter
