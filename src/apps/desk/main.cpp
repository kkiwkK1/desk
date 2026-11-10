// desk.exe - the desktop pegboard.
//
// To add a module: write src/panels/<name>/<name>.cpp with a wg::Module
// subclass and append one factory call below. The board creates the card,
// adds it to the Add Module menu and persists its position automatically.
#include "panels/account/account.h"
#include "panels/assistant/assistant.h"
#include "panels/cardspec/cardspec.h"
#include "panels/memory/memory.h"
#include "panels/ps/ps.h"
#include "panels/servers/servers.h"

#include "widget/appenv.h"
#include "widget/board.h"
#include "widget/store.h"

#include <atomic>
#include <cstdlib>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

namespace {

bool EqualsNoCase(const std::wstring& value, const wchar_t* expected) {
    const std::wstring lowered(expected);
    if (value.size() != lowered.size()) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (std::towlower(value[i]) != std::towlower(lowered[i])) return false;
    }
    return true;
}

bool TestFlag(const wchar_t* name) {
    wchar_t buffer[16]{};
    const DWORD n = GetEnvironmentVariableW(name, buffer,
                                            (DWORD)(sizeof(buffer) / sizeof(buffer[0])));
    if (n == 0 || n >= sizeof(buffer) / sizeof(buffer[0])) return false;
    const std::wstring value(buffer, n);
    return value == L"1" || EqualsNoCase(value, L"true") || EqualsNoCase(value, L"yes");
}

// Only used by the explicitly isolated CardSpec GUI fixture. Never log data/URLs.
bool WriteCardSpecTestCount(const std::wstring& path, unsigned count) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const std::string text = std::to_string(count) + "\n";
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr) &&
                    written == text.size();
    CloseHandle(file);
    return ok;
}

std::wstring Trim(const std::wstring& value) {
    std::size_t first = 0;
    while (first < value.size() && std::iswspace(value[first])) ++first;
    std::size_t last = value.size();
    while (last > first && std::iswspace(value[last - 1])) --last;
    return value.substr(first, last - first);
}

// "Ctrl+Alt+A" / "Ctrl+Shift+F10" / "Win+1". The hotkey field is user
// configuration, so invalid text falls back to Ctrl+Alt+A instead of silently
// registering a different key.
bool ParseHotkey(const std::wstring& text, UINT* modifiers, UINT* vk) {
    if (!modifiers || !vk) return false;
    UINT mods = 0;
    UINT key = 0;
    std::size_t start = 0;
    bool sawKey = false;
    while (start <= text.size()) {
        const std::size_t plus = text.find(L'+', start);
        const std::wstring token = Trim(
            text.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start));
        if (token.empty()) return false;
        if (sawKey) return false;  // a key must be the last component
        if (EqualsNoCase(token, L"ctrl") || EqualsNoCase(token, L"control")) {
            mods |= MOD_CONTROL;
        } else if (EqualsNoCase(token, L"alt")) {
            mods |= MOD_ALT;
        } else if (EqualsNoCase(token, L"shift")) {
            mods |= MOD_SHIFT;
        } else if (EqualsNoCase(token, L"win") || EqualsNoCase(token, L"windows")) {
            mods |= MOD_WIN;
        } else {
            if (token.size() == 1) {
                const wchar_t c = (wchar_t)std::towupper(token[0]);
                if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) {
                    key = (UINT)c;
                } else {
                    return false;
                }
            } else if (token[0] == L'F' || token[0] == L'f') {
                if (token.size() < 2 || token.size() > 3) return false;
                wchar_t* end = nullptr;
                const unsigned long number = std::wcstoul(token.c_str() + 1, &end, 10);
                if (!end || *end != L'\0' || number < 1 || number > 24) return false;
                key = (UINT)(VK_F1 + number - 1);
            } else {
                return false;
            }
            sawKey = true;
        }
        if (plus == std::wstring::npos) break;
        start = plus + 1;
    }
    if (!sawKey || mods == 0) return false;
    *modifiers = mods;
    *vk = key;
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wg::BoardOptions options;
    options.id = L"desk";
    options.title = L"Desktop Desk";
    options.configFile = L"desk.ini";
    options.boardTitle = L"DESKTOP BOARD";
    options.startWithWindows = true;
    options.legacyAutostartNames = {L"AiMeter", L"Server Status", L"SSH Pad"};

    // Test hook: a second instance that can run beside the real desk without
    // sharing its mutex, config or Windows startup entry. Set DESK_TEST_INSTANCE=qa1
    // and run a copy of desk.exe from its own directory. Tests never migrate or
    // create a real startup entry and do not add a tray icon.
    std::wstring suffix;
    const bool testInstance = wg::appenv::TestInstanceSuffix(&suffix);
    const bool cardspecOnly = TestFlag(L"DESK_TEST_CARDSPEC_ONLY");
    if (cardspecOnly && (!testInstance || !wg::appenv::TestConfigDirOverride())) {
        OutputDebugStringW(L"[desk] CardSpec GUI fixture requires isolated suffix/config.\n");
        return 2;
    }
    if (testInstance) {
        options.id = L"desk-" + suffix;
        options.startWithWindows = false;
        options.legacyAutostartNames.clear();
        options.enableTray = false;
        // Default test instances register no global hotkeys at all. The only
        // exception is the explicit physical-probe gate below.
        options.enableHotkeys = false;
    }

    // Resolve the exact directory the board will use, so the enabled flag and
    // hotkey below come from the same isolated config as the modules.
    std::wstring configError;
    const std::wstring configDir = wg::appenv::ConfigDir(options.title, &configError);
    if (configDir.empty()) {
        if (cardspecOnly) {
            OutputDebugStringW(L"[desk] CardSpec GUI fixture config is invalid.\n");
            return 2;
        }
        const std::wstring message =
            L"Desktop Desk cannot resolve its configuration directory.\n\n" +
            (configError.empty() ? L"Unknown configuration error." : configError);
        OutputDebugStringW((L"[desk] " + message + L"\n").c_str());
        MessageBoxW(nullptr, message.c_str(), L"Desktop Desk",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 1;
    }

    // An explicit, validated test instance registers only CardSpec. Its native
    // fetch seam always supplies fixed synthetic JSON: even an accidental
    // approved refresh cannot reach DNS/WinHTTP. Confirmation UI stays real.
    if (cardspecOnly) {
        const std::wstring countFile = configDir + L"\\cardspec-test-fetch-count.txt";
        if (!WriteCardSpecTestCount(countFile, 0)) return 2;
        auto calls = std::make_shared<std::atomic<unsigned>>(0);
        CardSpecOptions fixture;
        fixture.confirmNetwork = [](const std::wstring& message) {
            return MessageBoxW(nullptr, message.c_str(), L"CARDSPEC · 网络授权",
                               MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
        };
        fixture.fetch = [countFile, calls](std::string_view profile,
                                          const cardspec::HttpPolicy& policy) {
            cardspec::HttpResult result;
            const unsigned count = calls->fetch_add(1) + 1;
            if (!WriteCardSpecTestCount(countFile, count)) return result;
            cardspec::HttpEndpoint endpoint;
            cardspec::HttpFailure failure = cardspec::HttpFailure::NotApproved;
            if (!cardspec::AuthorizeHttp(profile, policy, &endpoint, &failure)) {
                result.failure = failure;
                return result;
            }
            result.failure = cardspec::HttpFailure::None;
            result.status = 200;
            result.body = R"({"items":[{"name":"HTTP_FIXTURE_ROW","text":"SYNTHETIC_RESPONSE","value":3,"max":5}]})";
            return result;
        };
        std::vector<std::unique_ptr<wg::Module>> modules;
        modules.push_back(CreateCardSpecModule(std::move(fixture)));
        return wg::RunBoard(std::move(modules), options);
    }

    wg::Store assistantConfig;
    assistantConfig.Open(configDir + L"\\assistant.ini");
    const bool assistantEnabled = assistantConfig.GetInt(L"enabled", 1) != 0;

    UINT assistantModifiers = MOD_CONTROL | MOD_ALT;
    UINT assistantVk = 'A';
    const std::wstring hotkeyText = assistantConfig.Get(L"hotkey", L"Ctrl+Alt+A");
    if (!ParseHotkey(hotkeyText, &assistantModifiers, &assistantVk)) {
        OutputDebugStringW(
            L"[desk] assistant.ini hotkey is invalid; using Ctrl+Alt+A\n");
        assistantModifiers = MOD_CONTROL | MOD_ALT;
        assistantVk = 'A';
    }
    options.assistantHotkeyModifiers = assistantModifiers;
    options.assistantHotkeyVk = assistantVk;

    // Physical hotkey probe gate: only an explicit env flag on a fully isolated
    // test instance (valid suffix + validated DESK_TEST_CONFIG_DIR) may register
    // one global chord. Force the independent Ctrl+Alt+F10 so the probe can
    // never take the real Ctrl+Alt+A or the board R/W/D/Q chords.
    if (testInstance && wg::appenv::TestConfigDirOverride() &&
        TestFlag(L"DESK_TEST_REGISTER_ASSISTANT_HOTKEY")) {
        options.enableHotkeys = true;
        options.assistantOnlyHotkeys = true;
        options.assistantHotkeyModifiers = MOD_CONTROL | MOD_ALT;
        options.assistantHotkeyVk = VK_F10;
    }

    // DESK_TEST_ASSISTANT_ONLY keeps a test instance from loading the Account,
    // Servers or PowerShell cards (which may read real user state or start a
    // shell). Outside tests every card is registered as before.
    const bool assistantOnly = testInstance && TestFlag(L"DESK_TEST_ASSISTANT_ONLY");

    std::vector<std::unique_ptr<wg::Module>> modules;
    if (!assistantOnly) {
        modules.push_back(CreateAccountModule());
        modules.push_back(CreateServersModule());
        modules.push_back(CreatePsModule());
        modules.push_back(CreateCardSpecModule());
    }
    if (assistantEnabled) {
        modules.push_back(CreateAssistantModule());
        modules.push_back(CreateMemoryModule());
    } else {
        OutputDebugStringW(
            L"[desk] assistant.ini enabled=0; assistant/memory are not registered\n");
    }

    return wg::RunBoard(std::move(modules), options);
}
