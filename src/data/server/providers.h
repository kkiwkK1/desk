// Server Status data sources.
//
// One SSH round trip per host, using the user's existing OpenSSH client and
// keys (~/.ssh/config aliases work as-is). The remote side is a short POSIX sh
// script fed over stdin; it only reads /proc, df and uptime. No agent, no
// daemon, no extra port.
#pragma once

#include <string>
#include <vector>

namespace server {

constexpr size_t kMaxHosts = 8;
// Below ssh's own ConnectTimeout on purpose: the widget terminates a stuck
// probe itself, so a dead host always reports TIMEOUT with a stable message
// (Windows ssh prints nothing on a pipe when its connect timer fires).
constexpr int kProbeTimeoutMs = 8000;

// One snapshot for one configured host. Produced on a worker thread.
struct HostStatus {
    std::wstring target;        // exactly as configured, e.g. "root@web-01"
    std::wstring name;          // display name, e.g. "WEB-01"
    bool ok = false;
    std::wstring state = L"...";// short value shown in the row: UP, TIMEOUT, AUTH...
    std::wstring error;         // last meaningful ssh line, for Details
    int cpuPercent = -1;
    int memPercent = -1;
    int diskPercent = -1;
    double load1 = -1.0;
    int cores = 0;
    long long uptimeSec = -1;
    int rttMs = 0;              // whole SSH round trip, connect + auth + script
};

// Parsed key=value output of the remote probe script.
struct ProbeOutput {
    int cpuPercent = -1;
    int memPercent = -1;
    int diskPercent = -1;
    double load1 = -1.0;
    int cores = 0;
    long long uptimeSec = -1;

    bool any() const {
        return cpuPercent >= 0 || memPercent >= 0 || diskPercent >= 0 || load1 >= 0.0 ||
               uptimeSec >= 0;
    }
};

// Splits "web-01, db-01; root@10.0.0.5" into unique targets (max kMaxHosts).
// Entries beginning with '-' are rejected so a config value cannot turn into
// an ssh option.
std::vector<std::wstring> ParseHosts(const std::wstring& text);

// Comma-separated form written back by the settings prompt.
std::wstring JoinHosts(const std::vector<std::wstring>& hosts);

// "root@web-01" -> "WEB-01"; used for the row label.
std::wstring DisplayName(const std::wstring& target);

// Parses the LOAD/CORES/MEMT/MEMA/DSK/UP/CPU lines. Returns false when none of
// the expected numbers were found.
bool ParseProbeOutput(const std::string& text, ProbeOutput& out);

// Maps ssh stderr to a short value-cell code: TIMEOUT, AUTH, DNS, REFUSED,
// HOSTKEY, UNREACH, CLOSED, SSH ERR.
std::wstring ClassifyError(const std::string& text);

// Resolves ssh.exe: explicit override (Store key "ssh"), then the Windows
// OpenSSH install, then PATH. Empty when no client exists.
std::wstring SshPath(const std::wstring& overridePath);

// Blocks. Never throws for ordinary failures; those land in state/error.
HostStatus ProbeHost(const std::wstring& target, const std::wstring& sshPath,
                     const std::wstring& identityPath, int timeoutMs);

std::wstring FormatUptime(long long seconds);   // 90 -> 1M, 7200 -> 2H, 1845781 -> 21D
std::wstring FormatLoad(double load);           // 0.42 -> "0.42"

} // namespace server
