#pragma once

#include "config.h"
#include "data/server/providers.h"

#include <string>
#include <vector>

namespace srv {

struct SshSpec {
    bool probe = false;      // -T + BatchMode=yes + probe script on stdin
    bool tty = false;        // force -t
    bool noTty = false;      // force -T (exec)
    int connectTimeoutSec = 10;
    std::vector<std::wstring> command;
};

std::wstring ResolveSsh(const Config& config);
std::wstring BuildSshArgs(const Config& config, const std::wstring& target, const SshSpec& spec);

// One parallel probe per target, same engine as the widget. The caller owns
// the timeout; ssh exits cleanly when the outer process is killed.
std::vector<server::HostStatus> ProbeTargets(const Config& config,
                                             const std::wstring& sshPath,
                                             const std::vector<std::wstring>& targets,
                                             int timeoutMs);

// Runs a remote command attached to this console. Returns ssh's exit code,
// 124 on timeout, 127 when ssh cannot start. 'effective' receives the command
// line for --verbose.
int ExecRemote(const Config& config, const std::wstring& target,
               const std::vector<std::wstring>& command, int timeoutSec, bool tty,
               std::wstring* effective, std::wstring& error);

// Opens the login shell (or runs the supplied command words).
int ShellRemote(const Config& config, const std::wstring& target,
                const std::vector<std::wstring>& command, int timeoutSec, bool tty,
                std::wstring* effective, std::wstring& error);

// First line of 'ssh -V' (written to stderr by OpenSSH).
std::wstring SshVersionLine(const std::wstring& sshPath, bool& ok);

} // namespace srv
