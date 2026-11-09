#include "collect.h"

#include "accounts.h"
#include "history.h"
#include "providers.h"
#include "token_usage.h"

#include "widget/secrets.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

namespace aimeter {
namespace {

constexpr const wchar_t* kCredTarget = L"AiMeter/DeepSeek";
constexpr const wchar_t* kHistoryFile = L"aimeter-history.ini";

// Updates the daily balance snapshot and returns today's spend in cents.
// The actual ini write happens once, at the end of Refresh.
long long SpendToday(long long cents, wg::Store& store) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t today[16];
    swprintf(today, 16, L"%04d-%02d-%02d", now.wYear, now.wMonth, now.wDay);

    std::wstring day = store.Get(L"day");
    long long start = (long long)store.GetInt(L"start", 0);
    long long last = (long long)store.GetInt(L"last", 0);

    // Day rollover: yesterday's closing balance becomes today's opening one,
    // so a machine that was off overnight still reports today's spend.
    if (day != today) {
        day = today;
        start = last > 0 ? last : cents;
    }
    last = cents;
    store.Set(L"day", day);
    store.SetInt(L"start", start);
    store.SetInt(L"last", last);
    return start - cents;
}

std::wstring SpendText(long long spent) {
    const wchar_t* sign = spent > 0 ? L"-" : (spent < 0 ? L"+" : L"");
    const long long magnitude = spent < 0 ? -spent : spent;
    return std::wstring(L"TODAY ") + sign + Money(magnitude);
}

// history file next to aimeter.ini (same directory, different name)
std::wstring HistoryPathFor(const wg::Store& store) {
    const std::wstring& path = store.path();
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return kHistoryFile;
    return path.substr(0, slash + 1) + kHistoryFile;
}

wg::Row CodexRow(const wg::Theme& theme) {
    wg::Row row{L"CODEX", L"...", L"", -1.0f, 0, L"", 0};
    float primary = 0.0f, secondary = 0.0f;
    if (ReadCodex(primary, secondary)) {
        wchar_t value[64];
        swprintf(value, 64, L"%d%% / %d%%", (int)(primary + 0.5f), (int)(secondary + 0.5f));
        row.value = value;
        row.frac = primary / 100.0f;
        row.accent = primary >= 90 ? theme.bad : (primary >= 70 ? theme.warn : 0);
    } else {
        row.value = L"NO DATA";
        row.frac = -1.0f;
    }
    return row;
}

wg::Row DeepseekRow(const AimeterData& data, const wg::Theme& theme) {
    wg::Row row{L"DEEPSEEK", data.balanceText, L"", -1.0f, 0, L"", 0};
    if (data.balanceText.empty() || data.balanceText == L"...") row.value = L"...";
    row.accent = data.balanceOk ? 0 : theme.bad;
    row.frac = -1.0f;
    if (data.balanceOk) row.noteLeft = SpendText(data.spendCents);
    if (data.tokensOk) {
        row.note = FormatTokens(data.tokens.total()) + L" TOK  LOCAL";
        row.noteColor = theme.label;
    }
    return row;
}

long long NowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER v{};
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    // 100ns since 1601 -> ms since 1970
    return (long long)(v.QuadPart / 10000ULL) - 11644473600000LL;
}

} // namespace

std::vector<wg::Row> InitialRows() {
    // First paint before Refresh: DeepSeek is opt-in, so it must not flash in.
    return {
        wg::Row{L"CODEX", L"...", L"", -1.0f, 0, L"", 0},
        wg::Row{L"COMMANDCODE", L"...", L"", -1.0f, 0, L"", 0},
    };
}

void Refresh(AimeterData& data, wg::Store& store, const wg::Theme& theme) {
    data.showDeepseek = store.GetInt(L"show_deepseek", 0) != 0;

    // --- DeepSeek (opt-in) -------------------------------------------------
    if (data.showDeepseek) {
        const std::wstring key = wg::LoadSecret(kCredTarget);
        data.balanceOk = false;
        data.balanceCents = 0;
        data.spendCents = 0;
        data.balanceText.clear();

        if (key.empty()) {
            data.balanceText = L"NO KEY";
        } else {
            const Balance balance = FetchDeepSeek(key);
            data.balanceText = balance.text;
            data.balanceOk = balance.ok;
            if (balance.ok) {
                data.balanceCents = balance.cents;
                data.spendCents = SpendToday(balance.cents, store);
            }
        }
    }

    // --- local DSH tokens (all providers, this machine) --------------------
    std::vector<DshSessionUsage> sessions;
    if (ReadDshSessions(sessions)) {
        data.tokensOk = true;
        data.tokens = UpdateTodayTokens(sessions, store, LocalDay());
    } else {
        data.tokensOk = false;
        data.tokens = {};
    }

    // --- CommandCode: N accounts, failures isolated ------------------------
    const long long nowMs = NowMs();
    const std::vector<AccountConfig> configs = LoadAccounts(store);
    AccountBoardData fresh = FetchAccounts(configs, nowMs);

    const std::vector<AccountSnapshot> previous = data.accounts.accounts;
    for (size_t i = 0; i < fresh.accounts.size(); ++i) {
        if (fresh.accounts[i].ok) continue;
        for (const AccountSnapshot& old : previous) {
            if (old.label == fresh.accounts[i].label && old.asOfMs > 0) {
                fresh.accounts[i] = CarryForward(fresh.accounts[i], old);
                break;
            }
        }
    }
    data.accounts = fresh;

    long long asOf = 0;
    for (const AccountSnapshot& account : data.accounts.accounts) {
        asOf = std::max(asOf, account.asOfMs);
    }
    data.accountsAsOfMs = asOf;

    // --- daily history + 7-day series -------------------------------------
    static wg::Store history;
    static std::wstring historyPath;
    const std::wstring wanted = HistoryPathFor(store);
    if (historyPath != wanted) {
        historyPath = wanted;
        HistoryOpen(history, historyPath);
    }

    HistoryDay day;
    day.date = TodayDate();
    day.accounts.resize(data.accounts.accounts.size());
    for (size_t i = 0; i < data.accounts.accounts.size(); ++i) {
        const AccountSnapshot& account = data.accounts.accounts[i];
        if (!account.ok) continue;
        day.accounts[i].five = HistoryWindow{account.fiveHour.used, account.fiveHour.cap};
        day.accounts[i].week = HistoryWindow{account.weekly.used, account.weekly.cap};
        day.accounts[i].monthly = account.monthlyCredits;
    }
    day.tokens = data.tokens;
    day.tokensOk = data.tokensOk;
    SaveDay(history, day, day.date);
    data.weekly = WeeklyUsageSeries(history, day.date, kHistoryChartDays);

    // --- rows: provider rows only; accounts are rendered per account ------
    data.rows.clear();
    if (data.showDeepseek) data.rows.push_back(DeepseekRow(data, theme));
    data.rows.push_back(CodexRow(theme));

    store.Save();
}

} // namespace aimeter
