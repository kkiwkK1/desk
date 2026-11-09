// srvctl - command-line companion for the Server Status widget.
//
// The widget answers "how are my machines?" at a glance. srvctl answers the
// same question for scripts, CI and interactive shells: probe, exec, ssh,
// manage the host list, and diagnose the local OpenSSH setup.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace srv {

inline constexpr const wchar_t* kVersion = L"1.0.0";
inline constexpr std::size_t kMaxTargets = 32;
inline constexpr std::size_t kWidgetHosts = 8;

enum class ColorMode { Auto, Always, Never };

enum ExitCode {
    kExitOk = 0,
    kExitFailure = 1,
    kExitUsage = 2,
    kExitPartial = 3,
    kExitConfig = 4,
    kExitTimeout = 124,
};

struct Options {
    std::wstring command;                  // canonical: status, exec, ssh, ...
    std::wstring hostsAction;              // hosts subcommand, empty otherwise
    std::vector<std::wstring> targets;     // status hosts / exec host / hosts args
    std::vector<std::wstring> remoteArgs;  // exec + ssh command words
    std::wstring configPath;
    std::wstring sshPath;
    std::wstring keyPath;
    int timeoutSec = 0;                    // 0 = unset
    int watchSec = 0;                      // 0 = no --watch
    bool watch = false;
    bool json = false;
    bool verbose = false;
    bool tty = false;
    bool help = false;
    bool version = false;
    ColorMode color = ColorMode::Auto;
};

struct ParseResult {
    bool ok = true;
    std::wstring error;
    std::wstring hint;
    Options options;
};

// Parses argv (including argv[0]). Never touches the console or the network.
ParseResult ParseArgs(const std::vector<std::wstring>& argv);

// Maps aliases (st, run, shell, host, check) to canonical command names.
// Returns an empty string when the name is unknown.
std::wstring CanonicalCommand(const std::wstring& name);

} // namespace srv
