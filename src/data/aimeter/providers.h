// AiMeter data sources. Kept out of the widget so they can be reused or
// unit-tested without a window.
#pragma once

#include <string>

namespace aimeter {

struct Balance {
    bool ok = false;
    long long cents = 0;
    std::wstring text;   // "CNY 6.75", "NO KEY", "NET ERR", "BAD KEY", ...
};

Balance FetchDeepSeek(const std::wstring& key);

// Newest ~/.codex/sessions/**/rollout-*.jsonl -> last rate_limits block.
// Offline: Codex writes the numbers, we only read them.
bool ReadCodex(float& primaryPercent, float& secondaryPercent);

long long ParseCents(const std::string& amount);
std::wstring Money(long long cents);

} // namespace aimeter
