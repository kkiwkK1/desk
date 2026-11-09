// Where the API key comes from, in priority order:
//   1. AIMINI_API_KEY (matches AIMINI_ENDPOINT, for other providers)
//   2. DEEPSEEK_API_KEY in the environment
//   3. Windows Credential Manager target "AiMini/DeepSeek" (set from the widget)
//   4. ~/.dsh/.credentials.yaml refs.DEEPSEEK_API_KEY (the DSH key this machine
//      already uses), read-only
// Nothing here ever writes the key to a plain file.
#pragma once

#include <string>

namespace aimini {

std::wstring LoadApiKey();
bool SaveApiKey(const std::wstring& key);

} // namespace aimini
