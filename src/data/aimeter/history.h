// Daily snapshots for the Account card's 7-day mini chart (M0a).
//
// One UTF-16 key=value file (aimeter-history.ini) in the app config
// directory, written through wg::Store's temp-file + rename atomic save. Keys
// follow docs/assistant-plan.md §4.4:
//
//   day.<YYYY-MM-DD>.cc.<i>.five.used / .cap
//   day.<YYYY-MM-DD>.cc.<i>.week.used / .cap
//   day.<YYYY-MM-DD>.cc.<i>.monthly
//   day.<YYYY-MM-DD>.tokens.input / output / cacheRead / cacheWrite
//
// Writes happen on the refresh worker; the UI only consumes HistoryDay /
// HistoryPoint snapshots. Days without a snapshot stay missing - they are
// never zero-filled (a zero would read as "used nothing" instead of "unknown").
#pragma once

#include "token_usage.h"
#include "widget/store.h"

#include <string>
#include <vector>

namespace aimeter {

// Keep 90 days (today plus the previous 89); the file stays well under 100KB.
constexpr int kHistoryKeepDays = 90;
constexpr int kHistoryChartDays = 7;

struct HistoryWindow {
    double used = 0.0;
    double cap = 0.0;
};

struct HistoryAccount {
    HistoryWindow five;
    HistoryWindow week;
    double monthly = 0.0;
};

struct HistoryDay {
    std::wstring date;                     // YYYY-MM-DD, local time
    std::vector<HistoryAccount> accounts;  // [0] -> cc.1
    TokenUsage tokens;                     // today's local DSH tokens
    bool tokensOk = false;                 // write tokens only if the log was readable

    bool empty() const { return accounts.empty() && !tokensOk; }
};

// One chart slot: value < 0 means "no snapshot for that day" (leave the bar
// blank). value is the tightest weekly usage percentage of that day.
struct HistoryPoint {
    std::wstring date;
    float value = -1.0f;
};

// --- Paths ---------------------------------------------------------------
// <config dir>/aimeter-history.ini (same directory as aimeter.ini).
std::wstring HistoryPath();
bool HistoryOpen(wg::Store& history);                               // Open(HistoryPath())
bool HistoryOpen(wg::Store& history, const std::wstring& path);

// --- Key builders (pure, shared with the tests) --------------------------
std::wstring DayPrefix(const std::wstring& date);                    // "day.<date>."
std::wstring DayKey(const std::wstring& date, const wchar_t* suffix);
std::wstring AccountDayKey(const std::wstring& date, int index,
                           const wchar_t* window, const wchar_t* part);
std::wstring TokenDayKey(const std::wstring& date, const wchar_t* part);

// --- Writing -------------------------------------------------------------
// Replaces that day's keys (a shrinking account list cannot leave orphans).
// Does not save; callers write a day once per refresh and save at the end.
void WriteDay(wg::Store& history, const HistoryDay& day);
// Removes dates older than keepDays relative to 'today'; returns keys removed.
int Trim(wg::Store& history, const std::wstring& today, int keepDays = kHistoryKeepDays);
// WriteDay + Trim + Store::Save, in that order (one atomic file replace).
bool SaveDay(wg::Store& history, const HistoryDay& day, const std::wstring& today,
             int keepDays = kHistoryKeepDays);

// --- Reading (worker thread only) ---------------------------------------
// Newest 'days' days ending at 'today' (inclusive), oldest first. Days with
// no snapshot are omitted, so a gap stays a gap.
std::vector<HistoryDay> ReadRecent(const wg::Store& history, const std::wstring& today,
                                   int days);
// Exactly 'days' points, oldest -> newest (today last); missing = value < 0.
std::vector<HistoryPoint> WeeklyUsageSeries(const wg::Store& history,
                                            const std::wstring& today,
                                            int days = kHistoryChartDays);

// --- Date math (pure) ----------------------------------------------------
bool ParseDate(const std::wstring& date, int* year, int* month, int* day);
std::wstring DateAddDays(const std::wstring& date, int delta);
long long DateDiffDays(const std::wstring& from, const std::wstring& to);  // to - from

// --- Value formatting ----------------------------------------------------
std::wstring FormatNumber(double value);                    // "5.4077", "14"
double ParseNumber(const std::wstring& text, double fallback = 0.0);
std::wstring TodayDate();                                   // local YYYY-MM-DD

} // namespace aimeter
