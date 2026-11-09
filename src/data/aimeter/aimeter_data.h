#pragma once

#include "accounts.h"
#include "commandcode.h"
#include "history.h"
#include "token_usage.h"
#include "widget/view.h"

#include <string>
#include <vector>

namespace aimeter {

// One immutable snapshot, produced by RefreshData on the worker thread.
struct AimeterData {
    // Legacy provider rows kept for the DeepSeek (optional) and Codex rows.
    // CommandCode accounts are no longer a single row: they live in `accounts`
    // and are rendered per account by account_view.
    std::vector<wg::Row> rows;

    bool showDeepseek = false;      // aimeter.ini show_deepseek (default 0)
    bool balanceOk = false;
    std::wstring balanceText;      // "CNY 6.75", "NO KEY", "NET ERR", ...
    long long balanceCents = 0;
    long long spendCents = 0;      // account-wide, today

    bool tokensOk = false;         // local DSH logs were readable
    TokenUsage tokens;             // today's local DSH tokens

    // M0a: CommandCode multi-account snapshot (N accounts + aggregate).
    AccountBoardData accounts;
    // Seven chart slots, oldest -> newest; value < 0 means "no snapshot".
    // Each value is that day's tightest weekly usage percentage.
    std::vector<HistoryPoint> weekly;
    // Newest successful account fetch, 0 when nothing has ever succeeded.
    long long accountsAsOfMs = 0;

    // Deprecated single-account view; kept so the old render path still
    // compiles while account_view moves to `accounts`.
    bool ccOk = false;
    CommandCodeUsage cc;
};

} // namespace aimeter
