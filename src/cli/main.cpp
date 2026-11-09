// srvctl entry point: parse, load config, dispatch.
#include "cli.h"
#include "commands.h"
#include "config.h"
#include "help.h"
#include "output.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <exception>
#include <string>
#include <vector>

namespace {

std::wstring Lower(std::wstring text) {
    for (wchar_t& c : text) c = (wchar_t)std::towlower(c);
    return text;
}

std::wstring ParentProcessName() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return {};

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentProcessId();
    DWORD parentId = 0;

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == self) {
                parentId = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    std::wstring name;
    if (parentId != 0) {
        entry = PROCESSENTRY32W{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ProcessID == parentId) {
                    name = entry.szExeFile;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
    }
    CloseHandle(snapshot);
    return name;
}

// A console exe started by Explorer (or a bare Windows Terminal profile) gets
// a fresh console and would close as soon as main returns. Being the only
// process in that console is one signal; the launcher's name is the other,
// and it still works when a console host adds helper processes.
bool LaunchedWithoutShell() {
    const std::wstring parent = Lower(ParentProcessName());

    // Shells that keep their own console/window alive after srvctl exits.
    const bool shell = parent == L"cmd.exe" || parent == L"powershell.exe" ||
                       parent == L"pwsh.exe" || parent == L"bash.exe" ||
                       parent == L"wsl.exe" || parent == L"zsh.exe" ||
                       parent == L"fish.exe" || parent == L"nu.exe" ||
                       parent == L"sh.exe";
    if (shell) return false;

    // Explorer, the Windows Terminal host, or any other GUI launcher (Start
    // menu, pinned shortcut, file manager) is a one-shot launch.
    if (!parent.empty()) return true;

    // Fallback when the parent cannot be queried: a fresh console with just us
    // in it is a double-click, not a shell.
    DWORD pids[4]{};
    if (GetConsoleProcessList(pids, 4) != 1) return false;

    DWORD mode = 0;
    if (!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode)) return false;
    if (!GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)) return false;
    return true;
}

void WaitForEnter() {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    FlushConsoleInputBuffer(input);
    wchar_t buf[2]{};
    DWORD read = 0;
    ReadConsoleW(input, buf, 1, &read, nullptr);
}

void PauseBeforeClose(srv::Output& out) {
    out.Line();
    out.Line(L"Press Enter to close...");
    WaitForEnter();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::wstring> args;
    args.reserve((std::size_t)(argc > 0 ? argc : 0));
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);

    const srv::ParseResult parsed = srv::ParseArgs(args);
    if (!parsed.ok) {
        srv::Output err(srv::ColorMode::Never);
        err.Err(L"srvctl: " + parsed.error);
        if (!parsed.hint.empty()) err.Err(parsed.hint);
        return srv::kExitUsage;
    }

    srv::Options options = parsed.options;
    srv::Output out(options.color);

    if (options.version || options.command == L"version") {
        out.Line(std::wstring(L"srvctl ") + srv::kVersion);
        return srv::kExitOk;
    }

    // A bare double-click means "connect me": one configured host goes straight
    // into ssh, several hosts show the picker. The pause below keeps the console
    // open after the remote shell exits.
    const bool launchedAlone = LaunchedWithoutShell();
    bool quickConnect = false;
    if (options.command.empty() && !options.help && launchedAlone) {
        options.command = L"ssh";
        quickConnect = true;
    }

    if (quickConnect) SetConsoleTitleW(L"srvctl - SSH");

    if (options.help || options.command.empty() || options.command == L"help") {
        if (options.command == L"help" && !options.targets.empty()) {
            const std::wstring topic = srv::CanonicalCommand(options.targets.front());
            srv::PrintCommandHelp(topic.empty() ? options.targets.front() : topic, out);
        } else {
            srv::PrintGeneralHelp(out);
        }

        if (launchedAlone) {
            out.Line();
            out.Line(L"This is a command-line tool. Run it from Command Prompt or PowerShell:");
            out.Line(L"  srvctl ssh web-01");
            out.Line(L"  srvctl status");
            PauseBeforeClose(out);
        }
        return srv::kExitOk;
    }

    srv::Config config;
    bool allowMissing = false;
    if (options.command == L"hosts") {
        allowMissing = options.hostsAction == L"add" || options.hostsAction == L"set" ||
                       options.hostsAction == L"clear";
    }

    std::wstring error;
    if (!config.Load(options.configPath, allowMissing, error)) {
        out.Err(L"srvctl: " + error);
        if (quickConnect) PauseBeforeClose(out);
        return srv::kExitConfig;
    }
    if (!options.sshPath.empty()) config.ssh = options.sshPath;
    if (!options.keyPath.empty()) config.key = options.keyPath;

    int code = srv::kExitOk;
    try {
        if (options.command == L"status") code = srv::CmdStatus(options, config, out);
        else if (options.command == L"exec") code = srv::CmdExec(options, config, out);
        else if (options.command == L"ssh") code = srv::CmdSsh(options, config, out);
        else if (options.command == L"hosts") code = srv::CmdHosts(options, config, out);
        else if (options.command == L"doctor") code = srv::CmdDoctor(options, config, out);
        else {
            out.Err(L"srvctl: unhandled command");
            code = srv::kExitUsage;
        }
    } catch (const std::exception& e) {
        out.Err(std::wstring(L"srvctl: unexpected error: ") + srv::Widen(e.what()));
        code = srv::kExitFailure;
    } catch (...) {
        out.Err(L"srvctl: unexpected error");
        code = srv::kExitFailure;
    }

    if (quickConnect && out.console()) PauseBeforeClose(out);
    return code;
}
