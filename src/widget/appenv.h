// Per-user application plumbing shared by the standalone hosts and the board.
#pragma once

#include <string>

namespace wg {
namespace appenv {

// Directory of the running exe (no trailing slash).
std::wstring ExeDir();

// True when DESK_TEST_INSTANCE holds a valid suffix (letters/digits/-/_). The
// suffix is copied to *suffix when non-null.
bool TestInstanceSuffix(std::wstring* suffix);

// True when a valid test instance also set a non-empty DESK_TEST_CONFIG_DIR.
// ConfigDir() still validates that path; this only reports the request.
bool TestConfigDirOverride();

// Directory that holds application and module configuration. Uses the exe directory when it
// is writable, otherwise %APPDATA%\<appTitle> (created on demand).
//
// Test isolation: with a valid DESK_TEST_INSTANCE the directory never falls back to the real
// %APPDATA%. DESK_TEST_CONFIG_DIR, when set, must be an existing absolute writable directory;
// a missing/relative/unwritable path is fail-closed. On failure the result is empty and
// *error (when non-null) explains why, so a host can refuse to start or show a diagnostic.
std::wstring ConfigDir(const std::wstring& appTitle, std::wstring* error = nullptr);
std::wstring ConfigPath(const std::wstring& fileName, const std::wstring& appTitle,
                        std::wstring* error = nullptr);

// HKCU\...\Run value helpers.
bool AutostartEnabled(const std::wstring& name);
std::wstring AutostartCommand(const std::wstring& name);
void SetAutostart(const std::wstring& name, bool on);
void RemoveAutostart(const std::wstring& name);

} // namespace appenv
} // namespace wg
