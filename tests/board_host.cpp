// Programmatic host checks: BoardHost lifecycle, metrics, layout preview,
// BoardEvent stream, body-size notifications, display re-adaptation and
// restore through an injected ScriptedDisplaySource.
#include "widget/appenv.h"
#include "widget/board.h"
#include "widget/diag.h"
#include "widget/display.h"
#include "widget/host.h"
#include "widget/module.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool ok, const wchar_t* what) {
    if (!ok) {
        std::wprintf(L"[host-test] FAIL %ls\n", what);
        ++failures;
    }
}

// Keeps a trailing separator so fixtures can append names directly.
std::wstring ExeDir() {
    wchar_t exe[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe, length);
    const std::size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(0, slash + 1);
}

// wg::appenv reports the exe directory without a trailing separator (see the
// ExeDir()/ConfigDir() contract in widget/appenv.h), so compare spellings through
// this normaliser instead of assuming both sides end the same way.
std::wstring WithoutTrailingSeparator(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

bool WriteText(const std::wstring& path, const std::string& text) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok =
        WriteFile(handle, text.data(), (DWORD)text.size(), &written, nullptr);
    CloseHandle(handle);
    return ok != FALSE;
}

bool PumpUntil(const std::function<bool()>& done, int timeoutMs) {
    for (int waited = 0; waited <= timeoutMs; waited += 10) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (done()) return true;
        Sleep(10);
    }
    return done();
}

std::vector<unsigned char> ReadBytes(const std::wstring& path) {
    std::vector<unsigned char> bytes;
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return bytes;
    unsigned char buffer[4096];
    DWORD read = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        bytes.insert(bytes.end(), buffer, buffer + read);
    }
    CloseHandle(handle);
    return bytes;
}

bool LooksLikeCanvasBmp(const std::wstring& path) {
    const std::vector<unsigned char> bytes = ReadBytes(path);
    if (bytes.size() < 54 || bytes[0] != 'B' || bytes[1] != 'M') return false;
    int width = 0;
    int height = 0;
    unsigned short bits = 0;
    std::memcpy(&width, bytes.data() + 18, sizeof(width));
    std::memcpy(&height, bytes.data() + 22, sizeof(height));
    std::memcpy(&bits, bytes.data() + 28, sizeof(bits));
    return width > 0 && height != 0 && bits == 32;
}

std::wstring ReadIni(const std::wstring& path) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::wstring();
    std::wstring text;
    wchar_t buffer[4096];
    DWORD read = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        text.append(buffer, read / sizeof(wchar_t));
    }
    CloseHandle(handle);
    return text;
}

bool Contains(const std::wstring& text, const wchar_t* needle) {
    return text.find(needle) != std::wstring::npos;
}

// The board saves on a background thread; give it a moment to flush.
bool WaitContains(const std::wstring& path, const wchar_t* needle, bool expected,
                  int timeoutMs) {
    for (int waited = 0; waited <= timeoutMs; waited += 20) {
        if (Contains(ReadIni(path), needle) == expected) return true;
        Sleep(20);
    }
    return false;
}

class TestModule : public wg::Module {
public:
    explicit TestModule(bool visibleByDefault = true) : visibleByDefault_(visibleByDefault) {}

    wg::ModuleSpec Describe() const override {
        wg::ModuleSpec spec;
        spec.id = visibleByDefault_ ? L"hosttest" : L"hosttest2";
        spec.title = visibleByDefault_ ? L"HOSTTEST" : L"HOSTTEST2";
        spec.width = 340;
        spec.sizePreset = wg::SizePreset::Standard;
        spec.hasDetails = true;
        spec.visibleByDefault = visibleByDefault_;
        return spec;
    }
    void Load() override {}
    void Refresh() override { ++refreshCalls; }
    void Commit() override { ++commitCalls; }
    int BodyHeight() const override { return expanded() ? 200 : 96; }
    void PaintBody(wg::Canvas&, const wg::Rect&) override {}
    void OnBodySizeChanged(const wg::Rect& body) override {
        lastBody = body;
        ++bodyCalls;
    }

    wg::Rect lastBody{};
    int bodyCalls = 0;
    std::atomic<int> refreshCalls{0};
    std::atomic<int> commitCalls{0};

private:
    bool visibleByDefault_ = true;
};

} // namespace

int wmain() {
    using namespace wg;

    const std::wstring dir = ExeDir();
    const std::wstring testConfigDir = dir + L"host-test-config";
    const std::wstring config = testConfigDir + L"\\host-test.ini";
    const std::wstring displayFile = dir + L"host-test-display.txt";
    const std::wstring frame0 = testConfigDir + L"\\assistant-test-frame-0.bmp";
    const std::wstring frame1 = testConfigDir + L"\\assistant-test-frame-1.bmp";
    const std::wstring frame2 = testConfigDir + L"\\assistant-test-frame-2.bmp";
    CreateDirectoryW(testConfigDir.c_str(), nullptr);
    DeleteFileW(config.c_str());
    DeleteFileW(displayFile.c_str());
    for (int slot = 0; slot < 3; ++slot) {
        DeleteFileW((testConfigDir + L"\\assistant-test-frame-" +
                     std::to_wstring(slot) + L".bmp").c_str());
    }
    WriteText(displayFile, "work=0,0,2560,1392\r\ndpi=96\r\n");

    // Test-instance configuration isolation: a relative override is rejected
    // (fail-closed, no real APPDATA fallback), a valid absolute one is used.
    SetEnvironmentVariableW(L"DESK_TEST_INSTANCE", L"hosttest");
    SetEnvironmentVariableW(L"DESK_TEST_CONFIG_DIR", L"relative\\host-test-config");
    std::wstring configError;
    const std::wstring rejected = wg::appenv::ConfigDir(L"Board Host Test", &configError);
    Check(rejected.empty() && !configError.empty(),
          L"relative DESK_TEST_CONFIG_DIR fails closed with a diagnostic");
    SetEnvironmentVariableW(L"DESK_TEST_CONFIG_DIR", nullptr);
    configError.clear();
    const std::wstring legacy = wg::appenv::ConfigDir(L"Board Host Test", &configError);
    const std::wstring legacyExpected = WithoutTrailingSeparator(dir);
    if (legacy != legacyExpected || !configError.empty()) {
        std::wprintf(
            L"[host-test]   legacy exe dir: expected <%ls> got <%ls> error <%ls>\n",
            legacyExpected.c_str(), legacy.c_str(), configError.c_str());
    }
    Check(legacy == legacyExpected && configError.empty(),
          L"test instance without an override keeps the writable exe directory");
    const std::wstring missingDir = testConfigDir + L"\\missing";
    SetEnvironmentVariableW(L"DESK_TEST_CONFIG_DIR", missingDir.c_str());
    configError.clear();
    Check(wg::appenv::ConfigDir(L"Board Host Test", &configError).empty() &&
              !configError.empty(),
          L"missing DESK_TEST_CONFIG_DIR fails closed with a diagnostic");
    SetEnvironmentVariableW(L"DESK_TEST_CONFIG_DIR", testConfigDir.c_str());
    configError.clear();
    const std::wstring accepted = wg::appenv::ConfigDir(L"Board Host Test", &configError);
    Check(!accepted.empty() && configError.empty(),
          L"absolute DESK_TEST_CONFIG_DIR is accepted");

    auto display = ScriptedDisplaySource::Create(displayFile);

    std::vector<BoardEventKind> events;
    auto record = [&events](const BoardEvent& event) { events.push_back(event.kind); };
    auto has = [&events](BoardEventKind kind) {
        for (BoardEventKind item : events) {
            if (item == kind) return true;
        }
        return false;
    };

    {
    TestModule* first = new TestModule(true);
    TestModule* second = new TestModule(false);
    std::vector<std::unique_ptr<Module>> modules;
    modules.emplace_back(first);
    modules.emplace_back(second);

    BoardOptions options;
    options.id = L"wg-host-test";
    options.title = L"Board Host Test";
    options.configFile = L"host-test.ini";
    options.display = display;
    options.enableTray = false;
    options.enableHotkeys = false;
    options.onEvent = record;

    BoardHost host(std::move(modules), options);
    Check(host.Start() == BoardHost::StartResult::Started, L"host starts");
    Check(host.running(), L"host reports running");
    Check(host.hwnd() != nullptr, L"host has a window");

    const BoardMetrics metrics = host.Metrics();
    Check(metrics.board.w == 1280 && metrics.board.h == 1310 &&
              metrics.board.x == 1280 && metrics.board.y == 0,
          L"first-run board is 1280x1310 at (1280,0)");
    Check(metrics.interior.w == 1252 && metrics.interior.h == 1248,
          L"interior is 1252x1248");
    Check(metrics.work.right - metrics.work.left == 2560, L"scripted work area is used");
    Check(metrics.dpi == 96, L"scripted DPI is used");
    Check(has(BoardEventKind::LayoutApplied), L"LayoutApplied event was delivered");

    // RequestRefresh must go through the board's UI queue, not start a worker
    // directly from the caller's thread.
    first->RequestRefresh();
    Check(PumpUntil([&] {
              return first->refreshCalls.load() > 0 && first->commitCalls.load() > 0;
          }, 3000),
          L"Module::RequestRefresh runs a refresh and commit");

    // Body notification: the visible card gets its (340 x 96) body rectangle.
    Check(first->bodyCalls > 0, L"OnBodySizeChanged was called for the visible card");
    Check(first->lastBody.w == 340 && first->lastBody.h == 96, L"body rect matches preset");
    Check(second->bodyCalls == 0, L"hidden card gets no body notification");

    // Layout preview does not change state.
    std::vector<Rect> preview;
    Check(host.PlanPreview(Rect{0, 0, 1280, 1310}, &preview) && preview.size() == 1 &&
              preview[0].w == 340,
          L"PlanPreview plans visible cards");

    // Expand / collapse through the host API.
    const int callsBeforeExpand = first->bodyCalls;
    Check(host.SetCardExpanded(0, true), L"SetCardExpanded(true) succeeds");
    Check(first->bodyCalls > callsBeforeExpand && first->lastBody.h > 96,
          L"expanding changes the body rect");
    Check(host.SetCardExpanded(0, false), L"SetCardExpanded(false) succeeds");

    // Show/hide a card.
    events.clear();
    Check(host.ShowCard(1, true), L"ShowCard(true) succeeds");
    Check(has(BoardEventKind::CardShown), L"CardShown event was delivered");
    Check(second->bodyCalls > 0, L"newly visible card gets a body notification");
    Check(host.ShowCard(1, false), L"ShowCard(false) succeeds");
    Check(has(BoardEventKind::CardHidden), L"CardHidden event was delivered");

    // A second host with the same id must not start.
    {
        BoardOptions duplicate = options;
        duplicate.enableTray = false;
        duplicate.enableHotkeys = false;
        std::vector<std::unique_ptr<Module>> more;
        more.emplace_back(new TestModule(true));
        BoardHost other(std::move(more), duplicate);
        Check(other.Start() == BoardHost::StartResult::AlreadyRunning,
              L"second host with the same id reports AlreadyRunning");
        Check(other.hwnd() == nullptr, L"second host creates no window");
    }

    // Display change: shrink the simulated work area, then restore it.
    events.clear();
    WriteText(displayFile, "work=0,0,2032,972\r\ndpi=96\r\n");
    host.ReadaptToDisplay();
    const BoardMetrics adapted = host.Metrics();
    Check(adapted.board.w <= 2032 && adapted.board.h <= 972, L"board adapts to the new work area");
    Check(adapted.board.x + adapted.board.w <= 2032, L"adapted board stays inside the work area");
    Check(has(BoardEventKind::LayoutRestorePending), L"adaptation snapshots the original layout");
    Check(has(BoardEventKind::DisplayAdapted), L"DisplayAdapted event was delivered");
    Check(WaitContains(config, L"saved.pending=1", true, 2000),
          L"saved.pending is persisted");

    WriteText(displayFile, "work=0,0,2560,1392\r\ndpi=96\r\n");
    events.clear();
    host.ReadaptToDisplay();
    const BoardMetrics restored = host.Metrics();
    Check(has(BoardEventKind::LayoutRestored), L"LayoutRestored event was delivered");
    Check(restored.board.w == 1280 && restored.board.h == 1310 && restored.board.x == 1280,
          L"restore brings the original board back");
    Check(WaitContains(config, L"saved.pending=1", false, 2000),
          L"saved.* was cleared by the restore");

    // A suggested DPI rectangle goes through the same snapshot/restore flow.
    events.clear();
    host.ApplySuggestedRect(RECT{0, 0, 1920, 1310});
    Check(host.Metrics().board.w == 1920, L"ApplySuggestedRect applies the suggested rectangle");
    Check(has(BoardEventKind::LayoutRestorePending), L"suggested rect snapshots the old layout");
    events.clear();
    host.ApplySuggestedRect(RECT{1280, 0, 1280, 1310});
    Check(has(BoardEventKind::LayoutRestored), L"returning to the original rect restores the layout");

    // Own-canvas export: test-instance only, fixed slots, no caller path.
    DeleteFileW(frame0.c_str());
    DeleteFileW(frame1.c_str());
    DeleteFileW(frame2.c_str());
    Check(SendMessageW(host.hwnd(), WM_APP + 11, 0, 0) == 1 && LooksLikeCanvasBmp(frame0),
          L"own-canvas frame 0 is exported as a 32-bit BMP");
    Check(host.ShowCard(1, true), L"second card shown before frame 1");
    Check(SendMessageW(host.hwnd(), WM_APP + 11, 1, 0) == 1 && LooksLikeCanvasBmp(frame1),
          L"own-canvas frame 1 is exported as a 32-bit BMP");
    Check(ReadBytes(frame0) != ReadBytes(frame1),
          L"the two own-canvas frames differ after a visible change");
    Check(SendMessageW(host.hwnd(), WM_APP + 11, 2, 0) == 0,
          L"own-canvas export rejects a slot outside 0..1");
    Check(!LooksLikeCanvasBmp(frame2), L"no arbitrary canvas path was written");
    Check(host.ShowCard(1, false), L"second card hidden after the frame test");

    // Invalid/indexed operations are rejected without crashing.
    Check(!host.ShowCard(99, true), L"invalid ShowCard index is rejected");
    Check(!host.SetCardExpanded(99, true), L"invalid SetCardExpanded index is rejected");

    host.Quit();
    Check(!host.running() || host.hwnd() != nullptr, L"Quit keeps the host object usable");
    }

    SetEnvironmentVariableW(L"DESK_TEST_CONFIG_DIR", nullptr);
    SetEnvironmentVariableW(L"DESK_TEST_INSTANCE", nullptr);
    DeleteFileW(frame0.c_str());
    DeleteFileW(frame1.c_str());
    DeleteFileW(config.c_str());
    DeleteFileW(displayFile.c_str());
    RemoveDirectoryW(testConfigDir.c_str());

    std::wprintf(L"[host-test] %s\n", failures ? L"failed" : L"ok");
    return failures == 0 ? 0 : 1;
}
