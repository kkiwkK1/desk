#include "account_view.h"

#include "data/aimeter/accounts.h"
#include "data/aimeter/history.h"
#include "data/aimeter/providers.h"
#include "data/aimeter/token_usage.h"

#include "widget/cjk.h"
#include "widget/size.h"
#include "widget/skin.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

namespace account_view {

using namespace wg;

namespace {

// Standard preset is 112..640 outer; card chrome + padding is 48, so the body
// may not exceed 592. The layout below keeps 4 accounts + DeepSeek at 576.
constexpr int kStandardMaxBody = 640 - 48;

constexpr int kAggregateLineH = 16;
constexpr int kAccountBlockH = 52;
constexpr int kTokenBlockH = 72;
constexpr int kChartBlockH = 70;

const wg::Color kInput = wg::Rgba(0x60A5FA);
const wg::Color kWrite = wg::Rgba(0xA78BFA);

long long NowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER v{};
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return (long long)(v.QuadPart / 10000ULL) - 11644473600000LL;
}

std::wstring HHMM(long long epochMs) {
    if (epochMs <= 0) return L"--";
    const long long ticks = (epochMs + 11644473600000LL) * 10000LL;
    FILETIME ft{};
    ft.dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFFLL);
    ft.dwHighDateTime = (DWORD)((unsigned long long)ticks >> 32);
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&ft, &utc)) return L"--";
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return L"--";
    wchar_t buf[16];
    swprintf(buf, 16, L"%02d:%02d", local.wHour, local.wMinute);
    return buf;
}

int TightestPercent(const aimeter::AccountSnapshot& account) {
    double best = 0.0;
    if (account.fiveHour.cap > 0.0) {
        best = std::max(best, account.fiveHour.percent());
    }
    if (account.weekly.cap > 0.0) {
        best = std::max(best, account.weekly.percent());
    }
    return (int)std::lround(best);
}

wg::Color StateColor(int percent, bool exceeded, const wg::Theme& theme) {
    if (exceeded || percent >= 90) return theme.bad;
    if (percent >= 75) return theme.warn;
    return theme.ok;
}

std::wstring Pct(const aimeter::QuotaWindow& window) {
    if (window.cap <= 0.0) return L"--";
    wchar_t buf[16];
    swprintf(buf, 16, L"%d%%", (int)std::lround(window.percent()));
    return buf;
}

std::wstring ResetText(const aimeter::QuotaWindow& window, long long now) {
    std::wstring five = aimeter::ResetCountdown(window.resetAtMs, now);
    return five.empty() ? L"--" : five;
}

const aimeter::HistoryPoint* PointAt(const std::vector<aimeter::HistoryPoint>& points,
                                     size_t index) {
    return index < points.size() ? &points[index] : nullptr;
}

bool IsAccountStale(const aimeter::AccountSnapshot& account, long long now) {
    return !account.ok || aimeter::IsStale(account.asOfMs, now);
}

// Card rows: provider rows (optional DEEPSEEK + CODEX) followed by one row per
// CommandCode account. When no account is configured one placeholder row keeps
// the card from collapsing and points at the config key.
std::vector<wg::Row> BuildRows(const aimeter::AimeterData& data, const wg::Theme& theme,
                               long long now) {
    std::vector<wg::Row> rows = data.rows;

    if (data.accounts.accounts.empty()) {
        rows.push_back(wg::Row{L"COMMANDCODE", L"NO ACCOUNTS", L"SET ACCOUNTS IN AIMETER.INI",
                               -1.0f, theme.warn, L"", 0});
        return rows;
    }

    for (const aimeter::AccountSnapshot& account : data.accounts.accounts) {
        wg::Row row;
        row.label = account.label.empty() ? L"ACC" : account.label;

        const int percent = TightestPercent(account);
        if (account.ok) {
            row.value = L"5H " + Pct(account.fiveHour) + L"  W " + Pct(account.weekly);
            row.frac = (float)std::clamp(percent, 0, 100) / 100.0f;
            row.accent = StateColor(percent, account.fiveHour.exceeded || account.weekly.exceeded,
                                    theme);
            row.noteLeft = L"MONTH " + aimeter::CreditsText(account.monthlyCredits);
            if (IsAccountStale(account, now)) {
                row.note = L"STALE " + aimeter::AgeText(now - account.asOfMs);
                row.noteColor = theme.warn;
            }
        } else {
            row.value = account.error.empty() ? L"NO DATA" : account.error;
            row.valueColor = theme.bad;
            row.frac = -1.0f;
            row.note = account.asOfMs > 0
                           ? L"STALE " + aimeter::AgeText(now - account.asOfMs)
                           : L"CHECK CREDENTIALS";
            row.noteColor = theme.warn;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

// Same visual grammar as skin::Rows, but labels go through cjk::Text so an
// account renamed to Chinese still renders instead of turning into spaces.
void DrawRowList(Canvas& canvas, const Rect& body, const std::vector<Row>& rows,
                 const Theme& theme) {
    const int left = body.x + skin::kCardPadX;
    const int right = body.right() - skin::kCardPadX;
    int y = body.y;

    for (const Row& row : rows) {
        cjk::Text(canvas, row.label, (float)left, (float)y, theme.label, skin::kValueScale);
        canvas.TextRight(row.value, right, (float)y,
                         row.valueColor ? row.valueColor : theme.text, skin::kValueScale);

        if (!row.note.empty() || !row.noteLeft.empty()) {
            if (!row.noteLeft.empty()) {
                canvas.Text(row.noteLeft, (float)left, (float)(y + 16), theme.faint,
                            skin::kMetaScale);
            }
            if (!row.note.empty()) {
                canvas.TextRight(row.note, right, (float)(y + 16),
                                 row.noteColor ? row.noteColor : theme.faint,
                                 skin::kMetaScale);
            }
        }
        // Every row with a real 0..1 fraction gets the same progress bar, even
        // when it also carries a note line (accounts show MONTH/STALE there).
        // Previously Codex had a bar and CommandCode rows did not.
        if (row.frac >= 0.0f) {
            canvas.Bar(left, y + 24, right - left, 4, row.frac,
                       row.accent ? row.accent : theme.ok, theme.track);
        }
        y += skin::kRowPitch;
    }
}

int RowsCount(const aimeter::AimeterData& data) {
    return (int)(data.rows.size() + std::max<size_t>(1, data.accounts.accounts.size()));
}

int DetailsHeight(const aimeter::AimeterData& data) {
    // 6 gap + aggregate line + N blocks + 6 + tokens + 6 + chart.
    return skin::kDetailsGap + kAggregateLineH +
           (int)data.accounts.accounts.size() * kAccountBlockH + skin::kDetailsGap +
           kTokenBlockH + skin::kDetailsGap + kChartBlockH;
}

std::wstring AggregateLine(const aimeter::AimeterData& data) {
    const size_t count = data.accounts.accounts.size();
    wchar_t buf[96];
    int errors = 0;
    for (const auto& account : data.accounts.accounts) {
        if (!account.ok) ++errors;
    }
    swprintf(buf, 96, L"%u ACC  TIGHT %d%%  MONTH %ls%s", (unsigned)count,
             (int)std::lround(data.accounts.tightestPct),
             aimeter::CreditsText(data.accounts.monthlyCreditsTotal).c_str(),
             errors > 0 ? L"  ERR" : L"");
    return buf;
}

void DrawTokens(Canvas& canvas, const aimeter::AimeterData& data, const wg::Theme& theme,
                const Rect& body, int y) {
    const int left = body.x + skin::kCardPadX;
    const int right = body.right() - skin::kCardPadX;

    canvas.Text(L"TOKENS TODAY", (float)left, (float)(y + 9), theme.faint, skin::kMetaScale);
    canvas.TextRight(L"LOCAL", right, (float)(y + 9), theme.label, skin::kMetaScale);

    const unsigned long long total = data.tokens.total();
    if (data.tokensOk) {
        const std::wstring tokenText = aimeter::FormatTokens(total);
        canvas.Text(tokenText, (float)left, (float)(y + 19), theme.text, skin::kBigScale);
        canvas.Text(L"TOK", (float)(left + canvas.TextWidth(tokenText, skin::kBigScale) + 6),
                    (float)(y + 30), theme.faint, skin::kMetaScale);

        const unsigned long long prompt =
            data.tokens.uncachedInput + data.tokens.cacheRead + data.tokens.cacheWrite;
        if (prompt > 0) {
            wchar_t hit[24];
            swprintf(hit, 24, L"CACHE HIT %.1f%%",
                     100.0 * (double)data.tokens.cacheRead / (double)prompt);
            canvas.TextRight(hit, right, (float)(y + 30), theme.label, skin::kMetaScale);
        }
    } else {
        canvas.Text(L"N/A", (float)left, (float)(y + 19), theme.text, skin::kBigScale);
        canvas.TextRight(L"NO DSH SESSION LOG", right, (float)(y + 30), theme.faint,
                         skin::kMetaScale);
    }

    canvas.Text(data.tokensOk ? L"DSH LOG - THIS MACHINE" : L"TOKENS - NO LOCAL SOURCE",
                (float)left, (float)(y + 50), theme.faint, skin::kMetaScale);
    if (data.tokensOk) {
        wchar_t in[64];
        swprintf(in, 64, L"IN %ls  OUT %ls",
                 aimeter::FormatTokens(data.tokens.uncachedInput).c_str(),
                 aimeter::FormatTokens(data.tokens.output).c_str());
        canvas.TextRight(in, right, (float)(y + 50), theme.faint, skin::kMetaScale);
    }
}

void DrawChart(Canvas& canvas, const aimeter::AimeterData& data, const wg::Theme& theme,
               const Rect& body, int y) {
    const int left = body.x + skin::kCardPadX;
    const int right = body.right() - skin::kCardPadX;

    canvas.Text(L"7D QUOTA MAX", (float)left, (float)(y + 9), theme.faint, skin::kMetaScale);
    canvas.TextRight(L"LOCAL", right, (float)(y + 9), theme.label, skin::kMetaScale);

    std::vector<float> values;
    values.reserve(aimeter::kHistoryChartDays);
    for (int i = 0; i < aimeter::kHistoryChartDays; ++i) {
        const aimeter::HistoryPoint* point = PointAt(data.weekly, (size_t)i);
        values.push_back(point ? point->value : -1.0f);
    }

    float peak = 0.0f;
    for (float v : values) peak = std::max(peak, v);
    const wg::Color fill = peak >= 90.0f ? theme.bad : (peak >= 75.0f ? theme.warn : theme.ok);

    skin::MiniBars(canvas, Rect{left, y + 20, right - left, 36}, values, fill, theme.track);
    canvas.Text(L"7D", (float)left, (float)(y + 58), theme.faint, skin::kMetaScale);
    canvas.TextRight(L"TODAY", right, (float)(y + 58), theme.faint, skin::kMetaScale);
}

} // namespace

int BodyHeight(const aimeter::AimeterData& data, bool expanded) {
    const int rows = RowsCount(data) * skin::kRowPitch;
    if (!expanded) return rows;
    const int body = rows + DetailsHeight(data);
    // Max 4 accounts + DeepSeek stays within 592; clamp is a safety net so a
    // future row cannot silently overflow the preset.
    return std::min(body, kStandardMaxBody);
}

void DrawBody(const aimeter::AimeterData& data, wg::Canvas& canvas, const wg::Rect& body,
              const wg::Theme& theme, bool expanded) {
    const long long now = NowMs();
    const std::vector<wg::Row> rows = BuildRows(data, theme, now);
    DrawRowList(canvas, body, rows, theme);
    if (!expanded) return;

    const int left = body.x + skin::kCardPadX;
    const int right = body.right() - skin::kCardPadX;
    int y = body.y + (int)rows.size() * skin::kRowPitch;

    skin::Divider(canvas, left, right, y, theme);
    y += skin::kDetailsGap;

    // Aggregate line: one glance for "how are the two accounts doing".
    canvas.Text(AggregateLine(data), (float)left, (float)(y + 5), theme.label,
                skin::kMetaScale);
    y += kAggregateLineH;

    for (const aimeter::AccountSnapshot& account : data.accounts.accounts) {
        skin::Divider(canvas, left, right, y, theme);

        const bool stale = IsAccountStale(account, now);
        const int percent = TightestPercent(account);
        const wg::Color state =
            StateColor(percent, account.fiveHour.exceeded || account.weekly.exceeded, theme);

        wchar_t reset[96];
        swprintf(reset, 96, L"RESET  5H %ls   W %ls",
                 ResetText(account.fiveHour, now).c_str(),
                 ResetText(account.weekly, now).c_str());
        canvas.Text(reset, (float)left, (float)(y + 9), theme.faint, skin::kMetaScale);

        const std::wstring month =
            L"MONTH " + aimeter::CreditsText(account.monthlyCredits) +
            (account.monthlyLimit > 0.0
                 ? L" / " + aimeter::CreditsText(account.monthlyLimit) + L" " +
                       std::to_wstring((int)std::lround(
                           100.0 * account.monthlyCredits / account.monthlyLimit)) + L"%"
                 : L"");
        canvas.Text(month, (float)left, (float)(y + 24), theme.text, skin::kMetaScale);

        if (account.sandboxLimitMinutes > 0) {
            wchar_t sandbox[64];
            swprintf(sandbox, 64, L"SANDBOX %lld/%lld MIN",
                     (long long)account.sandboxUsedMinutes,
                     (long long)account.sandboxLimitMinutes);
            canvas.TextRight(sandbox, right, (float)(y + 24), theme.faint, skin::kMetaScale);
        }

        std::wstring source = L"SOURCE ";
        source += aimeter::SourceTag(account.source);
        canvas.Text(source, (float)left, (float)(y + 39), theme.faint, skin::kMetaScale);

        std::wstring asOf = L"AS OF " + HHMM(account.asOfMs);
        if (stale) asOf = L"STALE " + aimeter::AgeText(now - account.asOfMs);
        canvas.TextRight(asOf, right, (float)(y + 39), stale ? theme.warn : theme.faint,
                         skin::kMetaScale);
        (void)state;

        y += kAccountBlockH;
    }

    skin::Divider(canvas, left, right, y, theme);
    y += skin::kDetailsGap;
    DrawTokens(canvas, data, theme, body, y);
    y += kTokenBlockH;

    skin::Divider(canvas, left, right, y, theme);
    y += skin::kDetailsGap;
    DrawChart(canvas, data, theme, body, y);
}

wg::StatusChip Status(const aimeter::AimeterData& data, const wg::Theme& theme) {
    if (data.accounts.accounts.empty()) {
        // Before the first refresh the placeholder rows still show "..." .
        bool loading = true;
        for (const wg::Row& row : data.rows) {
            if (row.value != L"...") loading = false;
        }
        return wg::StatusChip{loading ? L"LOADING" : L"NO DATA",
                              loading ? theme.faint : theme.warn};
    }

    const long long now = NowMs();
    int ok = 0, stale = 0;
    for (const aimeter::AccountSnapshot& account : data.accounts.accounts) {
        if (account.ok) ++ok;
        if (IsAccountStale(account, now)) ++stale;
    }
    if (ok == 0) return wg::StatusChip{L"ERROR", theme.bad};
    if (stale > 0 || !aimeter::AllAccountsOk(data.accounts)) {
        return wg::StatusChip{L"WARN", theme.warn};
    }
    return wg::StatusChip{L"OK", theme.ok};
}

} // namespace account_view
