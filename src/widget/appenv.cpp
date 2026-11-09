#include "widget/appenv.h"

#include <windows.h>

#include <string>
#include <vector>

namespace wg {
namespace appenv {
namespace {

constexpr const wchar_t* kRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

std::wstring Env(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return std::wstring();
    std::vector<wchar_t> buffer(size, L'\0');
    const DWORD n = GetEnvironmentVariableW(name, buffer.data(), size);
    return n > 0 && n < size ? std::wstring(buffer.data(), n) : std::wstring();
}

bool WritableDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    const std::wstring probe = dir + L"\\.wg-write-test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// Absolute = drive-rooted (X:\...) or UNC (\\server\share). A relative path
// is rejected rather than resolved against the current directory, because the
// test harness must never be able to redirect configuration implicitly.
bool IsAbsolutePath(const std::wstring& path) {
    if (path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') ||
                             (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        return true;
    }
    return path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
}

std::wstring CanonicalPath(const std::wstring& path) {
    const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return std::wstring();
    std::vector<wchar_t> buffer(needed, L'\0');
    const DWORD n = GetFullPathNameW(path.c_str(), needed, buffer.data(), nullptr);
    if (n == 0 || n >= needed) return std::wstring();
    std::wstring result(buffer.data(), n);
    while (result.size() > 3 &&
           (result.back() == L'\\' || result.back() == L'/')) {
        result.pop_back();
    }
    return result;
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ValidTestSuffix(const std::wstring& value) {
    if (value.empty()) return false;
    for (const wchar_t c : value) {
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                        (c >= L'0' && c <= L'9') || c == L'-' || c == L'_';
        if (!ok) return false;
    }
    return true;
}

} // namespace

bool TestInstanceSuffix(std::wstring* suffix) {
    const std::wstring value = Env(L"DESK_TEST_INSTANCE");
    if (!ValidTestSuffix(value)) return false;
    if (suffix) *suffix = value;
    return true;
}

bool TestConfigDirOverride() {
    std::wstring suffix;
    return TestInstanceSuffix(&suffix) && !Env(L"DESK_TEST_CONFIG_DIR").empty();
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(0, slash);
}

std::wstring ConfigDir(const std::wstring& appTitle, std::wstring* error) {
    if (error) error->clear();

    const std::wstring local = ExeDir();
    std::wstring suffix;
    const bool testInstance = TestInstanceSuffix(&suffix);

    if (testInstance) {
        const std::wstring overrideDir = Env(L"DESK_TEST_CONFIG_DIR");
        if (!overrideDir.empty()) {
            if (!IsAbsolutePath(overrideDir)) {
                if (error) *error = L"DESK_TEST_CONFIG_DIR must be an absolute path";
                return std::wstring();
            }
            const std::wstring full = CanonicalPath(overrideDir);
            if (full.empty()) {
                if (error) *error = L"DESK_TEST_CONFIG_DIR cannot be normalised";
                return std::wstring();
            }
            if (!DirectoryExists(full)) {
                if (error) *error = L"DESK_TEST_CONFIG_DIR does not exist or is not a directory";
                return std::wstring();
            }
            if (!WritableDir(full)) {
                if (error) *error = L"DESK_TEST_CONFIG_DIR is not writable";
                return std::wstring();
            }
            return full;
        }

        // Legacy isolated copy-exe runs: the exe directory is their config
        // directory. A non-writable one must fail closed, never touch the real
        // %APPDATA% of the user running the test.
        if (WritableDir(local)) return local;
        if (error) {
            *error = L"test instance has no writable exe directory and "
                     L"DESK_TEST_CONFIG_DIR is not set (refusing %APPDATA%)";
        }
        return std::wstring();
    }

    if (WritableDir(local)) return local;

    const std::wstring appData = Env(L"APPDATA");
    if (appData.empty()) {
        if (error) *error = L"no writable exe directory and APPDATA is not set";
        return local;
    }

    const std::wstring dir = appData + L"\\" + appTitle;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ConfigPath(const std::wstring& fileName, const std::wstring& appTitle,
                        std::wstring* error) {
    const std::wstring dir = ConfigDir(appTitle, error);
    if (dir.empty()) return std::wstring();
    return dir + L"\\" + fileName;
}

bool AutostartEnabled(const std::wstring& name) {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    const bool on = RegQueryValueExW(key, name.c_str(), nullptr, nullptr, nullptr, nullptr) ==
                    ERROR_SUCCESS;
    RegCloseKey(key);
    return on;
}

std::wstring AutostartCommand(const std::wstring& name) {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return std::wstring();

    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return std::wstring();
    }

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name.c_str(), nullptr, &type, (LPBYTE)buffer.data(), &bytes) !=
        ERROR_SUCCESS) {
        RegCloseKey(key);
        return std::wstring();
    }
    RegCloseKey(key);
    return std::wstring(buffer.data());
}

void SetAutostart(const std::wstring& name, bool on) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return;

    if (on) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        const std::wstring value = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(key, name.c_str(), 0, REG_SZ, (const BYTE*)value.c_str(),
                       (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, name.c_str());
    }
    RegCloseKey(key);
}

void RemoveAutostart(const std::wstring& name) {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    RegDeleteValueW(key, name.c_str());
    RegCloseKey(key);
}

} // namespace appenv
} // namespace wg
