#include "targets.h"

#include <cwctype>
#include <unordered_set>

namespace srv {
namespace {

std::wstring Trim(const std::wstring& s) {
    std::size_t begin = 0, end = s.size();
    while (begin < end && std::iswspace(s[begin])) ++begin;
    while (end > begin && std::iswspace(s[end - 1])) --end;
    return s.substr(begin, end - begin);
}

} // namespace

std::vector<std::wstring> TokenizeTargets(const std::wstring& text) {
    std::vector<std::wstring> tokens;
    std::wstring current;
    auto flush = [&] {
        std::wstring token = Trim(current);
        current.clear();
        if (!token.empty()) tokens.push_back(std::move(token));
    };
    for (wchar_t c : text) {
        if (c == L',' || c == L';' || std::iswspace(c)) flush();
        else current.push_back(c);
    }
    flush();
    return tokens;
}

bool ValidTarget(const std::wstring& target, std::wstring* why) {
    auto fail = [&](const wchar_t* reason) {
        if (why) *why = reason;
        return false;
    };
    if (target.empty()) return fail(L"empty target");
    if (target.size() > 255) return fail(L"target is longer than 255 characters");
    if (target.front() == L'-') return fail(L"target may not start with '-'");
    for (wchar_t c : target) {
        if (c < 0x20 || c == 0x7f) return fail(L"target contains a control character");
    }
    return true;
}

std::vector<std::wstring> ParseTargets(const std::wstring& text, std::size_t maxTargets) {
    std::vector<std::wstring> out;
    std::unordered_set<std::wstring> seen;
    for (const auto& token : TokenizeTargets(text)) {
        if (out.size() >= maxTargets) break;
        std::wstring why;
        if (!ValidTarget(token, &why)) continue;

        std::wstring key = token;
        for (wchar_t& c : key) c = (wchar_t)std::towlower(c);
        if (!seen.insert(key).second) continue;
        out.push_back(token);
    }
    return out;
}

std::wstring JoinTargets(const std::vector<std::wstring>& targets) {
    std::wstring out;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (i) out += L", ";
        out += targets[i];
    }
    return out;
}

} // namespace srv
