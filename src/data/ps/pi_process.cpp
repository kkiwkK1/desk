#include "data/ps/pi_process.h"

#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace ps {
namespace {

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit Handle(HANDLE h) : value(h) {}
};

std::wstring LowerPath(std::wstring_view text) {
    std::wstring result(text);
    for (wchar_t& c : result) {
        if (c == L'/') c = L'\\';
        else c = (wchar_t)std::towlower(c);
    }
    return result;
}

bool EndsWith(const std::wstring& text, std::wstring_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::wstring ImagePath(HANDLE process) {
    std::wstring path(32768, L'\0');
    DWORD size = (DWORD)path.size();
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) return {};
    path.resize(size);
    return path;
}

// ProcessCommandLineInformation is available on supported Windows 10/11.
// It returns a UNICODE_STRING and its text in our buffer; validate the range
// before copying it. No VM_READ privilege or external WMI process is required.
std::wstring CommandLine(HANDLE process) {
    struct UnicodeString { USHORT length; USHORT maximumLength; PWSTR buffer; };
    using QueryFn = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const FARPROC address = ntdll ? GetProcAddress(ntdll, "NtQueryInformationProcess") : nullptr;
    QueryFn query = nullptr;
    static_assert(sizeof(query) == sizeof(address));
    std::memcpy(&query, &address, sizeof(query));
    if (!query) return {};

    constexpr ULONG kProcessCommandLineInformation = 60;
    ULONG bytes = 0;
    query(process, kProcessCommandLineInformation, nullptr, 0, &bytes);
    if (bytes < sizeof(UnicodeString) || bytes > 1024 * 1024) return {};
    std::vector<unsigned char> data(bytes);
    if (query(process, kProcessCommandLineInformation, data.data(), bytes, &bytes) < 0)
        return {};
    const auto* value = reinterpret_cast<const UnicodeString*>(data.data());
    if (!value->buffer || !value->length || (value->length % sizeof(wchar_t)) != 0)
        return {};
    const auto begin = reinterpret_cast<std::uintptr_t>(data.data());
    const auto end = begin + data.size();
    const auto text = reinterpret_cast<std::uintptr_t>(value->buffer);
    if (text < begin || text > end || value->length > end - text) return {};
    return std::wstring(value->buffer, value->length / sizeof(wchar_t));
}

bool StartedAfter(HANDLE process, const FILETIME& rootCreated) {
    FILETIME created{}, exit{}, kernel{}, user{};
    return GetProcessTimes(process, &created, &exit, &kernel, &user) &&
           CompareFileTime(&created, &rootCreated) >= 0;
}

} // namespace

bool IsPiCliCommand(std::wstring_view imagePath, std::wstring_view commandLine) {
    const std::wstring image = LowerPath(imagePath);
    const auto slash = image.find_last_of(L'\\');
    if (image.substr(slash == std::wstring::npos ? 0 : slash + 1) != L"node.exe")
        return false;
    if (commandLine.empty() || commandLine.find(L'\0') != std::wstring_view::npos)
        return false;

    const std::wstring command(commandLine);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(command.c_str(), &argc);
    if (!argv) return false;
    bool matched = false;
    if (argc >= 2) {
        const std::wstring script = LowerPath(argv[1]);
        constexpr std::wstring_view kPackage =
            L"\\node_modules\\@earendil-works\\pi-coding-agent\\dist\\";
        matched = EndsWith(script, std::wstring(kPackage) + L"bundle\\cli.js") ||
                  EndsWith(script, std::wstring(kPackage) + L"cli.js");
        // Fail closed for explicit regular mode or an ambiguous mode switch.
        for (int i = 2; matched && i < argc; ++i) {
            const std::wstring arg = LowerPath(argv[i]);
            if (arg == L"--tui-mode") {
                if (++i >= argc || LowerPath(argv[i]) != L"fullscreen") matched = false;
            } else if (arg.compare(0, 11, L"--tui-mode=") == 0 &&
                       arg != L"--tui-mode=fullscreen") {
                matched = false;
            }
        }
    }
    LocalFree(argv);
    return matched;
}

bool HasPiCliDescendant(DWORD shellPid) {
    if (!shellPid) return false;
    Handle root(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shellPid));
    if (!root.value) return false;
    FILETIME rootCreated{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(root.value, &rootCreated, &exit, &kernel, &user)) return false;

    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry)) return false;

    struct Process { DWORD pid; DWORD parent; bool node; };
    std::vector<Process> processes;
    do {
        processes.push_back({entry.th32ProcessID, entry.th32ParentProcessID,
                             _wcsicmp(entry.szExeFile, L"node.exe") == 0});
    } while (Process32NextW(snapshot.value, &entry));

    std::vector<DWORD> descendants{shellPid};
    for (std::size_t index = 0; index < descendants.size(); ++index) {
        for (const auto& process : processes) {
            if (process.parent != descendants[index] || process.pid == shellPid ||
                std::find(descendants.begin(), descendants.end(), process.pid) != descendants.end())
                continue;
            descendants.push_back(process.pid);
            if (!process.node) continue;
            Handle child(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.pid));
            if (!child.value || !StartedAfter(child.value, rootCreated)) continue;
            const std::wstring image = ImagePath(child.value);
            if (image.empty()) continue;
            const std::wstring command = CommandLine(child.value);
            if (!command.empty() && IsPiCliCommand(image, command)) return true;
        }
    }
    return false;
}

} // namespace ps
