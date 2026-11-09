// CommandCode quota source.
//
// CommandCode exposes undocumented /alpha endpoints on api.commandcode.ai.
// The widget only needs billing/credits: the 5-hour / weekly windows plus the
// monthly credit balance. The key is read from AiMeter/CommandCode, falling
// back to the DSH credential store where CommandCode is configured.
#pragma once

#include <string>

namespace aimeter {

struct CcWindow {
    double used = 0.0;
    double cap = 0.0;
    bool exceeded = false;
    bool present = false;

    double percent() const { return cap > 0.0 ? used * 100.0 / cap : 0.0; }
};

struct CommandCodeUsage {
    bool ok = false;
    std::wstring text;          // error text when !ok
    CcWindow fiveHour;
    CcWindow weekly;
    double monthlyCredits = 0.0;
};

// AiMeter/CommandCode first, then ~/.dsh/.credentials.yaml COMMANDCODE_API_KEY.
std::wstring LoadCommandCodeKey();

// Parses the /alpha/billing/credits JSON body. Exposed for the headless test.
CommandCodeUsage ParseCommandCodeBody(const std::string& body);

CommandCodeUsage FetchCommandCode(const std::wstring& key);

} // namespace aimeter
