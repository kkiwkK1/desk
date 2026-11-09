#include "config.h"

#include "cli.h"
#include "targets.h"
#include "widget/store.h"

#include <windows.h>

#include <cwchar>
#include <cwctype>
#include <iterator>

namespace srv {
namespace {

std::wstring Trim(const std::wstring& s) {
    std::size_t begin = 0, end = s.size();
    while (begin < end && std::iswspace(s[begin])) ++begin;
    while (end > begin && std::iswspace(s[end - 1])) --end;
    return s.substr(begin, end - begin);
}

std::wstring Unquote(const std::wstring& s) {
    if (s.size() >= 2 && ((s.front() == L'"' && s.back() == L'"') ||
                          (s.front() == L'\'' && s.back() == L'\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

bool FileExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring FullPath(const std::wstring& path) {
    wchar_t buf[32768];
    const DWORD n = GetFullPathNameW(path.c_str(), (DWORD)std::size(buf), buf, nullptr);
    if (n == 0 || n >= std::size(buf)) return path;
    return std::wstring(buf, n);
}

std::wstring Env(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(needed, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, value.data(), needed);
    if (got == 0 || got >= needed) return {};
    value.resize(got);
    return value;
}

} // namespace

std::wstring ExeDir() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    if (n == 0) return L".";
    buf.resize(n);
    const std::size_t slash = buf.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : buf.substr(0, slash);
}

std::wstring DefaultConfigPath() {
    return ExeDir() + L"\\srvctl.ini";
}

bool Config::Load(const std::wstring& explicitPath, bool allowMissing, std::wstring& error) {
    *this = Config{};

    std::wstring requested = Trim(explicitPath);
    if (requested.empty()) requested = Trim(Env(L"SRVCTL_CONFIG"));
    const bool explicitGiven = !requested.empty();

    std::wstring readPath;
    if (explicitGiven) {
        readPath = FullPath(requested);
        if (!FileExists(readPath)) {
            const DWORD attrs = GetFileAttributesW(readPath.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                error = L"config path is a directory: " + readPath;
                return false;
            }
            if (!allowMissing) {
                error = L"config not found: " + readPath;
                return false;
            }
        }
    } else {
        const std::wstring dir = ExeDir();
        const std::wstring primary = dir + L"\\srvctl.ini";
        const std::wstring legacy = dir + L"\\server.ini";
        if (FileExists(primary)) {
            readPath = primary;
        } else if (FileExists(legacy)) {
            readPath = legacy;
            inherited = true;
        } else {
            readPath = primary;
        }
    }

    wg::Store store;
    const bool read = store.Open(readPath);
    exists = FileExists(readPath);
    sourcePath = readPath;
    path = inherited ? (ExeDir() + L"\\srvctl.ini") : readPath;

    if (!read && !exists && !allowMissing && explicitGiven) {
        error = L"config not found: " + readPath;
        return false;
    }

    for (const auto& host : ParseTargets(store.Get(L"hosts"), kMaxTargets)) {
        hosts.push_back(host);
    }
    ssh = Unquote(Trim(store.Get(L"ssh")));
    key = Unquote(Trim(store.Get(L"key")));

    const long long timeout = store.GetInt(L"timeout", 8);
    if (timeout < 1 || timeout > 3600) {
        error = L"invalid timeout in " + readPath + L" (expected 1..3600 seconds)";
        return false;
    }
    timeoutSec = (int)timeout;
    return true;
}

bool Config::Save(std::wstring& error) {
    const std::wstring target = inherited ? (ExeDir() + L"\\srvctl.ini") : path;
    if (target.empty()) {
        error = L"no config path";
        return false;
    }

    // Open the target first: when srvctl.ini already exists we keep unrelated
    // keys instead of replacing the file with only the four managed values.
    wg::Store store;
    store.Open(target);
    store.Set(L"hosts", JoinTargets(hosts));
    store.SetInt(L"timeout", timeoutSec);
    if (ssh.empty()) store.Remove(L"ssh");
    else store.Set(L"ssh", ssh);
    if (key.empty()) store.Remove(L"key");
    else store.Set(L"key", key);

    if (!store.Save()) {
        error = L"cannot write config: " + target;
        return false;
    }

    path = target;
    sourcePath = target;
    inherited = false;
    exists = true;
    return true;
}

} // namespace srv
