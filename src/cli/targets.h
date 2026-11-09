#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace srv {

// Splits on comma / semicolon / whitespace, trims, and keeps duplicates.
std::vector<std::wstring> TokenizeTargets(const std::wstring& text);

// An SSH target as accepted on the command line: no leading '-', no control
// characters, at most 255 chars. Set 'why' for a human-readable reason.
bool ValidTarget(const std::wstring& target, std::wstring* why = nullptr);

// Tokenizes, validates, de-duplicates case-insensitively and caps the result.
// Invalid entries are skipped; use ValidTarget first when an error matters.
std::vector<std::wstring> ParseTargets(const std::wstring& text,
                                       std::size_t maxTargets = 32);

std::wstring JoinTargets(const std::vector<std::wstring>& targets);

} // namespace srv
