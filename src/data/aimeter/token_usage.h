// DeepSeek token usage, read from the local DSH session projection cache.
//
// The DeepSeek balance API only reports money; it has no token counters. DSH,
// however, records every call's token buckets per session under
// %USERPROFILE%\.dsh\storages\session_projcache\sessions\*.json.
//
// Scope is deliberately local: this is DSH traffic on this machine, not the
// whole account. The UI must label it as LOCAL and never mix it with the
// account-wide balance/spend.
#pragma once

#include "widget/store.h"

#include <string>
#include <vector>

namespace aimeter {

struct TokenUsage {
    unsigned long long uncachedInput = 0;
    unsigned long long cacheRead = 0;
    unsigned long long cacheWrite = 0;
    unsigned long long output = 0;

    unsigned long long total() const {
        return uncachedInput + cacheRead + cacheWrite + output;
    }
};

struct DshSessionUsage {
    std::wstring id;
    long long createdAtMs = 0;   // Unix epoch milliseconds (DSH identity.createdAt)
    TokenUsage totals;           // cumulative for the session
};

// Returns false when there is no DSH data directory at all.
bool ReadDshSessions(std::vector<DshSessionUsage>& out);

// Computes today's local token total by snapshot-diffing the per-session totals.
// This mirrors the balance snapshot logic: usage while the widget is off is
// only counted when we can attribute it (sessions created today), so a session
// that spans midnight may miss its pre-start portion.
TokenUsage UpdateTodayTokens(const std::vector<DshSessionUsage>& sessions, wg::Store& store,
                             const std::wstring& today);

std::wstring LocalDay();                       // "YYYY-MM-DD", local time
std::wstring FormatTokens(unsigned long long count);  // 824, 12.4K, 1.24M, 3.10B

} // namespace aimeter
