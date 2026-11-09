#include "data/ps/shell.h"

#include <windows.h>
#include <winver.h>

#include <algorithm>
#include <cwchar>
#include <string>

namespace ps {
namespace {

bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring OnPath(const wchar_t* exe) {
    wchar_t found[MAX_PATH * 2];
    const DWORD n = SearchPathW(nullptr, exe, L".exe", MAX_PATH * 2, found, nullptr);
    if (n == 0 || n >= MAX_PATH * 2) return std::wstring();
    return found;
}

std::wstring Env(const wchar_t* name) {
    wchar_t value[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(name, value, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return std::wstring();
    return value;
}

std::wstring Join(const std::wstring& dir, const wchar_t* rest) {
    if (dir.empty()) return std::wstring();
    std::wstring out = dir;
    if (out.back() != L'\\') out.push_back(L'\\');
    out += rest;
    return out;
}

// PowerShell 5.1's fixed install path (System32, not Program Files).
std::wstring WindowsPowerShell() {
    const std::wstring root = Env(L"SystemRoot");
    if (root.empty()) return std::wstring();
    return Join(root, L"System32\\WindowsPowerShell\\v1.0\\powershell.exe");
}

std::wstring VersionText(const std::wstring& path, const wchar_t* prefix) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return prefix;

    std::wstring buffer(size / sizeof(wchar_t) + 2, L'\0');
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buffer.data())) return prefix;

    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(buffer.data(), L"\\", (void**)&info, &length) || !info) return prefix;
    if (length < sizeof(VS_FIXEDFILEINFO)) return prefix;

    wchar_t text[64];
    // %ls: MinGW's wide swprintf treats %s as narrow, which would print only
    // the first byte of the prefix ("P 7.6").
    swprintf(text, 64, L"%ls %u.%u", prefix, (unsigned)HIWORD(info->dwFileVersionMS),
             (unsigned)LOWORD(info->dwFileVersionMS));
    return text;
}

} // namespace

std::wstring ResolveShell(const std::wstring& overridePath) {
    if (FileExists(overridePath)) return overridePath;

    const std::wstring fromEnv = Env(L"PWSH_EXE");
    if (FileExists(fromEnv)) return fromEnv;

    if (const std::wstring onPath = OnPath(L"pwsh"); !onPath.empty()) return onPath;

    if (const std::wstring programFiles = Env(L"ProgramFiles"); !programFiles.empty()) {
        const std::wstring stable = Join(programFiles, L"PowerShell\\7\\pwsh.exe");
        if (FileExists(stable)) return stable;
    }

    // This machine keeps pwsh as a portable install on another drive; other
    // machines should set shell= in ps.ini instead of relying on this probe.
    if (FileExists(L"L:\\powershell\\pwsh.exe")) return L"L:\\powershell\\pwsh.exe";

    if (const std::wstring onPath = OnPath(L"powershell"); !onPath.empty()) return onPath;

    const std::wstring legacy = WindowsPowerShell();
    if (FileExists(legacy)) return legacy;
    return std::wstring();
}

std::wstring ShellLabel(const std::wstring& path) {
    const wchar_t* base = wcsrchr(path.c_str(), L'\\');
    base = base ? base + 1 : path.c_str();

    if (_wcsicmp(base, L"pwsh.exe") == 0) return VersionText(path, L"PWSH");
    if (_wcsicmp(base, L"powershell.exe") == 0) return VersionText(path, L"PS");
    return L"SHELL";
}

} // namespace ps
