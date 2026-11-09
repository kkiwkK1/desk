#include "commandcode.h"

#include "widget/http.h"
#include "widget/secrets.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace aimeter {
namespace {

std::string Trim(std::string s) {
    const char* ws = " \t\r\n";
    const size_t first = s.find_first_not_of(ws);
    if (first == std::string::npos) return {};
    const size_t last = s.find_last_not_of(ws);
    return s.substr(first, last - first + 1);
}

// The DSH credential file is a tiny YAML document:
//   refs:
//     COMMANDCODE_API_KEY: <secret>
std::wstring ReadDshCredential(const char* name) {
    wchar_t profile[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) return {};

    const fs::path path = fs::path(profile) / L".dsh" / L".credentials.yaml";
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};

    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    const std::string prefix = std::string(name) + ":";

    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;

        line = Trim(line);
        if (line.compare(0, prefix.size(), prefix) != 0) continue;

        std::string value = Trim(line.substr(prefix.size()));
        if (value.empty() || value[0] == '|' || value[0] == '>') return {};
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        if (value.size() < 8) return {};
        return std::wstring(value.begin(), value.end());
    }
    return {};
}

bool FindKey(const std::string& body, const char* key, size_t from, size_t& at) {
    const std::string pattern = std::string("\"") + key + "\"";
    size_t p = body.find(pattern, from);
    while (p != std::string::npos) {
        // Skip string values: "exceeded":"weekly" must not be mistaken for
        // the "weekly" object key.
        size_t q = p + pattern.size();
        while (q < body.size() &&
               (body[q] == ' ' || body[q] == '\t' || body[q] == '\r' || body[q] == '\n')) {
            ++q;
        }
        if (q < body.size() && body[q] == ':') {
            at = p + pattern.size();
            return true;
        }
        p = body.find(pattern, p + 1);
    }
    return false;
}

bool NumberAt(const std::string& body, size_t after_key, size_t limit, double& out) {
    size_t p = body.find(':', after_key);
    if (p == std::string::npos || p >= limit) return false;
    ++p;
    while (p < limit && (body[p] == ' ' || body[p] == '\t' || body[p] == '\r' ||
                         body[p] == '\n')) {
        ++p;
    }
    if (p >= limit) return false;

    char* end = nullptr;
    const double value = std::strtod(body.c_str() + p, &end);
    if (end == body.c_str() + p) return false;
    out = value;
    return true;
}

bool KeyNumber(const std::string& body, const char* key, size_t from, size_t limit,
               double& out) {
    size_t at = 0;
    if (!FindKey(body, key, from, at) || at >= limit) return false;
    return NumberAt(body, at, limit, out);
}

bool ParseWindow(const std::string& body, const char* name, size_t from, CcWindow& window) {
    size_t at = 0;
    if (!FindKey(body, name, from, at)) return false;
    const size_t brace = body.find('{', at);
    if (brace == std::string::npos) return false;
    size_t end = body.find('}', brace);
    if (end == std::string::npos) end = body.size();

    double used = 0.0, cap = 0.0;
    if (!KeyNumber(body, "used", brace, end, used)) return false;
    if (!KeyNumber(body, "cap", brace, end, cap)) return false;

    window.used = used;
    window.cap = cap;
    window.present = true;

    size_t ex = 0;
    if (FindKey(body, "exceeded", brace, ex) && ex < end) {
        const size_t colon = body.find(':', ex);
        if (colon != std::string::npos && colon < end) {
            size_t p = colon + 1;
            while (p < end && (body[p] == ' ' || body[p] == '\t')) ++p;
            window.exceeded = body.compare(p, 4, "true") == 0;
        }
    }
    return true;
}

} // namespace

std::wstring LoadCommandCodeKey() {
    std::wstring key = wg::LoadSecret(L"AiMeter/CommandCode");
    if (!key.empty()) return key;
    return ReadDshCredential("COMMANDCODE_API_KEY");
}

CommandCodeUsage ParseCommandCodeBody(const std::string& body) {
    CommandCodeUsage result;

    size_t limits = body.find("\"windowLimits\"");
    if (limits == std::string::npos) limits = 0;

    const bool hasFive = ParseWindow(body, "fiveHour", limits, result.fiveHour) ||
                         ParseWindow(body, "five_hour", limits, result.fiveHour);
    const bool hasWeekly = ParseWindow(body, "weekly", limits, result.weekly);
    if (!hasFive && !hasWeekly) {
        result.text = L"NO DATA";
        return result;
    }

    if (!KeyNumber(body, "monthlyCredits", 0, body.size(), result.monthlyCredits)) {
        KeyNumber(body, "monthly_credits", 0, body.size(), result.monthlyCredits);
    }
    result.ok = true;
    return result;
}

CommandCodeUsage FetchCommandCode(const std::wstring& key) {
    CommandCodeUsage result;
    if (key.empty()) {
        result.text = L"NO KEY";
        return result;
    }

    const wg::HttpResponse response =
        wg::HttpGet(L"api.commandcode.ai", L"/alpha/billing/credits", key, L"AiMeter/1.0");
    if (!response.transport) {
        result.text = L"NET ERR";
        return result;
    }
    if (response.status == 401 || response.status == 403) {
        result.text = L"BAD KEY";
        return result;
    }
    if (response.status != 200) {
        result.text = L"ERR " + std::to_wstring(response.status);
        return result;
    }
    return ParseCommandCodeBody(response.body);
}

} // namespace aimeter
