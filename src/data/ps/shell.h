// Where the terminal's shell comes from.
//
// The card carries no shell of its own: it finds pwsh (or falls back to 5.1)
// and reports the version it found in the title bar.
#pragma once

#include <string>

namespace ps {

// Explicit override, then %PWSH_EXE%, then pwsh on PATH, then the usual install
// locations, then Windows PowerShell 5.1. Empty when nothing exists.
std::wstring ResolveShell(const std::wstring& overridePath);

// "PWSH 7.6" / "PS 5.1" from the executable's version resource.
std::wstring ShellLabel(const std::wstring& path);

} // namespace ps
