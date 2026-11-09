#include "cli.h"

#include "targets.h"

#include <cwchar>
#include <cwctype>

namespace srv {
namespace {

bool IsDigits(const std::wstring& s) {
    if (s.empty()) return false;
    for (wchar_t c : s) {
        if (!std::iswdigit(c)) return false;
    }
    return true;
}

bool ParseInt(const std::wstring& s, int& out) {
    if (!IsDigits(s)) return false;
    wchar_t* end = nullptr;
    const long value = std::wcstol(s.c_str(), &end, 10);
    if (!end || *end != L'\0' || value < 0 || value > 1000000) return false;
    out = (int)value;
    return true;
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

struct Match {
    bool matched = false;
    bool inlineValue = false;
    std::wstring value;
};

Match MatchOption(const std::wstring& token, const wchar_t* shortName, const wchar_t* longName) {
    Match m;
    if ((shortName && *shortName && token == shortName) ||
        (longName && token == longName)) {
        m.matched = true;
        return m;
    }
    if (longName) {
        const std::wstring prefix = std::wstring(longName) + L"=";
        if (token.rfind(prefix, 0) == 0) {
            m.matched = true;
            m.inlineValue = true;
            m.value = token.substr(prefix.size());
        }
    }
    return m;
}

struct Parser {
    const std::vector<std::wstring>& argv;
    std::size_t i = 1;
    ParseResult result;

    bool Fail(const std::wstring& message, const std::wstring& hint = L"") {
        if (result.ok) {
            result.ok = false;
            result.error = message;
            result.hint = hint;
        }
        return false;
    }

    // Consumes the value for a matched option. Returns false on a missing value.
    bool Value(const std::wstring& option, const Match& m, std::wstring& out) {
        if (m.inlineValue) {
            if (m.value.empty()) {
                Fail(L"option " + option + L" needs a value", L"Try 'srvctl help'.");
                return false;
            }
            out = m.value;
            return true;
        }
        if (i + 1 >= argv.size()) {
            Fail(L"option " + option + L" needs a value", L"Try 'srvctl help'.");
            return false;
        }
        out = argv[++i];
        return true;
    }

    bool SetNumber(const std::wstring& option, const std::wstring& value, int low, int high,
                   int& out) {
        int parsed = 0;
        if (!ParseInt(value, parsed) || parsed < low || parsed > high) {
            Fail(L"option " + option + L" expects a number from " + std::to_wstring(low) +
                     L" to " + std::to_wstring(high),
                 L"Try 'srvctl help'.");
            return false;
        }
        out = parsed;
        return true;
    }

    // Returns false when the token is not an option this tool knows. The caller
    // decides whether that is an error or a positional argument.
    bool ApplyOption(const std::wstring& token, bool& recognized) {
        recognized = true;
        Options& o = result.options;
        Match m;
        std::wstring value;

        if ((m = MatchOption(token, L"-h", L"--help")).matched) {
            o.help = true;
            return true;
        }
        if ((m = MatchOption(token, L"-V", L"--version")).matched) {
            o.version = true;
            return true;
        }
        if ((m = MatchOption(token, L"-v", L"--verbose")).matched) {
            o.verbose = true;
            return true;
        }
        if ((m = MatchOption(token, L"", L"--json")).matched) {
            o.json = true;
            return true;
        }
        if ((m = MatchOption(token, L"", L"--tty")).matched) {
            o.tty = true;
            return true;
        }
        if ((m = MatchOption(token, L"", L"--no-color")).matched) {
            o.color = ColorMode::Never;
            return true;
        }
        if ((m = MatchOption(token, L"-c", L"--config")).matched) {
            if (!Value(L"--config", m, value)) return false;
            o.configPath = value;
            return true;
        }
        if ((m = MatchOption(token, L"", L"--ssh")).matched) {
            if (!Value(L"--ssh", m, value)) return false;
            o.sshPath = value;
            return true;
        }
        if ((m = MatchOption(token, L"-i", L"--key")).matched) {
            if (!Value(L"--key", m, value)) return false;
            o.keyPath = value;
            return true;
        }
        if ((m = MatchOption(token, L"", L"--color")).matched) {
            if (!Value(L"--color", m, value)) return false;
            value = Lower(value);
            if (value == L"auto") o.color = ColorMode::Auto;
            else if (value == L"always") o.color = ColorMode::Always;
            else if (value == L"never") o.color = ColorMode::Never;
            else {
                Fail(L"--color expects auto, always or never", L"Try 'srvctl help'.");
                return false;
            }
            return true;
        }
        if ((m = MatchOption(token, L"-t", L"--timeout")).matched) {
            if (!Value(L"--timeout", m, value)) return false;
            return SetNumber(L"--timeout", value, 1, 3600, o.timeoutSec);
        }
        if ((m = MatchOption(token, L"-w", L"--watch")).matched) {
            o.watch = true;
            int seconds = 5;
            if (m.inlineValue) {
                if (!SetNumber(L"--watch", m.value, 1, 3600, seconds)) return false;
            } else if (i + 1 < argv.size() && IsDigits(argv[i + 1])) {
                ++i;
                if (!SetNumber(L"--watch", argv[i], 1, 3600, seconds)) return false;
            }
            o.watchSec = seconds;
            return true;
        }

        recognized = false;
        return true;
    }

    bool UnknownOption(const std::wstring& token) {
        return Fail(L"unknown option '" + token + L"'", L"Try 'srvctl help'.");
    }
};

std::wstring CanonicalHostsAction(const std::wstring& raw) {
    const std::wstring action = Lower(raw);
    if (action == L"list" || action == L"ls" || action == L"show") return L"list";
    if (action == L"add" || action == L"append") return L"add";
    if (action == L"set" || action == L"replace") return L"set";
    if (action == L"remove" || action == L"rm" || action == L"delete") return L"remove";
    if (action == L"clear" || action == L"reset") return L"clear";
    return {};
}

} // namespace

std::wstring CanonicalCommand(const std::wstring& name) {
    const std::wstring c = Lower(name);
    if (c == L"status" || c == L"st") return L"status";
    if (c == L"exec" || c == L"run") return L"exec";
    if (c == L"ssh" || c == L"shell") return L"ssh";
    if (c == L"hosts" || c == L"host") return L"hosts";
    if (c == L"doctor" || c == L"check" || c == L"diag") return L"doctor";
    if (c == L"help" || c == L"?") return L"help";
    if (c == L"version" || c == L"ver") return L"version";
    return {};
}

ParseResult ParseArgs(const std::vector<std::wstring>& argv) {
    Parser p{argv, 1, ParseResult{}};
    Options& o = p.result.options;

    std::wstring command;
    while (p.i < argv.size()) {
        const std::wstring token = argv[p.i];
        if (token == L"--") {
            ++p.i;
            if (p.i < argv.size()) {
                command = argv[p.i];
                ++p.i;
            }
            break;
        }
        if (token.size() > 1 && token.front() == L'-') {
            bool recognized = false;
            if (!p.ApplyOption(token, recognized)) return p.result;
            if (!recognized) {
                p.UnknownOption(token);
                return p.result;
            }
            ++p.i;
            continue;
        }
        command = token;
        ++p.i;
        break;
    }

    if (!command.empty()) {
        const std::wstring canonical = CanonicalCommand(command);
        if (canonical.empty()) {
            if (o.help || o.version) {
                command.clear();  // --help wins over a mistyped command
            } else {
                p.Fail(L"unknown command '" + command + L"'", L"Try 'srvctl help'.");
                return p.result;
            }
        } else {
            o.command = canonical;
        }
    }

    if (o.command == L"exec" || o.command == L"ssh") {
        bool hostSeen = false;
        while (p.i < argv.size()) {
            const std::wstring token = argv[p.i];
            if (!hostSeen) {
                if (token == L"--") {
                    ++p.i;
                    continue;
                }
                if (token.size() > 1 && token.front() == L'-') {
                    bool recognized = false;
                    if (!p.ApplyOption(token, recognized)) return p.result;
                    if (!recognized) {
                        p.UnknownOption(token);
                        return p.result;
                    }
                    ++p.i;  // ApplyOption consumed the value, if any
                    continue;
                }
                o.targets.push_back(token);
                hostSeen = true;
                ++p.i;
                continue;
            }
            ++p.i;
            if (o.remoteArgs.empty() && token == L"--") continue;
            o.remoteArgs.push_back(token);
        }
    } else if (!o.command.empty()) {
        while (p.i < argv.size()) {
            const std::wstring token = argv[p.i];
            if (token == L"--") {
                ++p.i;
                continue;
            }
            if (token.size() > 1 && token.front() == L'-') {
                bool recognized = false;
                if (!p.ApplyOption(token, recognized)) return p.result;
                if (!recognized) {
                    p.UnknownOption(token);
                    return p.result;
                }
                ++p.i;
                continue;
            }
            o.targets.push_back(token);
            ++p.i;
        }
    }

    if (o.command == L"hosts") {
        if (o.targets.empty()) {
            o.hostsAction = L"list";
        } else {
            o.hostsAction = CanonicalHostsAction(o.targets.front());
            if (o.hostsAction.empty()) {
                p.Fail(L"unknown hosts action '" + o.targets.front() + L"'",
                       L"Try 'srvctl help hosts'.");
                return p.result;
            }
            o.targets.erase(o.targets.begin());
        }
    }

    if (o.help || o.version) return p.result;

    if (o.command.empty()) return p.result;

    if (o.watch && o.command != L"status") {
        p.Fail(L"--watch is only valid for 'status'", L"Try 'srvctl help status'.");
        return p.result;
    }
    if (o.tty && o.command != L"exec" && o.command != L"ssh") {
        p.Fail(L"--tty is only valid for 'exec' and 'ssh'", L"Try 'srvctl help'.");
        return p.result;
    }
    if (o.json && o.command != L"status" && o.command != L"hosts") {
        p.Fail(L"--json is not supported by '" + o.command + L"'",
               o.command == L"doctor" ? L"Use 'srvctl status --json' for machine-readable probes."
                                      : L"Try 'srvctl help'.");
        return p.result;
    }

    if (o.command == L"exec" || o.command == L"ssh") {
        if (o.targets.empty()) {
            // 'ssh' with no host opens the configured-host picker at runtime.
            if (o.command == L"exec") {
                p.Fail(L"'exec' needs a host", L"Example: srvctl exec web-01 -- uname -a");
                return p.result;
            }
        } else {
            const std::wstring& host = o.targets.front();
            std::wstring why;
            if (!ValidTarget(host, &why) || host.find_first_of(L",; \t") != std::wstring::npos) {
                p.Fail(L"invalid host '" + host + L"'",
                       why.empty() ? L"Pass one SSH target per command." : why);
                return p.result;
            }
        }
        if (o.command == L"exec" && o.remoteArgs.empty()) {
            p.Fail(L"'exec' needs a command to run",
                   L"Example: srvctl exec web-01 -- uname -a");
            return p.result;
        }
    }

    if (o.command == L"hosts") {
        const bool needsTargets = o.hostsAction == L"add" || o.hostsAction == L"set" ||
                                  o.hostsAction == L"remove";
        if (needsTargets && o.targets.empty()) {
            p.Fail(L"'hosts " + o.hostsAction + L"' needs at least one target",
                   L"Example: srvctl hosts add web-01 db-01");
            return p.result;
        }
        if (!needsTargets && !o.targets.empty()) {
            p.Fail(L"'hosts " + o.hostsAction + L"' does not take targets",
                   L"Try 'srvctl help hosts'.");
            return p.result;
        }
    }

    return p.result;
}

} // namespace srv
