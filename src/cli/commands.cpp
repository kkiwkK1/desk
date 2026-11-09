#include "commands.h"

#include "help.h"
#include "remote.h"
#include "targets.h"
#include "data/server/providers.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace srv {
namespace {

volatile LONG g_stop = 0;

BOOL WINAPI StopHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&g_stop, 1);
        return TRUE;
    }
    return FALSE;
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

Color PercentColor(int value) {
    if (value < 0) return Color::Dim;
    if (value >= 90) return Color::Red;
    if (value >= 75) return Color::Yellow;
    return Color::Green;
}

Color StateColor(const server::HostStatus& host) {
    if (!host.ok) return Color::Red;
    const int worst = std::max(host.cpuPercent, std::max(host.memPercent, host.diskPercent));
    return worst < 0 ? Color::Green : PercentColor(worst);
}

std::wstring HintFor(const std::wstring& state, const std::wstring& target) {
    if (state == L"TIMEOUT") return L"host unreachable, firewall dropping SSH, or wrong port; try: ssh -v " + target;
    if (state == L"AUTH") return L"key rejected; check User / IdentityFile; try: ssh -v " + target;
    if (state == L"DNS") return L"hostname does not resolve; check ~/.ssh/config and DNS";
    if (state == L"REFUSED") return L"sshd is not running or the port is wrong";
    if (state == L"HOSTKEY") return L"verify the fingerprint, then: ssh-keygen -R " + server::DisplayName(target);
    if (state == L"UNREACH") return L"no route to host; check VPN / network";
    if (state == L"CLOSED") return L"remote closed the connection during auth";
    if (state == L"NO SSH") return L"install the OpenSSH Client, or set ssh= in srvctl.ini";
    if (state == L"NO DATA") return L"target is not Linux, or /proc is not readable";
    if (state == L"ERROR") return L"probe crashed; rerun with -v and inspect ssh directly";
    return L"try: ssh -v " + target;
}

std::wstring Trim(const std::wstring& text) {
    std::size_t begin = 0, end = text.size();
    while (begin < end && std::iswspace(text[begin])) ++begin;
    while (end > begin && std::iswspace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

bool ReadLineFromConsole(std::wstring& line) {
    line.clear();
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);

    DWORD mode = 0;
    if (GetConsoleMode(input, &mode)) {
        wchar_t buffer[256]{};
        DWORD read = 0;
        if (!ReadConsoleW(input, buffer, 256, &read, nullptr) || read == 0) return false;
        line.assign(buffer, read);
    } else {
        std::string bytes;
        for (;;) {
            char c = 0;
            DWORD read = 0;
            if (!ReadFile(input, &c, 1, &read, nullptr) || read == 0) break;
            if (c == '\n') break;
            if (c != '\r') bytes.push_back(c);
            if (bytes.size() > 1024) break;
        }
        if (bytes.empty()) return false;
        line = Widen(bytes);
    }

    while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) line.pop_back();
    return true;
}

// Resolves the host for 'ssh' when none was given. One configured host connects
// directly; several show a small numbered picker in the console.
int PickHost(const Config& config, Output& out, std::wstring& target) {
    target.clear();

    if (config.hosts.empty()) {
        out.Err(L"srvctl: no hosts configured");
        out.Err(L"Add one with 'srvctl hosts add web-01', or use 'srvctl ssh web-01'.");
        return kExitConfig;
    }
    if (config.hosts.size() == 1) {
        target = config.hosts.front();
        return kExitOk;
    }
    if (!out.console()) {
        out.Err(L"srvctl: ssh needs a host when input is redirected");
        return kExitUsage;
    }

    out.Line(Paint(L"SSH hosts", Color::Bold, out.color()));
    for (std::size_t i = 0; i < config.hosts.size(); ++i) {
        out.Line(L"  " + std::to_wstring(i + 1) + L") " + config.hosts[i]);
    }
    out.Line();
    out.Raw(L"Select host [1-" + std::to_wstring(config.hosts.size()) +
            L"] (number/alias, q to quit): ");

    std::wstring answer;
    if (!ReadLineFromConsole(answer)) {
        out.Line();
        return kExitUsage;
    }
    out.Line();

    answer = Trim(answer);
    if (answer.empty() || _wcsicmp(answer.c_str(), L"q") == 0 ||
        _wcsicmp(answer.c_str(), L"quit") == 0) {
        return kExitOk;
    }

    wchar_t* end = nullptr;
    const long index = std::wcstol(answer.c_str(), &end, 10);
    if (end && *end == L'\0' && index >= 1 &&
        (std::size_t)index <= config.hosts.size()) {
        target = config.hosts[(std::size_t)index - 1];
        return kExitOk;
    }

    for (const auto& host : config.hosts) {
        if (_wcsicmp(host.c_str(), answer.c_str()) == 0) {
            target = host;
            return kExitOk;
        }
    }

    out.Err(L"srvctl: no such host: " + answer);
    return kExitUsage;
}

bool FlattenTargets(const std::vector<std::wstring>& args, std::vector<std::wstring>& out,
                    std::wstring& error) {
    std::unordered_set<std::wstring> seen;
    for (const auto& arg : args) {
        const std::vector<std::wstring> tokens = TokenizeTargets(arg);
        if (tokens.empty()) {
            error = L"empty target";
            return false;
        }
        for (const auto& token : tokens) {
            std::wstring why;
            if (!ValidTarget(token, &why)) {
                error = L"invalid target '" + token + L"': " + why;
                return false;
            }
            if (!seen.insert(Lower(token)).second) continue;
            if (out.size() >= kMaxTargets) {
                error = L"too many hosts (maximum " + std::to_wstring(kMaxTargets) + L")";
                return false;
            }
            out.push_back(token);
        }
    }
    return true;
}

std::string StatusJson(const Config& config, const std::wstring& ssh,
                       const std::vector<server::HostStatus>& hosts, int timeoutSec,
                       long long elapsedMs) {
    std::size_t up = 0;
    for (const auto& host : hosts) {
        if (host.ok) ++up;
    }

    std::string j = "{";
    j += "\"version\":" + JsonString(kVersion);
    j += ",\"timestamp\":" + JsonString(TimestampNow());
    j += ",\"epoch\":" + std::to_string(UnixNow());
    j += ",\"elapsedMs\":" + std::to_string(elapsedMs);
    j += ",\"timeoutSec\":" + std::to_string(timeoutSec);
    j += ",\"ssh\":" + JsonString(ssh);
    j += ",\"config\":" + JsonString(config.path);
    j += ",\"configSource\":" + JsonString(config.sourcePath);
    j += ",\"inherited\":" + std::string(config.inherited ? "true" : "false");
    j += ",\"hosts\":[";
    for (std::size_t i = 0; i < hosts.size(); ++i) {
        const auto& h = hosts[i];
        if (i) j += ',';
        j += "{";
        j += "\"target\":" + JsonString(h.target);
        j += ",\"name\":" + JsonString(h.name);
        j += ",\"ok\":" + std::string(h.ok ? "true" : "false");
        j += ",\"state\":" + JsonString(h.state);
        j += ",\"cpuPercent\":" + JsonIntOrNull(h.cpuPercent);
        j += ",\"memPercent\":" + JsonIntOrNull(h.memPercent);
        j += ",\"diskPercent\":" + JsonIntOrNull(h.diskPercent);
        j += ",\"load1\":" + (h.load1 < 0.0 ? std::string("null") : JsonDouble(h.load1));
        j += ",\"cores\":" + (h.cores > 0 ? std::to_string(h.cores) : std::string("null"));
        j += ",\"uptimeSec\":" + JsonLongOrNull(h.uptimeSec);
        j += ",\"rttMs\":" + JsonIntOrNull(h.rttMs);
        j += ",\"error\":" + JsonString(h.error);
        j += "}";
    }
    j += "],\"summary\":{\"total\":" + std::to_string(hosts.size()) +
         ",\"up\":" + std::to_string(up) +
         ",\"failed\":" + std::to_string(hosts.size() - up) + "}}";
    return j;
}

void PrintStatusTable(const Config& config, const std::wstring& ssh,
                      const std::vector<server::HostStatus>& hosts, int timeoutSec,
                      long long elapsedMs, bool watch, int watchSec, bool verbose,
                      Output& out) {
    std::size_t up = 0;
    bool anyFailed = false;
    for (const auto& host : hosts) {
        if (host.ok) ++up;
        else anyFailed = true;
    }

    std::wstring heading = TimestampNow() + L"  |  " + std::to_wstring(up) + L"/" +
                           std::to_wstring(hosts.size()) + L" up  |  " +
                           std::to_wstring(elapsedMs) + L"ms";
    if (anyFailed) heading += L"  |  " + std::to_wstring(hosts.size() - up) + L" failed";
    if (watch) {
        heading += L"  |  refresh " + std::to_wstring(watchSec) + L"s  |  Ctrl+C to stop";
    }
    out.Line(Paint(heading, Color::Dim, out.color()));

    std::vector<std::wstring> headers = {L"HOST", L"STATE", L"CPU", L"MEM", L"DSK",
                                         L"LOAD", L"UPTIME", L"RTT"};
    if (verbose) headers.push_back(L"CORES");
    if (anyFailed) headers.push_back(L"DETAIL");

    std::vector<std::vector<Cell>> rows;
    rows.reserve(hosts.size());
    for (const auto& host : hosts) {
        std::vector<Cell> row;
        row.push_back(Cell{TruncateTo(host.target, 32), Color::None, false});
        row.push_back(Cell{host.ok ? L"UP" : host.state, StateColor(host), false});
        row.push_back(Cell{FormatPercent(host.cpuPercent), PercentColor(host.cpuPercent), true});
        row.push_back(Cell{FormatPercent(host.memPercent), PercentColor(host.memPercent), true});
        row.push_back(Cell{FormatPercent(host.diskPercent), PercentColor(host.diskPercent), true});
        row.push_back(Cell{FormatLoadValue(host.load1), Color::None, true});
        row.push_back(Cell{FormatUptimeValue(host.uptimeSec), Color::None, true});
        row.push_back(Cell{FormatRtt(host.rttMs), Color::Dim, true});
        if (verbose) {
            row.push_back(Cell{host.cores > 0 ? std::to_wstring(host.cores) : std::wstring(L"--"),
                               Color::None, true});
        }
        if (anyFailed) {
            row.push_back(Cell{host.ok ? std::wstring() : host.error, Color::Dim, false});
        }
        rows.push_back(std::move(row));
    }

    out.Line(RenderTable(headers, rows, out.color(), out.width()));

    if (verbose) {
        out.Line(Paint(L"ssh     " + (ssh.empty() ? std::wstring(L"(not found)") : ssh),
                       Color::Dim, out.color()));
        out.Line(Paint(L"config  " + config.path +
                           (config.inherited ? L" (inherited)" : L"") +
                           L"   timeout " + std::to_wstring(timeoutSec) + L"s",
                       Color::Dim, out.color()));
    }
    out.Line();
}

int FlattenOrReport(const std::vector<std::wstring>& args, std::vector<std::wstring>& targets,
                    Output& out) {
    std::wstring error;
    if (!FlattenTargets(args, targets, error)) {
        out.Err(L"srvctl: " + error);
        return kExitUsage;
    }
    return kExitOk;
}

} // namespace

int CmdStatus(const Options& options, const Config& config, Output& out) {
    std::vector<std::wstring> targets;
    if (options.targets.empty()) {
        targets = config.hosts;
    } else {
        const int rc = FlattenOrReport(options.targets, targets, out);
        if (rc != kExitOk) return rc;
    }

    if (targets.empty()) {
        out.Err(L"srvctl: no hosts configured");
        out.Err(L"Add one with 'srvctl hosts add web-01', or pass a host: "
                L"'srvctl status web-01'.");
        return kExitConfig;
    }

    const int timeoutSec = options.timeoutSec > 0 ? options.timeoutSec : config.timeoutSec;
    const std::wstring ssh = ResolveSsh(config);
    if (ssh.empty()) {
        out.Err(L"srvctl: OpenSSH client not found; install it or set ssh= in the config.");
    }

    if (options.watch) {
        InterlockedExchange(&g_stop, 0);
        SetConsoleCtrlHandler(StopHandler, TRUE);
    }

    int lastCode = kExitOk;
    auto nextRefresh = std::chrono::steady_clock::now();
    for (;;) {
        const auto begin = std::chrono::steady_clock::now();
        const std::vector<server::HostStatus> hosts =
            ProbeTargets(config, ssh, targets, timeoutSec * 1000);
        const long long elapsedMs =
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin)
                .count();

        std::size_t up = 0;
        for (const auto& host : hosts) {
            if (host.ok) ++up;
        }
        if (up == hosts.size()) lastCode = kExitOk;
        else if (up == 0) lastCode = ssh.empty() ? kExitConfig : kExitFailure;
        else lastCode = kExitPartial;

        if (options.json) {
            out.LineUtf8(StatusJson(config, ssh, hosts, timeoutSec, elapsedMs));
        } else {
            if (options.watch && out.console()) out.Clear();
            PrintStatusTable(config, ssh, hosts, timeoutSec, elapsedMs, options.watch,
                             options.watchSec, options.verbose, out);
        }

        if (!options.watch) break;
        if (InterlockedCompareExchange(&g_stop, 0, 0) != 0) break;

        // Schedule from the previous target, not from "now": a 1.5s probe with
        // --watch 5 should refresh every 5s, not every 6.5s. If the probe is
        // slower than the interval, skip the wait and start the next one.
        nextRefresh += std::chrono::seconds(options.watchSec);
        const auto now = std::chrono::steady_clock::now();
        if (nextRefresh < now) nextRefresh = now;
        while (std::chrono::steady_clock::now() < nextRefresh) {
            if (InterlockedCompareExchange(&g_stop, 0, 0) != 0) break;
            Sleep(100);
        }
        if (InterlockedCompareExchange(&g_stop, 0, 0) != 0) break;
    }

    if (options.watch) {
        SetConsoleCtrlHandler(StopHandler, FALSE);
        return kExitOk;
    }
    return lastCode;
}

int CmdExec(const Options& options, const Config& config, Output& out) {
    const std::wstring target = options.targets.front();
    std::wstring effective;
    std::wstring error;
    const int code = ExecRemote(config, target, options.remoteArgs, options.timeoutSec,
                                options.tty, options.verbose ? &effective : nullptr, error);
    if (!effective.empty()) out.Err(L"+ " + effective);
    if (!error.empty()) out.Err(L"srvctl: " + error);
    return code;
}

int CmdSsh(const Options& options, const Config& config, Output& out) {
    std::wstring target;
    const bool picked = options.targets.empty();
    if (picked) {
        const int rc = PickHost(config, out, target);
        if (rc != kExitOk) return rc;
        if (target.empty()) return kExitOk;  // q / empty line
    } else {
        target = options.targets.front();
    }

    if (picked) out.Line(Paint(L"Connecting to " + target + L"...", Color::Dim, out.color()));

    std::wstring effective;
    std::wstring error;
    const int code = ShellRemote(config, target, options.remoteArgs, options.timeoutSec,
                                 options.tty, options.verbose ? &effective : nullptr, error);
    if (!effective.empty()) out.Err(L"+ " + effective);
    if (!error.empty()) out.Err(L"srvctl: " + error);
    return code;
}

int CmdHosts(const Options& options, Config& config, Output& out) {
    const std::wstring action = options.hostsAction;

    if (action == L"list") {
        if (options.json) {
            std::string j = "{";
            j += "\"config\":" + JsonString(config.path);
            j += ",\"configSource\":" + JsonString(config.sourcePath);
            j += ",\"exists\":" + std::string(config.exists ? "true" : "false");
            j += ",\"inherited\":" + std::string(config.inherited ? "true" : "false");
            j += ",\"ssh\":" + JsonString(ResolveSsh(config));
            j += ",\"timeoutSec\":" + std::to_string(config.timeoutSec);
            j += ",\"hosts\":[";
            for (std::size_t i = 0; i < config.hosts.size(); ++i) {
                if (i) j += ',';
                j += JsonString(config.hosts[i]);
            }
            j += "]}";
            out.LineUtf8(j);
            return kExitOk;
        }

        out.Line(Paint(L"CONFIG  " +
                           (config.inherited ? config.sourcePath : config.path),
                       Color::Bold, out.color()));
        if (config.inherited) {
            out.Line(L"        inherited; edits create " + config.path);
            out.Line(L"        the widget config is not touched");
        } else if (!config.exists) {
            out.Line(L"        not created yet; defaults in use");
        }
        const std::wstring ssh = ResolveSsh(config);
        out.Line(L"SSH     " + (ssh.empty() ? std::wstring(L"(not found)") : ssh));
        out.Line(L"HOSTS   " + std::to_wstring(config.hosts.size()));
        out.Line();

        if (config.hosts.empty()) {
            out.Line(L"  (none)");
            out.Line(L"  Add one with 'srvctl hosts add web-01'.");
        } else {
            std::vector<std::vector<Cell>> rows;
            for (std::size_t i = 0; i < config.hosts.size(); ++i) {
                rows.push_back({Cell{std::to_wstring(i + 1), Color::Dim, true},
                                Cell{config.hosts[i], Color::None, false}});
            }
            out.Line(RenderTable({L"#", L"HOST"}, rows, out.color(), out.width()));
        }
        return kExitOk;
    }

    if (options.json) {
        out.Err(L"srvctl: --json is only supported by 'hosts list'");
        return kExitUsage;
    }

    if (action == L"clear") {
        const bool inheritedBefore = config.inherited;
        const std::wstring sourceBefore = config.sourcePath;
        const std::size_t removed = config.hosts.size();
        config.hosts.clear();
        std::wstring error;
        if (!config.Save(error)) {
            out.Err(L"srvctl: " + error);
            return kExitConfig;
        }
        out.Line(L"cleared " + std::to_wstring(removed) + L" host(s)");
        if (inheritedBefore) {
            out.Line(L"created " + config.path + L" (seeded from " + sourceBefore + L")");
        } else {
            out.Line(L"config " + config.path);
        }
        return kExitOk;
    }

    std::vector<std::wstring> requested;
    {
        std::wstring error;
        if (!FlattenTargets(options.targets, requested, error)) {
            out.Err(L"srvctl: " + error);
            return kExitUsage;
        }
    }

    if (requested.empty()) {
        out.Err(L"srvctl: no targets given");
        return kExitUsage;
    }

    const bool inheritedBefore = config.inherited;
    const std::wstring sourceBefore = config.sourcePath;
    bool saved = false;

    if (action == L"set") {
        if (requested.size() > kMaxTargets) {
            out.Err(L"srvctl: too many hosts (maximum " + std::to_wstring(kMaxTargets) + L")");
            return kExitUsage;
        }
        config.hosts = requested;
        std::wstring error;
        if (!config.Save(error)) {
            out.Err(L"srvctl: " + error);
            return kExitConfig;
        }
        saved = true;
        out.Line(L"set " + std::to_wstring(config.hosts.size()) + L" host(s)");
    } else if (action == L"add") {
        std::unordered_set<std::wstring> seen;
        for (const auto& host : config.hosts) seen.insert(Lower(host));
        std::vector<std::wstring> added;
        std::vector<std::wstring> present;
        for (const auto& host : requested) {
            if (seen.insert(Lower(host)).second) {
                if (config.hosts.size() >= kMaxTargets) {
                    out.Err(L"srvctl: too many hosts (maximum " + std::to_wstring(kMaxTargets) +
                            L"); remove one first");
                    return kExitConfig;
                }
                config.hosts.push_back(host);
                added.push_back(host);
            } else {
                present.push_back(host);
            }
        }
        if (!added.empty()) {
            std::wstring error;
            if (!config.Save(error)) {
                out.Err(L"srvctl: " + error);
                return kExitConfig;
            }
            saved = true;
        }
        for (const auto& host : added) out.Line(L"added    " + host);
        for (const auto& host : present) out.Line(L"present  " + host);
        if (added.empty()) out.Line(L"no changes");
        if (config.hosts.size() > kWidgetHosts) {
            out.Line(Paint(L"note: server.exe shows the first " + std::to_wstring(kWidgetHosts) +
                               L" hosts",
                           Color::Yellow, out.color()));
        }
    } else if (action == L"remove") {
        std::vector<std::wstring> remaining = config.hosts;
        std::vector<std::wstring> removed;
        std::vector<std::wstring> missing;
        bool ambiguous = false;

        for (const auto& wanted : requested) {
            std::vector<std::size_t> exact;
            for (std::size_t i = 0; i < remaining.size(); ++i) {
                if (_wcsicmp(remaining[i].c_str(), wanted.c_str()) == 0) exact.push_back(i);
            }
            std::size_t index = (std::size_t)-1;
            if (!exact.empty()) {
                index = exact.front();
            } else {
                std::vector<std::size_t> names;
                for (std::size_t i = 0; i < remaining.size(); ++i) {
                    if (_wcsicmp(server::DisplayName(remaining[i]).c_str(), wanted.c_str()) == 0) {
                        names.push_back(i);
                    }
                }
                if (names.size() == 1) {
                    index = names.front();
                } else if (names.size() > 1) {
                    ambiguous = true;
                    out.Err(L"srvctl: '" + wanted +
                            L"' matches several hosts; use the full user@host target");
                    continue;
                }
            }

            if (index == (std::size_t)-1) {
                missing.push_back(wanted);
                continue;
            }
            removed.push_back(remaining[index]);
            remaining.erase(remaining.begin() + (std::ptrdiff_t)index);
        }

        if (ambiguous || !missing.empty()) {
            for (const auto& host : missing) out.Err(L"srvctl: not configured: " + host);
            return kExitFailure;
        }

        config.hosts = remaining;
        std::wstring error;
        if (!config.Save(error)) {
            out.Err(L"srvctl: " + error);
            return kExitConfig;
        }
        saved = true;
        for (const auto& host : removed) out.Line(L"removed  " + host);
    }

    if (saved) {
        if (inheritedBefore) {
            out.Line(L"created " + config.path + L" (seeded from " + sourceBefore + L")");
        } else {
            out.Line(L"config " + config.path);
        }
    }
    return kExitOk;
}

int CmdDoctor(const Options& options, const Config& config, Output& out) {
    bool failed = false;
    const std::wstring ssh = ResolveSsh(config);

    auto heading = [&](const wchar_t* text) {
        out.Line();
        out.Line(Paint(text, Color::Bold, out.color()));
    };
    auto diag = [&](const wchar_t* tag, Color color, const std::wstring& label,
                    const std::wstring& text) {
        std::wstring left = tag;
        while (DisplayWidth(left) < 7) left += L' ';
        std::wstring key = label;
        while (DisplayWidth(key) < 13) key += L' ';
        out.Line(L"  " + Paint(left, color, out.color()) + key + text);
    };
    auto hint = [&](const std::wstring& text) { out.Line(L"         hint: " + text); };

    heading(L"SSH CLIENT");
    if (ssh.empty()) {
        failed = true;
        diag(L"[fail]", Color::Red, L"path", L"OpenSSH client not found");
        hint(L"install OpenSSH Client, or set ssh= in " + config.path);
    } else {
        diag(L"[ok]", Color::Green, L"path", ssh);
        bool versionOk = false;
        const std::wstring version = SshVersionLine(ssh, versionOk);
        if (versionOk) diag(L"[ok]", Color::Green, L"version", version);
        else diag(L"[warn]", Color::Yellow, L"version", L"could not run 'ssh -V'");
    }

    heading(L"CONFIG");
    if (config.inherited) {
        diag(L"[ok]", Color::Green, L"file", config.sourcePath + L" (inherited)");
        diag(L"[--]", Color::Dim, L"edits", L"create " + config.path);
    } else if (config.exists) {
        diag(L"[ok]", Color::Green, L"file", config.path);
    } else {
        diag(L"[--]", Color::Dim, L"file", config.path + L" (not created; defaults in use)");
    }
    diag(L"[--]", Color::Dim, L"hosts",
         config.hosts.empty() ? std::wstring(L"none configured")
                              : std::to_wstring(config.hosts.size()) + L" configured");
    diag(L"[--]", Color::Dim, L"timeout", std::to_wstring(config.timeoutSec) + L"s");

    heading(L"SSH FILES");
    std::wstring profile;
    {
        std::wstring buf(32768, L'\0');
        const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf.data(), (DWORD)buf.size());
        if (n > 0 && n < buf.size()) profile.assign(buf.data(), n);
    }
    if (profile.empty()) {
        diag(L"[--]", Color::Dim, L"USERPROFILE", L"not set");
    } else {
        const std::wstring dir = profile + L"\\.ssh";
        const DWORD attrs = GetFileAttributesW(dir.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            diag(L"[--]", Color::Dim, L"~/.ssh", L"not found (optional)");
        } else {
            diag(L"[ok]", Color::Green, L"~/.ssh", dir);
            const bool sshConfig = GetFileAttributesW((dir + L"\\config").c_str()) !=
                                   INVALID_FILE_ATTRIBUTES;
            const bool knownHosts = GetFileAttributesW((dir + L"\\known_hosts").c_str()) !=
                                    INVALID_FILE_ATTRIBUTES;
            diag(sshConfig ? L"[ok]" : L"[--]", sshConfig ? Color::Green : Color::Dim,
                 L"config", sshConfig ? L"found" : L"not found (optional)");
            diag(knownHosts ? L"[ok]" : L"[--]", knownHosts ? Color::Green : Color::Dim,
                 L"known_hosts", knownHosts ? L"found"
                                            : L"not found (accept-new will create it)");

            std::vector<std::wstring> keys;
            WIN32_FIND_DATAW fd{};
            HANDLE find = FindFirstFileW((dir + L"\\id_*").c_str(), &fd);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    std::wstring name = fd.cFileName;
                    if (name.size() > 4 && name.substr(name.size() - 4) == L".pub") continue;
                    keys.push_back(name);
                } while (FindNextFileW(find, &fd));
                FindClose(find);
            }
            if (keys.empty()) {
                diag(L"[--]", Color::Dim, L"keys", L"no id_* private key in ~/.ssh");
            } else {
                std::wstring list;
                for (std::size_t i = 0; i < keys.size(); ++i) {
                    if (i) list += L", ";
                    list += keys[i];
                }
                diag(L"[ok]", Color::Green, L"keys", list);
            }
        }
    }

    heading(L"CONNECTIVITY");
    std::vector<std::wstring> targets;
    if (!options.targets.empty()) {
        const int rc = FlattenOrReport(options.targets, targets, out);
        if (rc != kExitOk) return rc;
    } else {
        targets = config.hosts;
    }

    if (targets.empty()) {
        failed = true;
        diag(L"[fail]", Color::Red, L"hosts", L"nothing to probe");
        hint(L"add one with 'srvctl hosts add web-01'");
    } else {
        const int timeoutSec = options.timeoutSec > 0 ? options.timeoutSec : config.timeoutSec;
        const auto begin = std::chrono::steady_clock::now();
        const std::vector<server::HostStatus> hosts =
            ProbeTargets(config, ssh, targets, timeoutSec * 1000);
        const long long elapsedMs =
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin)
                .count();

        for (const auto& host : hosts) {
            if (!host.ok) {
                failed = true;
                diag(L"[fail]", Color::Red, host.target, host.state);
                hint(HintFor(host.state, host.target));
                if (!host.error.empty()) out.Line(L"         ssh:  " + host.error);
                continue;
            }
            std::wstring text = L"UP " + FormatRtt(host.rttMs) + L"   CPU " +
                                FormatPercent(host.cpuPercent) + L"  MEM " +
                                FormatPercent(host.memPercent) + L"  DSK " +
                                FormatPercent(host.diskPercent);
            diag(L"[ok]", Color::Green, host.target, text);
        }
        out.Line();
        out.Line(Paint(L"probed " + std::to_wstring(hosts.size()) + L" host(s) in " +
                           std::to_wstring(elapsedMs) + L"ms",
                       Color::Dim, out.color()));
    }

    out.Line();
    if (failed) {
        out.Line(Paint(L"doctor: problems found", Color::Red, out.color()));
        return kExitFailure;
    }
    out.Line(Paint(L"doctor: all checks passed", Color::Green, out.color()));
    return kExitOk;
}

} // namespace srv
