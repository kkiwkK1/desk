#pragma once

#include <string>
#include <vector>

namespace srv {

// srvctl.ini (UTF-16 key=value, same shape as the widget's ini).
//
// Search order:
//   1. --config PATH
//   2. SRVCTL_CONFIG environment variable
//   3. srvctl.ini next to srvctl.exe
//   4. server.ini next to srvctl.exe (read-only inherited view; edits create
//      srvctl.ini so the widget's own config is never rewritten)
struct Config {
    std::wstring path;        // file edits are written to
    std::wstring sourcePath;  // file values were read from
    bool exists = false;      // sourcePath exists
    bool inherited = false;   // sourcePath is server.ini, not srvctl.ini
    std::vector<std::wstring> hosts;
    std::wstring ssh;
    std::wstring key;
    int timeoutSec = 8;

    bool Load(const std::wstring& explicitPath, bool allowMissing, std::wstring& error);
    bool Save(std::wstring& error);
};

std::wstring ExeDir();
std::wstring DefaultConfigPath();

} // namespace srv
