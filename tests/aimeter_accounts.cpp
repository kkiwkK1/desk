// Headless tests for the M0a Account data layer: multi-account config import,
// billing payload parsing, aggregation/carry-forward, staleness and the daily
// history file. No window, no network: fixtures only.
#include "data/aimeter/accounts.h"
#include "data/aimeter/history.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[aimeter-test] FAIL %s\n", message);
        ++failures;
    }
}

constexpr long long kNow = 1794000000000LL;  // fixed clock for all assertions

// Same shape as GET /alpha/billing/credits (fields the card consumes).
const char* const kBody1 =
    "{\"credits\":{\"belowThreshold\":false,\"creditThreshold\":0,"
    "\"monthlyCredits\":0.7826343205,\"purchasedCredits\":0,\"freeCredits\":0},"
    "\"windowLimits\":{\"limited\":true,\"exceeded\":null,"
    "\"fiveHour\":{\"used\":5.4077395155,\"cap\":14,\"exceeded\":false,"
    "\"resetAt\":1793998087850},"
    "\"weekly\":{\"used\":25.4533012059,\"cap\":35,\"exceeded\":false,"
    "\"resetAt\":1794470977120}},"
    "\"sandboxAccess\":true,\"sandboxMinutes\":{\"usedMinutes\":0,"
    "\"limitMinutes\":0,\"overageUsd\":0,\"resetAt\":1796083200000}}";

// Second account: no 5h window yet (resetAt=0) and the low-credit flag set.
const char* const kBody2 =
    "{\"credits\":{\"belowThreshold\":true,\"creditThreshold\":1.0,"
    "\"monthlyCredits\":59.6714505184,\"purchasedCredits\":0,\"freeCredits\":0},"
    "\"windowLimits\":{\"limited\":true,\"exceeded\":null,"
    "\"fiveHour\":{\"used\":0,\"cap\":14,\"exceeded\":false,\"resetAt\":0},"
    "\"weekly\":{\"used\":10.3285494816,\"cap\":35,\"exceeded\":false,"
    "\"resetAt\":1794422429275}},"
    "\"sandboxAccess\":true,\"sandboxMinutes\":{\"usedMinutes\":3,"
    "\"limitMinutes\":60,\"overageUsd\":0,\"resetAt\":1796083200000}}";

const char* const kYaml =
    "version: 1\n"
    "records:\n"
    "  client-connection/browser-session:\n"
    "    kind: generic\n"
    "refs:\n"
    "  DEEPSEEK_API_KEY: sk-deepseek-0000000000000000000000000000\n"
    "  COMMANDCODE_API_KEY: us-account-one-000000000000000000000000000000\n"
    "  COMMANDCODE_API_KEY_2: us-account-two-0000000000000000000000000000\n";

std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0) return L".";
    std::wstring text(path, n);
    const size_t slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : text.substr(0, slash);
}

bool Close(double a, double b, double epsilon = 1e-6) {
    return std::fabs(a - b) <= epsilon;
}

} // namespace

int wmain() {
    using namespace aimeter;

    // --- config import from the DSH credential file -----------------------
    const std::vector<AccountConfig> imported = ImportAccounts(kYaml);
    Check(imported.size() == 2, "two COMMANDCODE refs import as two accounts");
    if (imported.size() == 2) {
        Check(imported[0].keyRef == L"COMMANDCODE_API_KEY", "first ref keeps its rank");
        Check(imported[1].keyRef == L"COMMANDCODE_API_KEY_2", "second ref keeps its rank");
        Check(imported[0].label == L"ACC1", "default label is ASCII ACC1");
        Check(imported[1].label == L"ACC2", "default label is ASCII ACC2");
        Check(imported[0].source == Source::Dsh, "imported accounts use DSH source");
    }
    const std::vector<std::wstring> refs = DshCcRefNames(kYaml);
    Check(refs.size() == 2, "DshCcRefNames finds both CommandCode refs");
    Check(ImportAccounts("refs:\n  DEEPSEEK_API_KEY: sk-only-0000000000\n").empty(),
          "a DeepSeek-only file imports no CommandCode account");

    // --- billing payload parsing ------------------------------------------
    const AccountSnapshot first = ParseAccountBody(kBody1, kNow);
    Check(first.ok, "fixture 1 parses");
    Check(Close(first.fiveHour.used, 5.4077395155), "five-hour used");
    Check(Close(first.fiveHour.cap, 14.0), "five-hour cap");
    Check(first.fiveHour.resetAtMs == 1793998087850LL, "five-hour resetAt");
    Check(Close(first.weekly.used, 25.4533012059), "weekly used");
    Check(Close(first.weekly.cap, 35.0), "weekly cap");
    Check(first.weekly.resetAtMs == 1794470977120LL, "weekly resetAt");
    Check(Close(first.monthlyCredits, 0.7826343205), "monthly credits");
    Check(!first.belowThreshold, "belowThreshold false");
    Check(first.sandboxResetMs == 1796083200000LL, "sandbox resetAt");
    Check(first.asOfMs == kNow, "successful parse stamps asOf");
    Check(PercentText(first.fiveHour) == L"38%", "five-hour percent text");
    Check(PercentText(first.weekly) == L"72%", "weekly percent text (floor)");
    Check(!ResetCountdown(first.weekly.resetAtMs, kNow).empty(), "reset countdown non-empty");
    Check(HasReset(first.fiveHour), "five-hour window has a reset");

    const AccountSnapshot second = ParseAccountBody(kBody2, kNow);
    Check(second.ok, "fixture 2 parses");
    Check(second.belowThreshold, "belowThreshold true is honoured");
    Check(!HasReset(second.fiveHour), "resetAt=0 means no window");
    Check(ResetCountdown(0, kNow).empty(), "no-window countdown is empty, not 0M");
    Check(second.sandboxLimitMinutes == 60, "sandbox limit parses");

    const AccountSnapshot broken = ParseAccountBody("{\"credits\":{}}", kNow);
    Check(!broken.ok, "payload without window limits is not ok");
    Check(broken.error == kErrNoData, "missing fields report NO DATA");

    // --- aggregation and carry-forward ------------------------------------
    AccountSnapshot failed = first;
    failed.ok = false;
    failed.error = kErrNet;
    failed.asOfMs = 0;
    failed.monthlyCredits = 99.0;  // must be skipped: never fetched
    const AccountBoardData board = Aggregate({first, failed});
    Check(board.accounts.size() == 2, "aggregate keeps every account");
    Check(board.anyOk, "aggregate reports at least one good account");
    Check(Close(board.tightestPct, 25.4533012059 * 100.0 / 35.0, 1e-4),
          "tightest percent comes from the weekly window");
    Check(Close(board.monthlyCreditsTotal, 0.7826343205), "failed account is skipped in total");

    const AccountSnapshot carried = CarryForward(failed, first);
    Check(!carried.ok, "carried-forward snapshot stays not ok");
    Check(carried.error == kErrNet, "carried-forward keeps the fresh error");
    Check(Close(carried.weekly.used, 25.4533012059), "carried-forward keeps last good numbers");
    Check(carried.asOfMs == kNow, "carried-forward keeps the old timestamp");
    Check(CarryForward(failed, failed).asOfMs == 0, "nothing to carry when never fetched");

    // --- staleness ---------------------------------------------------------
    Check(IsStale(0, kNow), "never fetched is stale");
    Check(!IsStale(kNow - 1000, kNow), "a fresh sample is not stale");
    Check(!IsStale(kNow - 600000, kNow), "the boundary is still fresh");
    Check(IsStale(kNow - 600001, kNow), "older than the window is stale");
    Check(!IsStale(kNow + 1000, kNow), "future sample (clock skew) is not stale");

    // --- daily history -----------------------------------------------------
    const std::wstring historyPath = ExeDir() + L"\\aimeter-test-history.ini";
    DeleteFileW(historyPath.c_str());
    wg::Store history;
    Check(HistoryOpen(history, historyPath), "history store opens on a missing file");

    const std::wstring today = L"2026-11-07";
    Check(DateAddDays(today, -1) == L"2026-11-06", "date math crosses days");
    Check(DateDiffDays(L"2026-11-05", today) == 2, "date diff");

    for (int delta = -2; delta <= 0; ++delta) {
        HistoryDay day;
        day.date = DateAddDays(today, delta);
        day.accounts.resize(1);
        day.accounts[0].week = HistoryWindow{delta == 0 ? 30.0 : 20.0, 40.0};
        day.accounts[0].five = HistoryWindow{5.0, 14.0};
        day.accounts[0].monthly = 1.5;
        day.tokensOk = true;
        day.tokens.output = 100;
        Check(SaveDay(history, day, today), "SaveDay writes a day");
    }

    const std::vector<HistoryDay> recent = ReadRecent(history, today, 3);
    Check(recent.size() == 3, "three saved days read back");
    if (recent.size() == 3) {
        Check(recent.front().date == DateAddDays(today, -2), "first read day is oldest");
        Check(recent.back().date == today, "last read day is today");
        Check(Close(recent.back().accounts[0].week.used, 30.0), "today's weekly used reads back");
    }

    const std::vector<HistoryPoint> series = WeeklyUsageSeries(history, today, 7);
    Check(series.size() == 7, "series always has seven slots");
    if (series.size() == 7) {
        Check(series[0].value < 0.0f && series[3].value < 0.0f, "missing days stay blank");
        Check(Close(series[4].value, 50.0, 1e-3), "two days ago reads 50%");
        Check(Close(series[5].value, 50.0, 1e-3), "yesterday reads 50%");
        Check(Close(series[6].value, 75.0, 1e-3), "today reads 75%");
    }

    // Trim drops dates outside the retention window.
    HistoryDay old;
    old.date = L"2026-08-01";
    old.accounts.resize(1);
    WriteDay(history, old);
    Check(Trim(history, today, kHistoryKeepDays) > 0, "Trim removes expired days");
    const std::wstring oldPrefix = DayPrefix(L"2026-08-01");
    bool oldStillThere = false;
    for (const auto& entry : history.Snapshot()) {
        if (entry.first.compare(0, oldPrefix.size(), oldPrefix) == 0) oldStillThere = true;
    }
    Check(!oldStillThere, "expired day keys are gone");

    DeleteFileW(historyPath.c_str());
    std::printf("[aimeter-test] %s\n", failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
