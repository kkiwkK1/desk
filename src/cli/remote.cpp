#include "remote.h"

#include "cli.h"
#include "output.h"
#include "widget/proc.h"

#include <windows.h>

#include <algorithm>
#include <thread>

namespace srv {
namespace {

std::wstring Trim(const std::wstring& s) {
    std::size_t begin = 0, end = s.size();
    while (begin < end && iswspace(s[begin])) ++begin;
    while (end > begin && iswspace(s[end - 1])) --end;
    return s.substr(begin, end - begin);
}

std::wstring FirstLine(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(begin, end - begin);
        while (!line.empty() && (unsigned char)line.back() <= ' ') line.pop_back();
        while (!line.empty() && (unsigned char)line.front() <= ' ') line.erase(line.begin());
        if (!line.empty()) return Widen(line);
        begin = end + 1;
    }
    return {};
}

// ssh emits UTF-8; a legacy console defaults to the OEM code page. Switch the
// console to UTF-8 only for the duration of the child and always restore it.
class Utf8ConsoleScope {
public:
    Utf8ConsoleScope() {
        const UINT output = GetConsoleOutputCP();
        const UINT input = GetConsoleCP();
        if (output == 0) return;
        if (output != CP_UTF8 || input != CP_UTF8) {
            savedOut_ = output;
            savedIn_ = input;
            SetConsoleOutputCP(CP_UTF8);
            SetConsoleCP(CP_UTF8);
            active_ = true;
        }
    }
    ~Utf8ConsoleScope() {
        if (!active_) return;
        if (savedOut_) SetConsoleOutputCP(savedOut_);
        if (savedIn_) SetConsoleCP(savedIn_);
    }

private:
    UINT savedOut_ = 0;
    UINT savedIn_ = 0;
    bool active_ = false;
};

int RunRemote(const Config& config, const std::wstring& target,
              const std::vector<std::wstring>& command, int timeoutSec, bool tty,
              bool noTty, bool probe, std::wstring* effective, std::wstring& error) {
    const std::wstring ssh = ResolveSsh(config);
    if (ssh.empty()) {
        error = L"OpenSSH client not found; install it or set ssh= in srvctl.ini";
        return 127;
    }

    SshSpec spec;
    spec.probe = probe;
    spec.tty = tty;
    spec.noTty = noTty;
    spec.command = command;
    const std::wstring args = BuildSshArgs(config, target, spec);
    if (effective) *effective = wg::QuoteArg(ssh) + L" " + args;

    Utf8ConsoleScope utf8;
    bool timedOut = false;
    const int code = wg::RunInherited(ssh, args, timeoutSec > 0 ? timeoutSec * 1000 : 0,
                                      &timedOut);
    if (timedOut) {
        error = L"remote command timed out after " + std::to_wstring(timeoutSec) + L"s";
        return kExitTimeout;
    }
    if (code < 0) {
        error = L"cannot start ssh: " + ssh;
        return 127;
    }
    return code;
}

} // namespace

std::wstring ResolveSsh(const Config& config) {
    return server::SshPath(config.ssh);
}

std::wstring BuildSshArgs(const Config& config, const std::wstring& target, const SshSpec& spec) {
    std::wstring args = L"-o ConnectTimeout=" +
                        std::to_wstring(std::max(1, spec.connectTimeoutSec)) +
                        L" -o StrictHostKeyChecking=accept-new -o UpdateHostKeys=no -o LogLevel=ERROR";

    if (spec.probe) {
        args += L" -T -o BatchMode=yes";
    } else if (spec.tty) {
        args += L" -t";
    } else if (spec.noTty) {
        args += L" -T";
    }

    const std::wstring key = Trim(config.key);
    if (!key.empty()) args += L" -i " + wg::QuoteArg(key);

    args += L" " + wg::QuoteArg(target);
    if (spec.probe) {
        args += L" sh -s";
    } else {
        for (const auto& word : spec.command) args += L" " + wg::QuoteArg(word);
    }
    return args;
}

std::vector<server::HostStatus> ProbeTargets(const Config& config,
                                             const std::wstring& sshPath,
                                             const std::vector<std::wstring>& targets,
                                             int timeoutMs) {
    std::vector<server::HostStatus> statuses(targets.size());

    if (sshPath.empty()) {
        for (std::size_t i = 0; i < targets.size(); ++i) {
            statuses[i].target = targets[i];
            statuses[i].name = server::DisplayName(targets[i]);
            statuses[i].state = L"NO SSH";
            statuses[i].error = L"OPENSSH CLIENT NOT FOUND";
        }
        return statuses;
    }

    std::vector<std::thread> workers;
    workers.reserve(targets.size());
    for (std::size_t i = 0; i < targets.size(); ++i) {
        try {
            workers.emplace_back([&, i] {
                try {
                    statuses[i] = server::ProbeHost(targets[i], sshPath, config.key, timeoutMs);
                } catch (...) {
                    statuses[i].target = targets[i];
                    statuses[i].name = server::DisplayName(targets[i]);
                    statuses[i].state = L"ERROR";
                    statuses[i].error = L"PROBE FAILED";
                }
            });
        } catch (...) {
            statuses[i] = server::ProbeHost(targets[i], sshPath, config.key, timeoutMs);
        }
    }
    for (auto& worker : workers) worker.join();
    return statuses;
}

int ExecRemote(const Config& config, const std::wstring& target,
               const std::vector<std::wstring>& command, int timeoutSec, bool tty,
               std::wstring* effective, std::wstring& error) {
    return RunRemote(config, target, command, timeoutSec, tty, !tty, false, effective, error);
}

int ShellRemote(const Config& config, const std::wstring& target,
                const std::vector<std::wstring>& command, int timeoutSec, bool tty,
                std::wstring* effective, std::wstring& error) {
    return RunRemote(config, target, command, timeoutSec, tty, false, false, effective, error);
}

std::wstring SshVersionLine(const std::wstring& sshPath, bool& ok) {
    ok = false;
    if (sshPath.empty()) return {};
    const wg::RunResult run = wg::RunCapture(sshPath, L"-V", "", 4000);
    const std::wstring line = FirstLine(run.output);
    ok = run.started && !line.empty();
    return line;
}

} // namespace srv
