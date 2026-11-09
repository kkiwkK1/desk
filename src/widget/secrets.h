// Windows Credential Manager storage. Secrets never touch disk in plain text.
#pragma once

#include <string>

namespace wg {

bool SaveSecret(const std::wstring& target, const std::wstring& user,
                const std::wstring& secret);
std::wstring LoadSecret(const std::wstring& target);
bool DeleteSecret(const std::wstring& target);

} // namespace wg
