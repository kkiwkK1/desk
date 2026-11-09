#include "providers.h"

#include "widget/proc.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>

namespace server {
namespace {

// Read-only probe. Every block is guarded so an old or non-Linux host still
// returns the keys it does understand instead of failing the whole call.
// Keep a trailing newline: dash needs it to execute the final "fi".
constexpr char kProbeScript[] =
    "LC_ALL=C\n"
    "echo \"LOAD=$(cut -d' ' -f1 /proc/loadavg 2>/dev/null)\"\n"
    "echo \"CORES=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null)\"\n"
    "echo \"MEMT=$(awk '/^MemTotal:/{print $2}' /proc/meminfo 2>/dev/null)\"\n"
    "echo \"MEMA=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo 2>/dev/null)\"\n"
    "echo \"DSK=$(df -P / 2>/dev/null | awk 'NR==2{gsub(/%/,\"\",$5); print $5}')\"\n"
    "echo \"UP=$(cut -d. -f1 /proc/uptime 2>/dev/null)\"\n"
    "if read -r _ u n s i w irq sirq st _ < /proc/stat 2>/dev/null; then\n"
    "  t1=$((u+n+s+i+w+irq+sirq+st)); i1=$((i+w))\n"
    "  sleep 1\n"
    "  if read -r _ u n s i w irq sirq st _ < /proc/stat 2>/dev/null; then\n"
    "    t2=$((u+n+s+i+w+irq+sirq+st)); i2=$((i+w))\n"
    "    if [ \"$t2\" -gt \"$t1\" ]; then\n"
    "      echo \"CPU=$((100*(t2-t1-i2+i1)/(t2-t1)))\"\n"
    "    fi\n"
    "  fi\n"
    "fi\n";

std::wstring TrimW(const std::wstring& s) {
    size_t begin = 0, end = s.size();
    while (begin < end && iswspace(s[begin])) ++begin;
    while (end > begin && iswspace(s[end - 1])) --end;
    return s.substr(begin, end - begin);
}

std::string TrimA(const std::string& s) {
    size_t begin = 0, end = s.size();
    while (begin < end && (unsigned char)s[begin] <= ' ') ++begin;
    while (end > begin && (unsigned char)s[end - 1] <= ' ') --end;
    return s.substr(begin, end - begin);
}

std::wstring Unquote(const std::wstring& s) {
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), (int)s.size(),
                                nullptr, 0);
    UINT cp = CP_UTF8;
    if (n <= 0) {
        cp = CP_ACP;
        n = MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), nullptr, 0);
    }
    if (n <= 0) return {};
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

std::string FirstMeaningfulLine(const std::string& text) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = TrimA(text.substr(start, end - start));
        start = end + 1;
        if (line.empty()) continue;
        if (line.compare(0, 7, "Warning") == 0) continue;
        if (line.compare(0, 5, "debug") == 0) continue;
        return line;
    }
    return {};
}

bool ParseInt(const std::string& s, long long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

bool ParseDouble(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

int ClampPercent(long long v) {
    if (v < 0) return 0;
    if (v > 100) return 100;
    return (int)v;
}

} // namespace

std::vector<std::wstring> ParseHosts(const std::wstring& text) {
    std::vector<std::wstring> hosts;
    std::wstring current;

    auto flush = [&] {
        std::wstring token = TrimW(current);
        current.clear();
        if (token.empty() || token.size() > 128 || token.front() == L'-') return;
        for (const auto& existing : hosts) {
            if (_wcsicmp(existing.c_str(), token.c_str()) == 0) return;
        }
        if (hosts.size() < kMaxHosts) hosts.push_back(std::move(token));
    };

    for (wchar_t c : text) {
        if (c == L',' || c == L';' || iswspace(c)) flush();
        else current.push_back(c);
    }
    flush();
    return hosts;
}

std::wstring JoinHosts(const std::vector<std::wstring>& hosts) {
    std::wstring out;
    for (size_t i = 0; i < hosts.size(); ++i) {
        if (i) out += L", ";
        out += hosts[i];
    }
    return out;
}

std::wstring DisplayName(const std::wstring& target) {
    std::wstring name = TrimW(target);
    const size_t at = name.find_last_of(L'@');
    if (at != std::wstring::npos && at + 1 < name.size()) name = name.substr(at + 1);
    if (name.size() >= 2 && name.front() == L'[' && name.back() == L']') {
        name = name.substr(1, name.size() - 2);
    }
    return name.empty() ? target : name;
}

bool ParseProbeOutput(const std::string& text, ProbeOutput& out) {
    unsigned long long memTotal = 0, memAvailable = 0;
    bool sawTotal = false, sawAvailable = false;

    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = TrimA(text.substr(start, end - start));
        start = end + 1;

        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = TrimA(line.substr(0, eq));
        const std::string value = TrimA(line.substr(eq + 1));
        if (value.empty()) continue;

        long long n = 0;
        double d = 0.0;
        if (key == "LOAD") {
            if (ParseDouble(value, d) && d >= 0.0) out.load1 = d;
        } else if (key == "CORES") {
            if (ParseInt(value, n) && n > 0 && n < 4096) out.cores = (int)n;
        } else if (key == "MEMT") {
            if (ParseInt(value, n) && n > 0) {
                memTotal = (unsigned long long)n;
                sawTotal = true;
            }
        } else if (key == "MEMA") {
            if (ParseInt(value, n) && n >= 0) {
                memAvailable = (unsigned long long)n;
                sawAvailable = true;
            }
        } else if (key == "DSK") {
            if (ParseInt(value, n)) out.diskPercent = ClampPercent(n);
        } else if (key == "UP") {
            if (ParseInt(value, n) && n >= 0) out.uptimeSec = n;
        } else if (key == "CPU") {
            if (ParseInt(value, n)) out.cpuPercent = ClampPercent(n);
        }
    }

    if (sawTotal && sawAvailable && memTotal > 0 && memAvailable <= memTotal) {
        const long long used = (long long)(memTotal - memAvailable);
        out.memPercent = ClampPercent(used * 100 / (long long)memTotal);
    }
    return out.any();
}

std::wstring ClassifyError(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    auto has = [&](const char* needle) { return lower.find(needle) != std::string::npos; };

    if (has("host key verification failed")) return L"HOSTKEY";
    if (has("permission denied") || has("publickey")) return L"AUTH";
    if (has("could not resolve") || has("name or service not known") ||
        has("no such host")) {
        return L"DNS";
    }
    if (has("connection refused")) return L"REFUSED";
    if (has("timed out") || has("timeout")) return L"TIMEOUT";
    if (has("no route to host")) return L"UNREACH";
    if (has("closed by remote host") || has("connection closed")) return L"CLOSED";
    return L"SSH ERR";
}

std::wstring SshPath(const std::wstring& overridePath) {
    const std::wstring override = Unquote(TrimW(overridePath));
    if (!override.empty()) {
        const DWORD attrs = GetFileAttributesW(override.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            return override;
        }
        wchar_t found[MAX_PATH]{};
        if (SearchPathW(nullptr, override.c_str(), L".exe", MAX_PATH, found, nullptr)) {
            return found;
        }
        return {};
    }

    wchar_t systemDir[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring candidate = std::wstring(systemDir) + L"\\OpenSSH\\ssh.exe";
        const DWORD attrs = GetFileAttributesW(candidate.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            return candidate;
        }
    }

    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, L"ssh.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return {};
}

HostStatus ProbeHost(const std::wstring& target, const std::wstring& sshPath,
                     const std::wstring& identityPath, int timeoutMs) {
    HostStatus status;
    status.target = target;
    status.name = DisplayName(target);

    if (sshPath.empty()) {
        status.state = L"NO SSH";
        status.error = L"OPENSSH CLIENT NOT FOUND";
        return status;
    }

    std::wstring args =
        L"-T -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new"
        L" -o LogLevel=ERROR";
    const std::wstring identity = Unquote(TrimW(identityPath));
    if (!identity.empty()) args += L" -i " + wg::QuoteArg(identity);
    args += L" " + wg::QuoteArg(target) + L" sh -s";

    const auto started = std::chrono::steady_clock::now();
    const wg::RunResult run = wg::RunCapture(sshPath, args, kProbeScript, timeoutMs);
    const auto finished = std::chrono::steady_clock::now();
    status.rttMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(finished - started)
                       .count();

    if (!run.started) {
        status.state = L"SSH ERR";
        status.error = L"CANNOT START " + sshPath;
        return status;
    }
    if (run.timedOut) {
        status.state = L"TIMEOUT";
        status.error = L"NO RESPONSE IN " + std::to_wstring(timeoutMs / 1000) + L"S";
        return status;
    }
    if (run.exitCode != 0) {
        std::wstring state = ClassifyError(run.output);
        const std::string line = FirstMeaningfulLine(run.output);
        // Some Windows ssh failures are silent on a pipe. A slow, silent exit
        // right around ssh's connect timeout is a timeout, not a mystery error.
        if (state == L"SSH ERR" && line.empty() && status.rttMs >= 4000) state = L"TIMEOUT";
        status.state = state;
        if (line.empty()) {
            status.error = state == L"TIMEOUT"
                               ? L"NO RESPONSE IN " + std::to_wstring(status.rttMs / 1000) + L"S"
                               : L"SSH EXIT " + std::to_wstring(run.exitCode);
        } else {
            status.error = Widen(line);
        }
        return status;
    }

    ProbeOutput probe;
    if (!ParseProbeOutput(run.output, probe)) {
        status.state = L"NO DATA";
        status.error = L"NO /PROC METRICS (LINUX ONLY)";
        return status;
    }

    status.ok = true;
    status.state = L"UP";
    status.cpuPercent = probe.cpuPercent;
    status.memPercent = probe.memPercent;
    status.diskPercent = probe.diskPercent;
    status.load1 = probe.load1;
    status.cores = probe.cores;
    status.uptimeSec = probe.uptimeSec;
    return status;
}

std::wstring FormatUptime(long long seconds) {
    if (seconds < 0) return L"--";
    wchar_t buf[24];
    if (seconds >= 86400) swprintf(buf, 24, L"%lldD", seconds / 86400);
    else if (seconds >= 3600) swprintf(buf, 24, L"%lldH", seconds / 3600);
    else swprintf(buf, 24, L"%lldM", seconds / 60);
    return buf;
}

std::wstring FormatLoad(double load) {
    if (load < 0.0) return L"--";
    wchar_t buf[24];
    if (load >= 100.0) swprintf(buf, 24, L"%.0f", load);
    else swprintf(buf, 24, L"%.2f", load);
    return buf;
}

} // namespace server
