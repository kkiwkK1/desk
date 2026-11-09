#include "history.h"

#include "widget/appenv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aimeter {
namespace {

constexpr const wchar_t* kAppTitle = L"Desktop Desk";
constexpr const wchar_t* kHistoryFile = L"aimeter-history.ini";
constexpr const wchar_t* kDayRoot = L"day.";

bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysInMonth(int year, int month) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && IsLeapYear(year)) return 29;
    return kDays[month - 1];
}

// Howard Hinnant's civil-date algorithms: deterministic, timezone- and
// locale-free, and valid for every year this widget can live to see.
long long DaysFromCivil(int year, int month, int day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy =
        (153u * (unsigned)(month + (month > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (long long)era * 146097LL + (long long)doe - 719468LL;
}

void CivilFromDays(long long days, int& year, int& month, int& day) {
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    const unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned m = mp + (mp < 10u ? 3u : (unsigned)-9);
    year = (int)((long long)yoe + era * 400LL + (m <= 2u ? 1 : 0));
    month = (int)m;
    day = (int)d;
}

std::wstring MakeDate(int year, int month, int day) {
    wchar_t buffer[16];
    std::swprintf(buffer, 16, L"%04d-%02d-%02d", year, month, day);
    return buffer;
}

bool ParseKeyIndex(const std::wstring& key, const std::wstring& dayPrefix, int& index) {
    // day.<date>.cc.<i>.<window>.<part>
    const std::wstring head = dayPrefix + L"cc.";
    if (key.size() <= head.size() || key.compare(0, head.size(), head) != 0) return false;
    size_t pos = head.size();
    int value = 0;
    bool any = false;
    while (pos < key.size() && key[pos] >= L'0' && key[pos] <= L'9') {
        value = value * 10 + (key[pos] - L'0');
        any = true;
        ++pos;
    }
    if (!any || pos >= key.size() || key[pos] != L'.') return false;
    index = value;
    return true;
}

unsigned long long ParseCount(const std::wstring& text) {
    if (text.empty()) return 0;
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text.c_str(), &end, 10);
    if (end == text.c_str()) return 0;
    return value;
}

} // namespace

// --- Paths ---------------------------------------------------------------

std::wstring HistoryPath() {
    return wg::appenv::ConfigPath(kHistoryFile, kAppTitle);
}

bool HistoryOpen(wg::Store& history) { return HistoryOpen(history, HistoryPath()); }

bool HistoryOpen(wg::Store& history, const std::wstring& path) {
    if (path.empty()) return false;
    // Open() reports false when the file does not exist yet; that is still a
    // usable store (the first SaveDay creates it), so only an empty path fails.
    history.Open(path);
    return true;
}

// --- Key builders --------------------------------------------------------

std::wstring DayPrefix(const std::wstring& date) {
    return std::wstring(kDayRoot) + date + L".";
}

std::wstring DayKey(const std::wstring& date, const wchar_t* suffix) {
    return DayPrefix(date) + suffix;
}

std::wstring AccountDayKey(const std::wstring& date, int index, const wchar_t* window,
                           const wchar_t* part) {
    return DayPrefix(date) + L"cc." + std::to_wstring(index) + L"." + window + L"." + part;
}

std::wstring TokenDayKey(const std::wstring& date, const wchar_t* part) {
    return DayPrefix(date) + L"tokens." + part;
}

// --- Writing -------------------------------------------------------------

void WriteDay(wg::Store& history, const HistoryDay& day) {
    if (day.date.empty()) return;

    // Replace the whole day: if an account was removed, its old keys must not
    // survive inside the same date.
    history.RemovePrefix(DayPrefix(day.date));

    for (size_t i = 0; i < day.accounts.size(); ++i) {
        const HistoryAccount& account = day.accounts[i];
        const int index = (int)i + 1;
        history.Set(AccountDayKey(day.date, index, L"five", L"used").c_str(),
                    FormatNumber(account.five.used));
        history.Set(AccountDayKey(day.date, index, L"five", L"cap").c_str(),
                    FormatNumber(account.five.cap));
        history.Set(AccountDayKey(day.date, index, L"week", L"used").c_str(),
                    FormatNumber(account.week.used));
        history.Set(AccountDayKey(day.date, index, L"week", L"cap").c_str(),
                    FormatNumber(account.week.cap));
        history.Set(AccountDayKey(day.date, index, L"monthly", L"value").c_str(),
                    FormatNumber(account.monthly));
        history.Remove(AccountDayKey(day.date, index, L"monthly", L"value").c_str());
        history.Set(DayKey(day.date, (L"cc." + std::to_wstring(index) + L".monthly").c_str())
                        .c_str(),
                    FormatNumber(account.monthly));
    }

    // Tokens are global to this machine; a day without a readable DSH log is
    // simply not recorded, so an older value is never overwritten by zeros.
    if (day.tokensOk) {
        history.Set(TokenDayKey(day.date, L"input").c_str(),
                    std::to_wstring(day.tokens.uncachedInput));
        history.Set(TokenDayKey(day.date, L"output").c_str(),
                    std::to_wstring(day.tokens.output));
        history.Set(TokenDayKey(day.date, L"cacheRead").c_str(),
                    std::to_wstring(day.tokens.cacheRead));
        history.Set(TokenDayKey(day.date, L"cacheWrite").c_str(),
                    std::to_wstring(day.tokens.cacheWrite));
    }
}

int Trim(wg::Store& history, const std::wstring& today, int keepDays) {
    if (today.empty()) return 0;
    if (keepDays <= 0) keepDays = kHistoryKeepDays;

    const std::map<std::wstring, std::wstring> values = history.Snapshot();
    std::set<std::wstring> dates;
    for (const auto& entry : values) {
        const std::wstring& key = entry.first;
        if (key.size() < 14 || key.compare(0, 4, kDayRoot) != 0) continue;
        const std::wstring date = key.substr(4, 10);
        int year = 0, month = 0, day = 0;
        if (ParseDate(date, &year, &month, &day)) dates.insert(date);
    }

    int removed = 0;
    for (const std::wstring& date : dates) {
        if (DateDiffDays(date, today) >= keepDays) {
            removed += history.RemovePrefix(DayPrefix(date));
        }
    }
    return removed;
}

bool SaveDay(wg::Store& history, const HistoryDay& day, const std::wstring& today,
             int keepDays) {
    if (day.date.empty()) return false;
    WriteDay(history, day);
    Trim(history, today.empty() ? day.date : today, keepDays);
    return history.Save();
}

// --- Reading -------------------------------------------------------------

std::vector<HistoryDay> ReadRecent(const wg::Store& history, const std::wstring& today,
                                   int days) {
    std::vector<HistoryDay> out;
    if (days <= 0 || today.empty()) return out;

    const std::map<std::wstring, std::wstring> values = history.Snapshot();
    auto find = [&values](const std::wstring& key) -> const std::wstring* {
        const auto it = values.find(key);
        return it == values.end() ? nullptr : &it->second;
    };
    auto number = [&](const std::wstring& key, double& out) -> bool {
        const std::wstring* text = find(key);
        if (!text) return false;
        out = ParseNumber(*text, 0.0);
        return true;
    };

    for (int back = days - 1; back >= 0; --back) {
        const std::wstring date = DateAddDays(today, -back);
        const std::wstring prefix = DayPrefix(date);

        HistoryDay day;
        day.date = date;

        // Which accounts were recorded that day (never assume a fixed count).
        std::set<int> indexes;
        for (const auto& entry : values) {
            int index = 0;
            if (ParseKeyIndex(entry.first, prefix, index) && index > 0) indexes.insert(index);
        }

        for (const int index : indexes) {
            HistoryAccount account;
            number(AccountDayKey(date, index, L"five", L"used"), account.five.used);
            number(AccountDayKey(date, index, L"five", L"cap"), account.five.cap);
            number(AccountDayKey(date, index, L"week", L"used"), account.week.used);
            number(AccountDayKey(date, index, L"week", L"cap"), account.week.cap);
            number(DayKey(date, (L"cc." + std::to_wstring(index) + L".monthly").c_str()),
                   account.monthly);
            day.accounts.push_back(account);
        }

        const std::wstring* input = find(TokenDayKey(date, L"input"));
        const std::wstring* output = find(TokenDayKey(date, L"output"));
        const std::wstring* cacheRead = find(TokenDayKey(date, L"cacheRead"));
        const std::wstring* cacheWrite = find(TokenDayKey(date, L"cacheWrite"));
        if (input || output || cacheRead || cacheWrite) {
            day.tokensOk = true;
            if (input) day.tokens.uncachedInput = ParseCount(*input);
            if (output) day.tokens.output = ParseCount(*output);
            if (cacheRead) day.tokens.cacheRead = ParseCount(*cacheRead);
            if (cacheWrite) day.tokens.cacheWrite = ParseCount(*cacheWrite);
        }

        // A day without any key is a gap, not a zero row.
        if (!day.empty()) out.push_back(std::move(day));
    }
    return out;
}

std::vector<HistoryPoint> WeeklyUsageSeries(const wg::Store& history,
                                            const std::wstring& today, int days) {
    if (days <= 0) days = kHistoryChartDays;
    const std::vector<HistoryDay> recent = ReadRecent(history, today, days);

    std::vector<HistoryPoint> points;
    points.reserve((size_t)days);
    for (int back = days - 1; back >= 0; --back) {
        HistoryPoint point;
        point.date = DateAddDays(today, -back);

        for (const HistoryDay& day : recent) {
            if (day.date != point.date) continue;
            double best = -1.0;
            for (const HistoryAccount& account : day.accounts) {
                if (account.week.cap <= 0.0) continue;
                const double percent = account.week.used * 100.0 / account.week.cap;
                if (percent > best) best = percent;
            }
            if (best >= 0.0) point.value = (float)best;
            break;
        }
        points.push_back(std::move(point));
    }
    return points;
}

// --- Date math -----------------------------------------------------------

bool ParseDate(const std::wstring& date, int* year, int* month, int* day) {
    if (date.size() != 10 || date[4] != L'-' || date[7] != L'-') return false;
    int parts[3] = {0, 0, 0};
    const size_t starts[3] = {0, 5, 8};
    const size_t lengths[3] = {4, 2, 2};
    for (int i = 0; i < 3; ++i) {
        for (size_t c = 0; c < lengths[i]; ++c) {
            const wchar_t ch = date[starts[i] + c];
            if (ch < L'0' || ch > L'9') return false;
            parts[i] = parts[i] * 10 + (ch - L'0');
        }
    }
    const int y = parts[0], m = parts[1], d = parts[2];
    if (y < 1 || y > 9999) return false;
    if (m < 1 || m > 12) return false;
    if (d < 1 || d > DaysInMonth(y, m)) return false;
    if (year) *year = y;
    if (month) *month = m;
    if (day) *day = d;
    return true;
}

std::wstring DateAddDays(const std::wstring& date, int delta) {
    int year = 0, month = 0, day = 0;
    if (!ParseDate(date, &year, &month, &day)) return {};
    const long long shifted = DaysFromCivil(year, month, day) + delta;
    int outYear = 0, outMonth = 0, outDay = 0;
    CivilFromDays(shifted, outYear, outMonth, outDay);
    return MakeDate(outYear, outMonth, outDay);
}

long long DateDiffDays(const std::wstring& from, const std::wstring& to) {
    int fromYear = 0, fromMonth = 0, fromDay = 0;
    int toYear = 0, toMonth = 0, toDay = 0;
    if (!ParseDate(from, &fromYear, &fromMonth, &fromDay)) return 0;
    if (!ParseDate(to, &toYear, &toMonth, &toDay)) return 0;
    return DaysFromCivil(toYear, toMonth, toDay) - DaysFromCivil(fromYear, fromMonth, fromDay);
}

// --- Formatting ----------------------------------------------------------

std::wstring FormatNumber(double value) {
    if (!std::isfinite(value)) return L"0";
    wchar_t buffer[64];
    std::swprintf(buffer, 64, L"%.6g", value);
    return buffer;
}

double ParseNumber(const std::wstring& text, double fallback) {
    if (text.empty()) return fallback;
    wchar_t* end = nullptr;
    const double value = std::wcstod(text.c_str(), &end);
    if (end == text.c_str()) return fallback;
    return value;
}

std::wstring TodayDate() { return LocalDay(); }

} // namespace aimeter
