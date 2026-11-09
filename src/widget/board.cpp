#include "widget/board.h"

#include "widget/appenv.h"
#include "widget/board_grow.h"
#include "widget/board_layout.h"
#include "widget/board_plan.h"
#include "widget/diag.h"
#include "widget/display.h"
#include "widget/host.h"
#include "widget/skin.h"

#include <shellapi.h>
#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

namespace wg {
namespace {

constexpr UINT kMsgModuleDone = WM_APP + 1;
constexpr UINT kMsgTray = WM_APP + 2;
constexpr UINT kMsgDirty = WM_APP + 3;
// Module::RequestRefresh() posts this; only the UI thread starts the worker.
constexpr UINT kMsgModuleRefresh = WM_APP + 4;
// Compatibility shims for the display-simulation test hook. The real
// implementation is ScriptedDisplaySource (widget/display.h); these messages
// only exist for the existing GUI harness scripts.
constexpr UINT kMsgTestDisplay = WM_APP + 9;
constexpr UINT kMsgTestDpi = WM_APP + 10;
// Test-instance only: write the board canvas to a fixed BMP in
// DESK_TEST_CONFIG_DIR. wParam = 0 or 1 selects the fixed slot; the return
// value is 1 on success. Never accepts a path from the caller.
constexpr UINT kMsgTestCanvasBmp = WM_APP + 11;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT_PTR kCaretTimerId = 2;
constexpr UINT kCaretBlinkMs = 530;

constexpr int kHotkeyRefresh = 1;
constexpr int kHotkeyMenu = 2;
constexpr int kHotkeyDetails = 3;
constexpr int kHotkeyQuit = 4;
constexpr int kHotkeyAssistant = 5;
constexpr int kModuleHotkeyBase = 1000;

constexpr UINT kMenuTidy = 10;
constexpr UINT kMenuRefreshAll = 11;
constexpr UINT kMenuGrid = 12;
constexpr UINT kMenuSolid = 13;
constexpr UINT kMenuAutostart = 14;
constexpr UINT kMenuExit = 15;

constexpr UINT kMenuCardDetails = 20;
constexpr UINT kMenuCardRefresh = 21;
constexpr UINT kMenuCardRemove = 22;

constexpr UINT kMenuModuleBase = 100;
constexpr UINT kMenuAddBase = 500;

constexpr UINT kMenuFitCards = 16;
constexpr UINT kMenuResetPanel = 17;
constexpr UINT kMenuRestoreLayout = 18;
constexpr UINT kMenuSummonAssistant = 19;

constexpr int kClickSlop = 4;
constexpr int kRepaintMs = 16;
constexpr int kHintMs = 4000;          // transient footer notice duration
constexpr int kBoardConfigVersion = 2; // one-time geometry migration marker
constexpr int kCardGap = 12;           // minimum space between two cards
constexpr int kGrowStep = 8;           // auto-growth search step (one grid cell)

} // namespace

class Board {
public:
    // Programmatic lifecycle. BoardHost forwards to these; RunBoard() is built
    // on them too. Start() does everything up to (not including) the message
    // loop, so a host or a test can drive the board directly.
    BoardHost::StartResult Start(std::vector<std::unique_ptr<Module>> modules,
                                 BoardOptions options);
    void Run();
    void Quit();
    void Finish();
    bool started() const { return started_; }
    BoardMetrics Metrics() const;
    bool PreviewPlan(const Rect& board, std::vector<Rect>* cards) const;
    void ReadaptToDisplay();
    void ApplyScriptedSuggestedRect();
    bool ShowCard(int index, bool visible);
    bool SetCardExpanded(int index, bool expanded);
    bool RefreshCardAt(int index);
    bool ResetStandardPanel();
    bool RestorePreviousLayout();
    HWND Window() const { return hwnd_; }

private:
    friend class BoardHost;
    struct CardState {
        int x = skin::kBoardPad + 6;
        int y = skin::kBoardTitleH + 6;
        bool visible = true;
        bool expanded = false;
        bool placed = false;
    };

    struct ModuleRuntime {
        // Module destructors may use their bound Store; destroy them first.
        Store store;
        std::unique_ptr<Module> module;
        ModuleSpec spec;
        CardState state;
        CardSize size;
        StatusChip status;
        bool failed = false;
        bool layoutFailed = false;
        bool lastOk = true;
        std::thread worker;
        bool busy = false;
        bool again = false;
        ULONGLONG dueAt = 0;
        // Last body rectangle reported through OnBodySizeChanged, and whether
        // the last BodyHeight() had to be clamped down to MaxBodyHeight().
        Rect lastBody_;
        bool hasBody_ = false;
        bool lastSizeExceeded_ = false;
    };

    struct VisibleCard {
        int module = -1;
        Rect rect;
    };

    struct LayoutPlan {
        Rect board;
        std::vector<int> indices;
        std::vector<Rect> cards;
        std::vector<CardSize> sizes;
    };

    void Render();
    void RenderThrottled();
    void LayoutCards();
    void EnsureAddSlots();
    void RecomputeAddSlots();
    Rect Interior() const;
    Rect InteriorFor(const Rect& rect) const;

    // Plans the whole visible card set for 'target' (and, when allowGrow is
    // set, for progressively enlarged boards) before anything is committed.
    // On failure *out is untouched and *status/*blockedIndex describe why.
    bool PlanLayout(const Rect& target, int priority, int pendingIndex, bool allowGrow,
                    LayoutPlan* out, board::PlanStatus* status = nullptr,
                    int* blockedIndex = nullptr) const;
    bool ApplyLayout(const LayoutPlan& plan);
    void LayoutError(const wchar_t* operation, const wchar_t* reason = nullptr) const;
    void SetHint(std::wstring text, int ms);
    std::wstring FooterHint() const;

    // Monitor geometry. The board always sits inside a work area, is placed
    // flush right on first run, and auto-growth never leaves the right half.
    RECT TargetWork() const;
    Rect DefaultBoardRect(const RECT& work) const;
    int MaxBoardWidth(const RECT& work) const;
    int MaxBoardHeight(const RECT& work) const;
    int WindowDpi() const;
    void AdaptToWorkArea(bool persist);
    bool AdoptStandardPanel();
    bool BackupConfigFile(const wchar_t* suffix) const;
    bool MigrateBoardConfig();
    void SaveRestoreSnapshot();
    void SaveRestoreSnapshotOf(const Rect& board, const std::vector<POINT>& positions);
    void ClearRestoreSnapshot();
    // Restores the exact saved layout. Returns true only when the full saved
    // rectangle fits the current work area and the layout plans without
    // growth; otherwise the snapshot is kept for a later attempt.
    bool TryRestoreSavedLayout(bool notify);
    // Take a restore snapshot only when none is pending. A second display
    // change must never replace the earliest layout the user still may want
    // to come back to.
    void SnapshotLayoutIfNoPending();
    // One display-change path shared by WM_DPICHANGED and the work-area
    // adaptation: try an unresolved restore first, snapshot the current layout,
    // then plan the suggested rectangle (falling back to AdaptToWorkArea).
    void HandleDpiChanged(const RECT* suggested);
    // Emits a BoardEvent (UI thread, guarded against reentrancy) and disables
    // itself when no callback is configured.
    void Emit(BoardEventKind kind, board::PlanStatus status = board::PlanStatus::Ok,
              int card = -1, const Rect& rect = {}, const std::wstring& detail = {});
    // Calls OnBodySizeChanged for every visible card whose body rect changed,
    // and emits SizeRejected when a component's request had to be clamped.
    void DispatchBodySizes();
    // options.display, or DESK_TEST_DISPLAY_FILE (scripted) / system default.
    static std::shared_ptr<DisplaySource> DefaultDisplaySource();
    void FallbackLayout(board::PlanStatus status = board::PlanStatus::NotFound);

    void ArmTimer();
    void StartRefresh(int index);
    void FinishRefresh(int index);
    void RefreshAll();
    void ToggleDetailsAll();
    void ToggleExpanded(int index);
    void FitCards();
    bool CardsAreClear() const;

    void SaveState();
    void StartSaveThread();
    void StopSaveThread();
    void SaveLoop();
    void LoadModuleStates();
    void LoadBoardRect(bool hadState);
    bool PlaceMissingCards();
    void SeedDefaultPositions();
    void MigrateAutostart(bool hadState);
    RECT WorkArea() const;
    RECT MonitorWork() const;

    void PinToDesktop();
    void AddTray();
    void RemoveTray();
    HICON BuildIcon();
    void Shutdown();

    void ShowMenu(bool boardMenu, int cardIndex);
    void AddModule(int index);
    void RemoveModule(int index);
    void Tidy();
    LONG_PTR BeginMenu();
    void EndMenu(LONG_PTR oldExStyle);

    board::Hit HitAt(int x, int y);
    void UpdateCursor(const board::Hit& hit, int x, int y);
    void ResetGesture();
    void CancelGesture();

    // Keyboard focus. The board only ever takes focus for a card that claimed
    // the click with Module::WantsKeyboard(); the window itself stops being
    // WS_EX_NOACTIVATE, but WM_MOUSEACTIVATE still says "don't activate" for
    // every other click, so cards stay click-through as far as focus goes.
    void FocusCard(int index);
    int AssistantIndex() const;
    void SummonAssistant();
    // Test-instance only own-canvas export (see kMsgTestCanvasBmp).
    bool ExportTestCanvas(int slot);
    void BlurCard();
    void PasteIntoFocusedCard();
    void SetClipboardText(std::wstring_view text);
    bool FocusedBodyRect(Rect* body) const;
    void ForwardKey(WPARAM vk);
    void ForwardChar(wchar_t c);

    void NotifyDirty() {
        if (hwnd_) PostMessageW(hwnd_, kMsgDirty, 0, 0);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void CALLBACK OnForeground(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                      LONG a, LONG b, DWORD c, DWORD d);

    std::vector<std::unique_ptr<ModuleRuntime>> modules_;
    BoardOptions options_;
    Theme theme_;
    std::wstring configDir_;
    std::wstring className_;

    Store boardStore_;
    Canvas canvas_;
    Rect boardRect_;
    bool grid_ = true;
    bool solid_ = false;
    bool quitting_ = false;

    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW nid_{};
    bool tray_ = false;
    bool assistantHotkeyRegistered_ = false;
    bool testCanvasExport_ = false;
    HICON icon_ = nullptr;
    UINT taskbarCreated_ = 0;
    HWINEVENTHOOK fgHook_ = nullptr;
    HANDLE mutex_ = nullptr;

    std::vector<VisibleCard> visible_;
    std::vector<Rect> addSlots_;

    int hoverCard_ = -1;
    int focusCard_ = -1;
    bool caretOn_ = true;
    bool assistantSummoned_ = false;
    std::chrono::steady_clock::time_point lastPaint_{};

    board::HitKind pressKind_ = board::HitKind::Empty;
    int pressCard_ = -1;
    POINT pressScreen_{};
    POINT pressBoard_{};
    int pressCardX_ = 0;
    int pressCardY_ = 0;
    int pressW_ = 0;
    int pressH_ = 0;
    bool pressMoved_ = false;
    bool pressToggle_ = false;
    int dragCard_ = -1;
    bool draggingBoard_ = false;
    bool resizing_ = false;
    bool resizeRejected_ = false;

    // Cached "ADD MODULE" slots: recomputed only when the board size or the
    // visible card rects change, never on every repaint.
    bool addSlotsDirty_ = true;
    // True while the saved cards cannot be arranged at all: the board still
    // starts, shows NO SPACE chips and offers repair actions in the menu.
    bool layoutIncomplete_ = false;
    // A display change adapted the layout in memory; the original rectangles
    // are kept under saved.* keys until the user changes the layout or restores.
    bool restorePending_ = false;
    // The saved layout as it was on disk before this start: needed so a start
    // on a smaller work area can snapshot the original instead of overwriting
    // it with the adapted coordinates.
    Rect originalBoardRect_;
    bool originalBoardValid_ = false;
    std::vector<POINT> originalCardPositions_;

    std::shared_ptr<DisplaySource> display_;
    bool started_ = false;
    bool shutDown_ = false;
    bool emittingEvent_ = false;
    std::wstring hintOverride_;
    ULONGLONG hintUntil_ = 0;

    HWND previousForeground_ = nullptr;

    std::thread saveThread_;
    std::mutex saveMu_;
    std::condition_variable saveCv_;
    std::map<std::wstring, std::wstring> savePending_;
    bool savePendingSet_ = false;
    bool saveStop_ = false;
};

namespace {
Board* g_board = nullptr;

}

BoardHost::StartResult Board::Start(std::vector<std::unique_ptr<Module>> modules,
                                      BoardOptions options) {
    options_ = std::move(options);
    shutDown_ = false;
    quitting_ = false;

    const std::wstring mutexName = options_.id + L"SingleInstance";
    mutex_ = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (mutex_ && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex_);
        mutex_ = nullptr;
        return BoardHost::StartResult::AlreadyRunning;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // The board never reads the desktop directly: the source is either
    // injected by the application or the system one (a test may point
    // DESK_TEST_DISPLAY_FILE at a scripted source).
    display_ = options_.display ? options_.display : DefaultDisplaySource();

    std::wstring configError;
    configDir_ = appenv::ConfigDir(options_.title, &configError);
    if (configDir_.empty()) {
        if (configError.empty()) {
            Log(LogLevel::Error, L"config", L"cannot resolve the configuration directory");
        } else {
            Log(LogLevel::Error, L"config", configError);
        }
        Finish();
        return BoardHost::StartResult::Failed;
    }
    // The canvas export is a test-only diagnostic for layered windows whose
    // PrintWindow result is blank. It needs both a valid test instance and an
    // explicit, already validated DESK_TEST_CONFIG_DIR.
    testCanvasExport_ = appenv::TestConfigDirOverride();
    const bool hadState = boardStore_.Open(configDir_ + L"\\" + options_.configFile);
    MigrateAutostart(hadState);

    modules_.reserve(modules.size());
    for (auto& module : modules) {
        if (!module) continue;
        auto rt = std::make_unique<ModuleRuntime>();
        rt->module = std::move(module);
        rt->spec = rt->module->Describe();
        if (rt->spec.width < 80) rt->spec.width = 80;

        const std::wstring file =
            rt->spec.configFile.empty() ? rt->spec.id + L".ini" : rt->spec.configFile;
        rt->store.Open(configDir_ + L"\\" + file);
        rt->module->Bind(&rt->store, [this] { NotifyDirty(); });
        const int index = (int)modules_.size();
        rt->module->BindFocus([this, index] { FocusCard(index); });
        rt->module->BindClipboard([this](std::wstring_view text) { SetClipboardText(text); });
        rt->module->BindRefresh([this, index] {
            if (hwnd_ && !quitting_) {
                PostMessageW(hwnd_, kMsgModuleRefresh, (WPARAM)index, 0);
            }
        });
        modules_.push_back(std::move(rt));
    }

    if (modules_.empty()) {
        Finish();
        return BoardHost::StartResult::Failed;
    }

    // State first (expanded/visible/position), then Load so the initial card
    // heights are available for the default flow.
    LoadModuleStates();
    for (auto& rt : modules_) rt->module->Load();
    LoadBoardRect(hadState);
    SeedDefaultPositions();
    bool migrated = false;
    if (!hadState) {
        // A fresh install already uses the current geometry; it must never be
        // migrated again later.
        boardStore_.SetInt(L"board.version", kBoardConfigVersion);
    } else {
        migrated = MigrateBoardConfig();
    }
    // A display too small for the saved layout must not stop the app: start,
    // keep every card state, and let the user repair the layout from the menu.
    if (!PlaceMissingCards()) FallbackLayout();
    const bool hadPendingRestore = restorePending_;
    if (hadPendingRestore) TryRestoreSavedLayout(false);
    const bool restoredNow = hadPendingRestore && !restorePending_;
    // Starting on a smaller display than the saved rectangle must not destroy
    // the original layout: snapshot it before the adapted coordinates are
    // written back, so a later start on the original display can restore it.
    if (!restoredNow && !restorePending_ && originalBoardValid_ && !migrated) {
        const bool boardAdapted = boardRect_.x != originalBoardRect_.x ||
                                  boardRect_.y != originalBoardRect_.y ||
                                  boardRect_.w != originalBoardRect_.w ||
                                  boardRect_.h != originalBoardRect_.h;
        if (boardAdapted) {
            SaveRestoreSnapshotOf(originalBoardRect_, originalCardPositions_);
            Log(LogLevel::Info, L"layout",
                L"startup adapted the layout; original rectangles snapshotted");
            Emit(BoardEventKind::LayoutRestorePending, board::PlanStatus::Ok, -1,
                 originalBoardRect_);
            if (!layoutIncomplete_) {
                SetHint(L"STARTUP ADAPTED THE LAYOUT - MENU: RESTORE PREVIOUS LAYOUT",
                        8000);
            }
        }
    }
    if (!layoutIncomplete_ && hintOverride_.empty()) {
        // The standard right panel does not fit this display: say so instead of
        // silently presenting a smaller board.
        const RECT work = TargetWork();
        const int nominalW = MulDiv(options_.width, WindowDpi(), 96);
        const int nominalH = MulDiv(options_.height, WindowDpi(), 96);
        if (nominalW > (int)(work.right - work.left) / 2 ||
            nominalH > (int)(work.bottom - work.top)) {
            SetHint(L"DISPLAY SMALLER THAN THE STANDARD PANEL - SIZED TO FIT", 8000);
        }
    }

    className_ = L"wg." + options_.id;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Board::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = className_.c_str();
    RegisterClassExW(&wc);

    // WS_EX_NOACTIVATE is deliberately absent: a card that owns the keyboard
    // (the console) needs real focus. Every other click answers MA_NOACTIVATE
    // in WM_MOUSEACTIVATE, so the board still never steals focus by itself.
    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    hwnd_ = CreateWindowExW(exStyle, className_.c_str(), options_.title.c_str(), WS_POPUP,
                            boardRect_.x, boardRect_.y, boardRect_.w, boardRect_.h, nullptr,
                            nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) {
        Finish();
        return BoardHost::StartResult::Failed;
    }

    g_board = this;
    StartSaveThread();
    SaveState();

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (options_.enableTray) AddTray();
    PinToDesktop();
    fgHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                              &Board::OnForeground, 0, 0, WINEVENT_OUTOFCONTEXT);

    if (options_.enableHotkeys) {
        // assistantOnlyHotkeys is the physical test-probe gate: no board or
        // module chord may be claimed, only the assistant summon chord.
        if (!options_.assistantOnlyHotkeys) {
            RegisterHotKey(hwnd_, kHotkeyRefresh, MOD_CONTROL | MOD_ALT,
                           options_.refreshHotkeyVk);
            RegisterHotKey(hwnd_, kHotkeyMenu, MOD_CONTROL | MOD_ALT, options_.menuHotkeyVk);
            RegisterHotKey(hwnd_, kHotkeyDetails, MOD_CONTROL | MOD_ALT,
                           options_.detailsHotkeyVk);
            RegisterHotKey(hwnd_, kHotkeyQuit, MOD_CONTROL | MOD_ALT,
                           options_.quitHotkeyVk);
            for (int i = 0; i < (int)modules_.size(); ++i) {
                for (const Hotkey& hk : modules_[i]->spec.hotkeys) {
                    if (hk.vk == 0 || hk.id <= 0 || hk.id >= 100) continue;
                    RegisterHotKey(hwnd_, kModuleHotkeyBase + i * 100 + hk.id, hk.mods,
                                   hk.vk);
                }
            }
        }
        // Only a registered assistant may take the summon chord; minimal-board
        // and other SDK consumers must not grab Ctrl+Alt+A.
        if (AssistantIndex() >= 0) {
            assistantHotkeyRegistered_ =
                RegisterHotKey(hwnd_, kHotkeyAssistant, options_.assistantHotkeyModifiers,
                               options_.assistantHotkeyVk) != FALSE;
            if (!assistantHotkeyRegistered_) {
                const DWORD code = GetLastError();
                const std::wstring message =
                    L"cannot register the assistant hotkey (Win32 error " +
                    std::to_wstring(code) + L"); use the Summon assistant menu";
                Log(LogLevel::Warn, L"hotkey", message);
                OutputDebugStringW((L"[board] " + message + L"\n").c_str());
            }
        }
    }
    for (auto& rt : modules_) rt->dueAt = 0;

    LayoutCards();
    Render();
    if (options_.enableHotkeys && AssistantIndex() >= 0 && !assistantHotkeyRegistered_) {
        // Registration can fail when another application already owns the
        // chord. Keep the menu path discoverable instead of failing silently.
        SetHint(L"ASSISTANT HOTKEY UNAVAILABLE - RIGHT CLICK: SUMMON ASSISTANT", 10000);
    }
    ArmTimer();
    started_ = true;
    return BoardHost::StartResult::Started;
}

void Board::Run() {
    if (!started_ || quitting_) return;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Shutdown();
    started_ = false;
}

void Board::Quit() {
    if (!started_) return;
    if (hwnd_ && IsWindow(hwnd_)) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void Board::Finish() {
    // Synchronous teardown for a host that started the board but never ran the
    // message loop (tests, embedding). Shutdown is idempotent.
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    Shutdown();
    started_ = false;
}

void Board::LoadModuleStates() {
    for (auto& rt : modules_) {
        const std::wstring prefix = L"card." + rt->spec.id + L".";
        const std::wstring kExpanded = prefix + L"expanded";
        const std::wstring kVisible = prefix + L"visible";
        const std::wstring kX = prefix + L"x";
        const std::wstring kY = prefix + L"y";

        if (boardStore_.Has(kExpanded.c_str())) {
            rt->state.expanded = boardStore_.GetInt(kExpanded.c_str(), 0) != 0;
        } else {
            rt->state.expanded = rt->spec.expandedByDefault;
        }
        if (boardStore_.Has(kVisible.c_str())) {
            rt->state.visible = boardStore_.GetInt(kVisible.c_str(), 1) != 0;
        } else {
            rt->state.visible = rt->spec.visibleByDefault;
        }
        if (boardStore_.Has(kX.c_str()) && boardStore_.Has(kY.c_str())) {
            rt->state.x = (int)boardStore_.GetInt(kX.c_str(), rt->state.x);
            rt->state.y = (int)boardStore_.GetInt(kY.c_str(), rt->state.y);
            rt->state.placed = true;
        }
        rt->module->SetExpanded(rt->state.expanded);
    }
}

RECT Board::TargetWork() const {
    // Prefer the monitor that hosted the saved board, so a board living on a
    // secondary display is not teleported to the primary one.
    if (boardStore_.Has(L"board.x") && boardStore_.Has(L"board.y") &&
        boardStore_.Has(L"board.w") && boardStore_.Has(L"board.h")) {
        const int x = (int)boardStore_.GetInt(L"board.x", 0);
        const int y = (int)boardStore_.GetInt(L"board.y", 0);
        const int w = (int)boardStore_.GetInt(L"board.w", 0);
        const int h = (int)boardStore_.GetInt(L"board.h", 0);
        POINT centre{x + w / 2, y + h / 2};
        if (display_) {
            const RECT work = display_->WorkAreaForPoint(centre);
            if (work.right > work.left && work.bottom > work.top) return work;
            Log(LogLevel::Warn, L"display",
                L"DisplaySource returned an empty work area for the saved board");
        }
    }
    return WorkArea();
}

std::shared_ptr<DisplaySource> Board::DefaultDisplaySource() {
    // Test-only: a scripted source lets the harness simulate work-area/DPI
    // changes on one physical display. Applications can inject their own source
    // through BoardOptions::display instead.
    wchar_t file[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"DESK_TEST_DISPLAY_FILE", file, MAX_PATH) > 0 && file[0]) {
        auto scripted = ScriptedDisplaySource::Create(file);
        if (scripted) return scripted;
        Log(LogLevel::Warn, L"display", L"cannot create the scripted display source");
    }
    return CreateSystemDisplaySource();
}

int Board::WindowDpi() const {
    if (display_) {
        const int dpi = hwnd_ ? display_->DpiForWindow(hwnd_) : display_->SystemDpi();
        if (dpi > 0) return dpi;
        Log(LogLevel::Warn, L"display", L"DisplaySource returned an invalid DPI");
        return 96;
    }
    if (hwnd_) {
        const UINT dpi = GetDpiForWindow(hwnd_);
        if (dpi) return (int)dpi;
    }
    const UINT dpi = GetDpiForSystem();
    return dpi ? (int)dpi : 96;
}

Rect Board::DefaultBoardRect(const RECT& work) const {
    const int workW = std::max(1, (int)(work.right - work.left));
    const int workH = std::max(1, (int)(work.bottom - work.top));
    const int dpi = WindowDpi();
    int w = MulDiv(options_.width, dpi, 96);
    int h = MulDiv(options_.height, dpi, 96);

    // The panel owns the right half of the work area, scaled for DPI and then
    // limited by it: the origin is derived per display, never hard-coded.
    w = std::min(w, std::max(options_.minWidth, workW / 2));
    w = std::min(std::max(w, options_.minWidth), workW);
    h = std::min(std::max(h, options_.minHeight), workH);

    Rect r;
    r.w = w;
    r.h = h;
    r.x = std::max((int)work.left, (int)work.right - w);
    r.y = (int)work.top;
    return r;
}

int Board::MaxBoardWidth(const RECT& work) const {
    const int workW = std::max(1, (int)(work.right - work.left));
    const int cap = std::min(MulDiv(options_.width, WindowDpi(), 96),
                             std::max(options_.minWidth, workW / 2));
    return std::max(options_.minWidth, std::min(cap, workW));
}

int Board::MaxBoardHeight(const RECT& work) const {
    const int workH = std::max(1, (int)(work.bottom - work.top));
    return std::max(options_.minHeight, workH);
}

void Board::LoadBoardRect(bool hadState) {
    const RECT work = TargetWork();
    Rect want = DefaultBoardRect(work);

    originalBoardValid_ = false;
    originalCardPositions_.clear();
    if (hadState) {
        want.x = (int)boardStore_.GetInt(L"board.x", want.x);
        want.y = (int)boardStore_.GetInt(L"board.y", want.y);
        want.w = (int)boardStore_.GetInt(L"board.w", want.w);
        want.h = (int)boardStore_.GetInt(L"board.h", want.h);
        // Keep the exact saved rectangle and card positions: if this start has
        // to adapt them to a smaller display, these are what a later restore
        // must bring back.
        originalBoardRect_ = want;
        originalBoardValid_ = true;
        originalCardPositions_.reserve(modules_.size());
        for (const auto& rt : modules_) {
            originalCardPositions_.push_back(POINT{rt->state.x, rt->state.y});
        }
    }
    boardRect_ = board::ClampBoard(want, work, options_.minWidth, options_.minHeight);
    grid_ = boardStore_.GetInt(L"board.grid", 1) != 0;
    solid_ = boardStore_.GetInt(L"board.solid", 0) != 0;
    restorePending_ = boardStore_.GetInt(L"saved.pending", 0) != 0;
    addSlotsDirty_ = true;
}

bool Board::PlaceMissingCards() {
    LayoutPlan plan;
    if (!PlanLayout(boardRect_, -1, -1, true, &plan)) return false;
    ApplyLayout(plan);
    return true;
}

void Board::SeedDefaultPositions() {
    // First-run cards have no saved rectangle. Lay them out like the design's
    // reference shelf (Account and Servers side by side, then the terminal on
    // the next row) instead of stacking everything in the corner; positions
    // already loaded from the config are never touched.
    const Rect interior = Interior();
    const int grid = std::max(1, options_.grid);
    const auto alignUp = [grid](int origin, int value) {
        if (grid <= 1) return value;
        const int delta = value - origin;
        return origin + ((delta + grid - 1) / grid) * grid;
    };
    int x = interior.x;
    int y = interior.y;
    int rowHeight = 0;
    for (auto& rt : modules_) {
        if (!rt->state.visible || rt->state.placed) continue;
        const int body = ClampBodyHeight(rt->module->BodyHeight(),
                                         rt->module->MinBodyHeight(),
                                         rt->module->MaxBodyHeight());
        const CardSize size = MeasureCardSize(rt->spec.sizePreset, rt->spec.width, body,
                                              interior.w);
        const int w = size.representable ? size.width : std::max(80, rt->spec.width);
        const int h = size.representable ? size.height : kCardChromeHeight;
        if (x != interior.x && x + w > interior.right()) {
            x = interior.x;
            y = alignUp(interior.y, y + rowHeight + kCardGap);
            rowHeight = 0;
        }
        // Seeds obey the same grid rule as drag/plan results; the 12 px gap is
        // rounded up to the next 8 px row (so rows are 16 px apart).
        rt->state.x = alignUp(interior.x, x);
        rt->state.y = alignUp(interior.y, y);
        x = rt->state.x + w + kCardGap;
        rowHeight = std::max(rowHeight, h);
    }
}

void Board::FallbackLayout(board::PlanStatus status) {
    // The board still starts. Every card keeps its saved position and state;
    // positions are only clamped into the interior so nothing is lost off the
    // board, and the chips say why the layout is incomplete.
    layoutIncomplete_ = true;
    const Rect interior = Interior();
    for (auto& rt : modules_) {
        if (!rt->state.visible) continue;
        const int body = ClampBodyHeight(rt->module->BodyHeight(),
                                         rt->module->MinBodyHeight(),
                                         rt->module->MaxBodyHeight());
        CardSize size = MeasureCardSize(rt->spec.sizePreset, rt->spec.width, body,
                                        interior.w);
        if (!size.representable) {
            const SizePreset preset =
                ResolveSizePreset(rt->spec.sizePreset, rt->spec.width, interior.w);
            const SizeRule rule = SizePresetRule(preset, interior.w);
            size.preset = preset;
            size.width = rule.width > 0 ? rule.width : std::max(80, rt->spec.width);
            size.height = std::max(rule.minHeight, kCardChromeHeight);
            size.representable = size.width > 0;
        }
        rt->size = size;
        rt->layoutFailed = true;
        const Rect clamped = board::ClampCard(rt->state.x, rt->state.y, size.width,
                                              size.height, interior);
        rt->state.x = clamped.x;
        rt->state.y = clamped.y;
    }
    SetHint(L"LAYOUT INCOMPLETE - RIGHT CLICK: FIT CARDS / RESET PANEL", 0);
    Log(LogLevel::Warn, L"layout", L"cards do not fit the board; layout incomplete");
    DispatchBodySizes();
    Emit(BoardEventKind::LayoutIncomplete, status, -1, boardRect_);
}

bool Board::BackupConfigFile(const wchar_t* suffix) const {
    const std::wstring path = boardStore_.path();
    if (path.empty()) return false;
    const std::wstring tag = suffix && *suffix ? suffix : L"bak";
    const std::wstring backup = path + L"." + tag + L".bak";
    if (CopyFileW(path.c_str(), backup.c_str(), FALSE)) return true;
    // A config that does not exist yet has nothing to back up.
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
}

bool Board::MigrateBoardConfig() {
    // One-time geometry migration: older desk.ini files still carry the old
    // 760x800 board and win over the new defaults. The config is backed up
    // first, the new size plus a full layout plan is applied in memory, and
    // only a successful plan is saved. A failed plan leaves the file untouched
    // and retries on a later start (for example on a larger display).
    if (boardStore_.GetInt(L"board.version", 0) >= kBoardConfigVersion) return false;
    if (!BackupConfigFile(L"pre-v2")) return false;

    const Rect oldBoard = boardRect_;
    std::vector<POINT> oldPositions;
    oldPositions.reserve(modules_.size());
    for (const auto& rt : modules_) oldPositions.push_back(POINT{rt->state.x, rt->state.y});

    const Rect target = DefaultBoardRect(TargetWork());
    LayoutPlan plan;
    if (!PlanLayout(target, -1, -1, true, &plan)) {
        boardRect_ = oldBoard;
        for (std::size_t i = 0; i < modules_.size() && i < oldPositions.size(); ++i) {
            modules_[i]->state.x = oldPositions[i].x;
            modules_[i]->state.y = oldPositions[i].y;
        }
        SetHint(L"BOARD MIGRATION DEFERRED - LAYOUT DID NOT FIT", 8000);
        Log(LogLevel::Warn, L"migration", L"layout did not fit; migration deferred");
        Emit(BoardEventKind::MigrationDeferred, board::PlanStatus::Insufficient, -1,
             boardRect_);
        return false;
    }

    ApplyLayout(plan);
    boardStore_.SetInt(L"board.version", kBoardConfigVersion);
    SaveState();
    SetHint(L"BOARD RESIZED TO THE RIGHT HALF - CONFIG BACKED UP", 6000);
    Log(LogLevel::Info, L"migration", L"board geometry migrated to the standard panel");
    Emit(BoardEventKind::MigrationApplied, board::PlanStatus::Ok, -1, boardRect_);
    return true;
}

bool Board::AdoptStandardPanel() {
    const RECT work = hwnd_ ? MonitorWork() : TargetWork();
    const Rect target = DefaultBoardRect(work);
    LayoutPlan plan;
    board::PlanStatus status = board::PlanStatus::NotFound;
    int blocked = -1;
    if (!PlanLayout(target, -1, -1, true, &plan, &status, &blocked)) {
        LayoutError(L"The board cannot be reset",
                    status == board::PlanStatus::Insufficient
                        ? L"The cards need more space than this display's right half provides."
                        : L"No arrangement was found within the layout search limits.");
        return false;
    }
    if (!BackupConfigFile(L"pre-reset")) {
        LayoutError(L"The board cannot be reset",
                    L"The configuration file could not be backed up.");
        return false;
    }
    ApplyLayout(plan);
    ClearRestoreSnapshot();
    boardStore_.SetInt(L"board.version", kBoardConfigVersion);
    SaveState();
    SetHint(L"STANDARD RIGHT PANEL RESTORED", kHintMs);
    LayoutCards();
    Render();
    return true;
}

void Board::MigrateAutostart(bool hadState) {
    if (hadState) return;

    bool old = false;
    for (const auto& name : options_.legacyAutostartNames) {
        if (!appenv::AutostartCommand(name).empty()) {
            appenv::RemoveAutostart(name);
            old = true;
        }
    }
    if (old || options_.startWithWindows) {
        appenv::SetAutostart(options_.title, true);
    }
}

RECT Board::WorkArea() const {
    if (display_) {
        const RECT work = display_->PrimaryWorkArea();
        if (work.right > work.left && work.bottom > work.top) return work;
        Log(LogLevel::Warn, L"display",
            L"DisplaySource returned an empty primary work area; using the system one");
    }
    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    return work;
}

RECT Board::MonitorWork() const {
    if (display_) {
        POINT centre{boardRect_.x + boardRect_.w / 2, boardRect_.y + boardRect_.h / 2};
        const RECT work = display_->WorkAreaForPoint(centre);
        if (work.right > work.left && work.bottom > work.top) return work;
        Log(LogLevel::Warn, L"display",
            L"DisplaySource returned an empty monitor work area; using the primary one");
        return WorkArea();
    }
    RECT work{};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (hwnd_ && GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
        work = mi.rcWork;
    } else {
        work = WorkArea();
    }
    return work;
}

Rect Board::Interior() const {
    return InteriorFor(boardRect_);
}

Rect Board::InteriorFor(const Rect& rect) const {
    Rect r;
    r.x = skin::kBoardPad;
    r.y = skin::kBoardTitleH + 6;
    r.w = std::max(1, rect.w - 2 * skin::kBoardPad);
    r.h = std::max(1, rect.h - skin::kBoardTitleH - 6 - skin::kBoardPad -
                          skin::kBoardResize);
    return r;
}

bool Board::PlanLayout(const Rect& target, int priority, int pendingIndex,
                       bool allowGrow, LayoutPlan* out, board::PlanStatus* status,
                       int* blockedIndex) const {
    if (!out) return false;
    board::PlanStatus reason = board::PlanStatus::NotFound;
    int reasonIndex = -1;

    std::vector<int> indices;
    int prioritySlot = -1;
    for (int i = 0; i < (int)modules_.size(); ++i) {
        const auto& rt = *modules_[i];
        if (!rt.state.visible) continue;
        if (i == priority) prioritySlot = (int)indices.size();
        indices.push_back(i);
    }

    // One planning attempt for one candidate board size. Card sizes are
    // measured against that candidate's interior, so a Full-width card really
    // follows the panel width instead of a stale measurement.
    const auto attempt = [&](const Rect& board, LayoutPlan* plan, board::PlanStatus* why,
                             int* whyIndex) -> bool {
        const Rect interior = InteriorFor(board);
        std::vector<Rect> desired;
        std::vector<CardSize> sizes;
        desired.reserve(indices.size());
        sizes.reserve(indices.size());
        for (std::size_t slot = 0; slot < indices.size(); ++slot) {
            const auto& rt = *modules_[indices[slot]];
            const int requested = indices[slot] == pendingIndex
                                      ? rt.module->PendingBodyHeight()
                                      : rt.module->BodyHeight();
            // Components may declare a narrower renderable range; the measured
            // size always uses the clamped value so no content is silently cut.
            const int body = ClampBodyHeight(requested, rt.module->MinBodyHeight(),
                                             rt.module->MaxBodyHeight()); 
            const CardSize size =
                MeasureCardSize(rt.spec.sizePreset, rt.spec.width, body, interior.w);
            if (!size.representable) {
                // Not measurable at all: a Full card without a usable interior,
                // a broken body height or an unknown preset.
                if (why) *why = board::PlanStatus::Insufficient;
                if (whyIndex) *whyIndex = indices[slot];
                return false;
            }
            desired.push_back(Rect{rt.state.x, rt.state.y, size.width, size.height});
            sizes.push_back(size);
        }

        std::vector<Rect> cards;
        board::PlanStatus result = board::PlanStatus::Ok;
        int blocked = -1;
        if (!board::TryPlanLayout(desired, {}, interior, kCardGap, options_.grid, &cards,
                                  prioritySlot, &result, &blocked)) {
            if (why) *why = result;
            if (whyIndex) {
                *whyIndex = blocked >= 0 && blocked < (int)indices.size()
                                ? indices[(std::size_t)blocked]
                                : -1;
            }
            return false;
        }
        plan->board = board;
        plan->indices = indices;
        plan->cards = std::move(cards);
        plan->sizes = std::move(sizes);
        return true;
    };

    if (attempt(target, out, &reason, &reasonIndex)) return true;
    if (!allowGrow) {
        if (status) *status = reason;
        if (blockedIndex) *blockedIndex = reasonIndex;
        return false;
    }

    // Grow only when needed. board::FindMinimalGrow scans every 8 px width up
    // to the right-half cap, binary-searches the smallest fitting height for
    // each width and returns the smallest-area candidate, right-aligned in the
    // work area so an expanded panel stays on that side of the screen (the real
    // fix for "growth used to anchor on the old window's right edge").
    // Nothing is committed until the final attempt succeeds.
    const RECT work = hwnd_ ? MonitorWork() : TargetWork();
    const int maxW = MaxBoardWidth(work);
    const int maxH = MaxBoardHeight(work);
    if (maxW <= 0 || maxH <= 0 || work.right <= work.left || work.bottom <= work.top) {
        if (status) *status = reason;
        if (blockedIndex) *blockedIndex = reasonIndex;
        return false;
    }

    board::GrowLimits limits;
    limits.minWidth = options_.minWidth;
    limits.minHeight = options_.minHeight;
    limits.maxWidth = maxW;
    limits.maxHeight = maxH;
    limits.step = kGrowStep;

    Rect grown;
    const auto fits = [&](const Rect& candidate) {
        return attempt(candidate, out, &reason, &reasonIndex);
    };
    if (board::FindMinimalGrow(target, work, limits, fits, &grown) &&
        attempt(grown, out, &reason, &reasonIndex)) {
        return true;
    }

    if (status) *status = reason;
    if (blockedIndex) *blockedIndex = reasonIndex;
    return false;
}

bool Board::ApplyLayout(const LayoutPlan& plan) {
    const bool boardChanged = plan.board.x != boardRect_.x || plan.board.y != boardRect_.y ||
                              plan.board.w != boardRect_.w || plan.board.h != boardRect_.h;
    bool changed = boardChanged;
    boardRect_ = plan.board;
    for (std::size_t i = 0; i < plan.indices.size(); ++i) {
        auto& rt = *modules_[plan.indices[i]];
        changed |= rt.state.x != plan.cards[i].x || rt.state.y != plan.cards[i].y ||
                   !rt.state.placed || rt.layoutFailed;
        rt.state.x = plan.cards[i].x;
        rt.state.y = plan.cards[i].y;
        rt.state.placed = true;
        rt.size = plan.sizes[i];
        rt.layoutFailed = false;
    }
    if (boardChanged && hwnd_) {
        SetWindowPos(hwnd_, nullptr, boardRect_.x, boardRect_.y, boardRect_.w, boardRect_.h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        addSlotsDirty_ = true;
    }
    if (changed) addSlotsDirty_ = true;
    layoutIncomplete_ = false;
    if (hintUntil_ == 0) hintOverride_.clear();
    DispatchBodySizes();
    Emit(BoardEventKind::LayoutApplied, board::PlanStatus::Ok, -1, boardRect_);
    return changed;
}

void Board::LayoutError(const wchar_t* operation, const wchar_t* reason) const {
    std::wstring message = operation ? operation : L"The layout cannot be changed";
    if (reason && *reason) {
        message += L"\n\n";
        message += reason;
    }
    message += L"\n\nThe board kept its previous layout and every card state.\n"
               L"Collapse details (Ctrl+Alt+D), remove a card, or use "
               L"\"Reset to right standard panel\" in the board menu to make room.";
    MessageBoxW(hwnd_, message.c_str(), options_.title.c_str(), MB_OK | MB_ICONINFORMATION);
}

void Board::SetHint(std::wstring text, int ms) {
    hintOverride_ = std::move(text);
    hintUntil_ = ms > 0 ? GetTickCount64() + (ULONGLONG)ms : 0;
    if (hwnd_ && !quitting_) Render();
}

std::wstring Board::FooterHint() const {
    if (hintOverride_.empty()) return options_.boardHint;
    if (hintUntil_ != 0 && GetTickCount64() >= hintUntil_) return options_.boardHint;
    return hintOverride_;
}

void Board::LayoutCards() {
    std::vector<VisibleCard> next;
    next.reserve(modules_.size());
    for (int i = 0; i < (int)modules_.size(); ++i) {
        auto& rt = *modules_[i];
        if (!rt.state.visible) continue;
        rt.module->SetExpanded(rt.state.expanded);
        const Rect r{rt.state.x, rt.state.y, rt.size.width, rt.size.height};
        next.push_back(VisibleCard{i, r});
    }

    // The add slots depend only on these rectangles and the board size, so
    // repaints can reuse the last result instead of searching again.
    const auto differs = [&] {
        if (next.size() != visible_.size()) return true;
        for (std::size_t i = 0; i < next.size(); ++i) {
            if (next[i].module != visible_[i].module ||
                next[i].rect.x != visible_[i].rect.x ||
                next[i].rect.y != visible_[i].rect.y ||
                next[i].rect.w != visible_[i].rect.w ||
                next[i].rect.h != visible_[i].rect.h) {
                return true;
            }
        }
        return false;
    };
    if (differs()) {
        visible_ = std::move(next);
        addSlotsDirty_ = true;
    }
}

void Board::EnsureAddSlots() {
    if (!addSlotsDirty_) return;
    addSlotsDirty_ = false;
    RecomputeAddSlots();
}

void Board::RecomputeAddSlots() {
    addSlots_.clear();
    const Rect interior = Interior();
    if (interior.w < skin::kAddW || interior.h < skin::kAddH) return;

    std::vector<Rect> occupied;
    occupied.reserve(visible_.size() + 2);
    for (const VisibleCard& card : visible_) occupied.push_back(card.rect);

    RECT work{interior.x, interior.y, interior.right(), interior.bottom()};
    for (int i = 0; i < 2; ++i) {
        const POINT p = FindFreeSpot(interior.x + 6, interior.y + 6, skin::kAddW,
                                     skin::kAddH, occupied, work, 12);
        Rect slot{p.x, p.y, skin::kAddW, skin::kAddH};
        if (slot.x < interior.x || slot.y < interior.y || slot.right() > interior.right() ||
            slot.bottom() > interior.bottom()) {
            break;
        }
        bool blocked = false;
        for (const Rect& other : occupied) {
            if (slot.intersects(other, 12)) {
                blocked = true;
                break;
            }
        }
        if (blocked) break;
        addSlots_.push_back(slot);
        occupied.push_back(slot);
    }
}

void Board::Render() {
    if (!hwnd_ || quitting_) return;
    if (canvas_.width() != boardRect_.w || canvas_.height() != boardRect_.h) {
        if (!canvas_.Resize(boardRect_.w, boardRect_.h)) return;
    }

    LayoutCards();
    // While a card or the board is being dragged the add-slot hints are not
    // used; skipping their search keeps the drag path free of layout work.
    const bool gesture = dragCard_ >= 0 || draggingBoard_ || resizing_;
    if (!gesture) EnsureAddSlots();
    canvas_.Clear();
    skin::BoardChrome(canvas_, Rect{0, 0, boardRect_.w, boardRect_.h}, options_.boardTitle,
                      FooterHint(), theme_, grid_, options_.dotGrid);

    if (!gesture) {
        for (const Rect& slot : addSlots_) skin::AddSlot(canvas_, slot, theme_);
    }

    for (const VisibleCard& vc : visible_) {
        auto& rt = *modules_[vc.module];
        StatusChip chip = rt.module->Status();
        if (rt.layoutFailed) {
            chip.text = L"NO SPACE";
            chip.color = theme_.warn;
        } else if (rt.failed) {
            chip.text = L"ERROR";
            chip.color = theme_.bad;
        } else if (!rt.size.heightWithinRange && rt.size.representable) {
            // The framework reports an oversized card instead of clipping it.
            chip.text = L"TOO TALL";
            chip.color = theme_.warn;
        }
        skin::CardChrome(canvas_, vc.rect, rt.spec.title, chip.text, chip.color, theme_,
                         rt.spec.hasDetails, rt.state.expanded, hoverCard_ == vc.module,
                         dragCard_ == vc.module);

        const Rect body = skin::CardBody(vc.rect);
        canvas_.SetClip(body.x, body.y, body.w, body.h);
        rt.module->PaintBody(canvas_, body);
        canvas_.ResetClip();
    }

    canvas_.Present(hwnd_, boardRect_.x, boardRect_.y);
}

void Board::RenderThrottled() {
    const auto now = std::chrono::steady_clock::now();
    if (lastPaint_ != std::chrono::steady_clock::time_point{} &&
        now - lastPaint_ < std::chrono::milliseconds(kRepaintMs)) {
        return;
    }
    lastPaint_ = now;
    Render();
}

void Board::ArmTimer() {
    if (!hwnd_ || quitting_) return;
    KillTimer(hwnd_, kTimerId);

    const ULONGLONG now = GetTickCount64();
    ULONGLONG next = 0;
    for (const auto& rt : modules_) {
        if (!rt->state.visible || rt->spec.refreshSeconds <= 0) continue;
        ULONGLONG due = rt->dueAt ? rt->dueAt : now + 50;
        if (due < now) due = now + 50;
        if (!next || due < next) next = due;
    }
    if (!next) return;

    const ULONGLONG delta = next > now ? next - now : 50;
    SetTimer(hwnd_, kTimerId, (UINT)std::min<ULONGLONG>(std::max<ULONGLONG>(delta, 50), 600000),
             nullptr);
}

void Board::StartRefresh(int index) {
    if (index < 0 || index >= (int)modules_.size()) return;
    auto& rt = *modules_[index];

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG interval =
        (ULONGLONG)std::max(0, rt.spec.refreshSeconds) * 1000ULL;
    rt.dueAt = interval ? now + interval : 0;

    if (rt.busy) {
        rt.again = true;
        return;
    }

    rt.busy = true;
    rt.lastOk = true;
    try {
        rt.worker = std::thread([this, index] {
            bool ok = true;
            try {
                modules_[index]->module->Refresh();
            } catch (...) {
                ok = false;
            }
            modules_[index]->lastOk = ok;
            if (hwnd_) PostMessageW(hwnd_, kMsgModuleDone, (WPARAM)index, 0);
        });
    } catch (...) {
        rt.busy = false;
        rt.lastOk = false;
        rt.failed = true;
        NotifyDirty();
    }
}

void Board::FinishRefresh(int index) {
    if (index < 0 || index >= (int)modules_.size()) return;
    auto& rt = *modules_[index];
    if (!rt.busy) return;

    if (rt.worker.joinable()) rt.worker.join();
    rt.busy = false;
    if (quitting_) return;

    LayoutPlan plan;
    const bool fits = !rt.state.visible ||
                      (rt.lastOk && PlanLayout(boardRect_, index, index, true, &plan));
    rt.layoutFailed = rt.lastOk && !fits;
    rt.failed = !detail::CommitRefresh(*rt.module, rt.lastOk && fits);
    if (rt.lastOk && !fits) rt.failed = false;
    if (!rt.failed && fits && rt.state.visible && ApplyLayout(plan)) SaveState();
    rt.status = rt.module->Status();

    LayoutCards();
    Render();
    ArmTimer();

    if (rt.again) {
        rt.again = false;
        StartRefresh(index);
    }
}

void Board::RefreshAll() {
    for (int i = 0; i < (int)modules_.size(); ++i) {
        if (modules_[i]->state.visible) StartRefresh(i);
    }
    ArmTimer();
}

void Board::ToggleDetailsAll() {
    bool any = false;
    for (auto& rt : modules_) {
        if (!rt->spec.hasDetails) continue;
        rt->state.expanded = !rt->state.expanded;
        rt->module->SetExpanded(rt->state.expanded);
        any = true;
    }
    if (!any) return;
    LayoutPlan plan;
    board::PlanStatus status = board::PlanStatus::NotFound;
    if (!PlanLayout(boardRect_, -1, -1, true, &plan, &status)) {
        for (auto& rt : modules_) {
            if (!rt->spec.hasDetails) continue;
            rt->state.expanded = !rt->state.expanded;
            rt->module->SetExpanded(rt->state.expanded);
        }
        LayoutError(L"The cards cannot be expanded or collapsed together.",
                    status == board::PlanStatus::Insufficient
                        ? L"The expanded cards need more space than this display's right half provides."
                        : L"No arrangement was found within the layout search limits.");
        return;
    }
    ApplyLayout(plan);
    ClearRestoreSnapshot();
    for (auto& rt : modules_) {
        if (rt->spec.hasDetails) rt->module->OnExpandChanged(rt->state.expanded);
    }
    SaveState();
    LayoutCards();
    Render();
}

void Board::ToggleExpanded(int index) {
    if (index < 0 || index >= (int)modules_.size()) return;
    auto& rt = *modules_[index];
    if (!rt.spec.hasDetails) return;
    rt.state.expanded = !rt.state.expanded;
    rt.module->SetExpanded(rt.state.expanded);
    LayoutPlan plan;
    board::PlanStatus status = board::PlanStatus::NotFound;
    const bool planned = PlanLayout(boardRect_, index, -1, true, &plan, &status);
    if (!planned) {
        rt.state.expanded = !rt.state.expanded;
        rt.module->SetExpanded(rt.state.expanded);
        LayoutError(L"This card cannot be expanded or collapsed here.",
                    status == board::PlanStatus::Insufficient
                        ? L"The expanded card needs more space than this display's right half provides."
                        : L"No arrangement was found within the layout search limits.");
        return;
    }
    ApplyLayout(plan);
    ClearRestoreSnapshot();
    rt.module->OnExpandChanged(rt.state.expanded);
    SaveState();
    LayoutCards();
    Render();
}

void Board::AddModule(int index) {
    if (index < 0 || index >= (int)modules_.size()) return;
    auto& rt = *modules_[index];
    rt.state.visible = true;
    rt.module->SetExpanded(rt.state.expanded);

    LayoutPlan plan;
    board::PlanStatus status = board::PlanStatus::NotFound;
    if (!PlanLayout(boardRect_, -1, -1, true, &plan, &status)) {
        rt.state.visible = false;
        LayoutError(L"This card cannot be added to the current layout.",
                    status == board::PlanStatus::Insufficient
                        ? L"The board has no room left for another card."
                        : L"No arrangement was found within the layout search limits.");
        return;
    }
    ApplyLayout(plan);
    ClearRestoreSnapshot();
    rt.dueAt = 0;

    SaveState();
    LayoutCards();
    Render();
    ArmTimer();
    if (!rt.busy) StartRefresh(index);
    Emit(BoardEventKind::CardShown, board::PlanStatus::Ok, index,
         Rect{rt.state.x, rt.state.y, rt.size.width, rt.size.height});
}

void Board::RemoveModule(int index) {
    if (index < 0 || index >= (int)modules_.size()) return;
    modules_[index]->state.visible = false;
    if (layoutIncomplete_) {
        // Removing a card is one of the repair paths: try to complete the
        // layout again so the remaining cards stop showing NO SPACE.
        LayoutPlan plan;
        if (PlanLayout(boardRect_, -1, -1, true, &plan)) ApplyLayout(plan);
    }
    ClearRestoreSnapshot();
    SaveState();
    LayoutCards();
    Render();
    ArmTimer();
    Emit(BoardEventKind::CardHidden, board::PlanStatus::Ok, index);
}

bool Board::CardsAreClear() const {
    for (std::size_t i = 0; i < visible_.size(); ++i) {
        for (std::size_t j = i + 1; j < visible_.size(); ++j) {
            if (visible_[i].rect.intersects(visible_[j].rect, kCardGap)) return false;
        }
    }
    return true;
}

void Board::FitCards() {
    // Repair path for an incomplete layout. First try to keep everything as it
    // is; only if that cannot be arranged, collapse the card details, which is
    // the least destructive way to reclaim height.
    LayoutPlan plan;
    board::PlanStatus status = board::PlanStatus::NotFound;
    if (PlanLayout(boardRect_, -1, -1, true, &plan, &status)) {
        ApplyLayout(plan);
        ClearRestoreSnapshot();
        SaveState();
        LayoutCards();
        Render();
        return;
    }

    std::vector<bool> wasExpanded;
    wasExpanded.reserve(modules_.size());
    bool anyExpanded = false;
    for (auto& rt : modules_) {
        const bool expanded = rt->spec.hasDetails && rt->state.expanded;
        wasExpanded.push_back(expanded);
        anyExpanded |= expanded;
        if (!rt->spec.hasDetails) continue;
        rt->state.expanded = false;
        rt->module->SetExpanded(false);
    }
    if (anyExpanded && PlanLayout(boardRect_, -1, -1, true, &plan, &status)) {
        ApplyLayout(plan);
        for (auto& rt : modules_) {
            if (rt->spec.hasDetails) rt->module->OnExpandChanged(false);
        }
        ClearRestoreSnapshot();
        SaveState();
        LayoutCards();
        Render();
        SetHint(L"CARDS COLLAPSED TO FIT THE BOARD", kHintMs);
        return;
    }

    // Restore the previous expansion state; nothing was committed.
    for (std::size_t i = 0; i < modules_.size() && i < wasExpanded.size(); ++i) {
        auto& rt = *modules_[i];
        if (!rt.spec.hasDetails) continue;
        rt.state.expanded = wasExpanded[i];
        rt.module->SetExpanded(wasExpanded[i]);
    }
    LayoutError(L"The cards cannot be fitted into this board.",
                status == board::PlanStatus::Insufficient
                    ? L"They need more space than this display's right half provides."
                    : L"No arrangement was found within the layout search limits.");
}

void Board::Tidy() {
    std::vector<POINT> old;
    old.reserve(modules_.size());
    const Rect interior = Interior();
    for (auto& rt : modules_) {
        old.push_back(POINT{rt->state.x, rt->state.y});
        if (rt->state.visible) {
            rt->state.x = interior.x;
            rt->state.y = interior.y;
        }
    }
    LayoutPlan plan;
    board::PlanStatus status = board::PlanStatus::NotFound;
    if (!PlanLayout(boardRect_, -1, -1, true, &plan, &status)) {
        for (std::size_t i = 0; i < modules_.size(); ++i) {
            modules_[i]->state.x = old[i].x;
            modules_[i]->state.y = old[i].y;
        }
        LayoutError(L"The current cards cannot be arranged together.",
                    status == board::PlanStatus::Insufficient
                        ? L"They need more space than this display's right half provides."
                        : L"No arrangement was found within the layout search limits.");
        return;
    }
    ApplyLayout(plan);
    ClearRestoreSnapshot();
    SaveState();
    LayoutCards();
    Render();
}

void Board::SaveState() {
    boardStore_.SetInt(L"board.x", boardRect_.x);
    boardStore_.SetInt(L"board.y", boardRect_.y);
    boardStore_.SetInt(L"board.w", boardRect_.w);
    boardStore_.SetInt(L"board.h", boardRect_.h);
    boardStore_.SetInt(L"board.grid", grid_ ? 1 : 0);
    boardStore_.SetInt(L"board.solid", solid_ ? 1 : 0);

    for (const auto& rt : modules_) {
        const std::wstring prefix = L"card." + rt->spec.id + L".";
        boardStore_.SetInt((prefix + L"x").c_str(), rt->state.x);
        boardStore_.SetInt((prefix + L"y").c_str(), rt->state.y);
        boardStore_.SetInt((prefix + L"visible").c_str(), rt->state.visible ? 1 : 0);
        boardStore_.SetInt((prefix + L"expanded").c_str(), rt->state.expanded ? 1 : 0);
    }

    if (!saveThread_.joinable()) {
        if (!boardStore_.Save()) {
            Log(LogLevel::Error, L"save", L"cannot write the board config");
        }
        return;
    }

    auto snapshot = boardStore_.Snapshot();
    {
        std::lock_guard<std::mutex> lock(saveMu_);
        savePending_ = std::move(snapshot);
        savePendingSet_ = true;
    }
    saveCv_.notify_one();
}

void Board::StartSaveThread() {
    if (saveThread_.joinable()) return;
    try {
        saveThread_ = std::thread([this] { SaveLoop(); });
    } catch (...) {
        saveThread_ = std::thread();
    }
}

void Board::SaveLoop() {
    for (;;) {
        std::map<std::wstring, std::wstring> snapshot;
        {
            std::unique_lock<std::mutex> lock(saveMu_);
            saveCv_.wait(lock, [this] { return saveStop_ || savePendingSet_; });
            if (!savePendingSet_ && saveStop_) break;
            snapshot = std::move(savePending_);
            savePending_.clear();
            savePendingSet_ = false;
        }

        // Write exactly this snapshot instead of merging it into the file on
        // disk: a key removed from the in-memory store (for example a cleared
        // saved.* layout snapshot) must not come back on the next start.
        if (!SaveSnapshotFile(boardStore_.path(), snapshot)) {
            Log(LogLevel::Error, L"save", L"cannot write the board config snapshot");
        }
        // Loop again: a newer snapshot may have been queued while writing
        // (including the final snapshot queued by Shutdown).
    }
}

void Board::StopSaveThread() {
    if (!saveThread_.joinable()) {
        SaveState();
        return;
    }

    SaveState();  // queue the final snapshot
    {
        std::lock_guard<std::mutex> lock(saveMu_);
        saveStop_ = true;
    }
    saveCv_.notify_one();
    saveThread_.join();
}

void Board::PinToDesktop() {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Board::OnForeground(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (g_board && event == EVENT_SYSTEM_FOREGROUND && g_board->hwnd_ && hwnd != g_board->hwnd_) {
        g_board->assistantSummoned_ = false;
        g_board->PinToDesktop();
    }
}

HICON Board::BuildIcon() {
    Canvas icon;
    if (!icon.Resize(16, 16)) return nullptr;
    icon.Clear(0);
    icon.Rect(2, 2, 12, 12, Rgba(0x8A8A96));
    for (int y = 5; y < 12; y += 3) {
        for (int x = 5; x < 13; x += 3) icon.Fill(x, y, 1, 1, Rgba(0x4A4A5C));
    }
    icon.Fill(4, 5, 4, 4, theme_.ok);
    icon.Fill(9, 6, 3, 3, theme_.text);
    return icon.ToIcon();
}

void Board::AddTray() {
    if (!icon_) icon_ = BuildIcon();

    nid_ = {};
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = kMsgTray;
    nid_.hIcon = icon_ ? icon_ : LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(nid_.szTip, options_.title.c_str(), 128);
    tray_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
}

void Board::RemoveTray() {
    if (tray_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        tray_ = false;
    }
}

void Board::Shutdown() {
    if (shutDown_) return;
    shutDown_ = true;
    quitting_ = true;

    if (hwnd_) {
        KillTimer(hwnd_, kTimerId);
        if (!options_.assistantOnlyHotkeys) {
            UnregisterHotKey(hwnd_, kHotkeyRefresh);
            UnregisterHotKey(hwnd_, kHotkeyMenu);
            UnregisterHotKey(hwnd_, kHotkeyDetails);
            UnregisterHotKey(hwnd_, kHotkeyQuit);
            for (int i = 0; i < (int)modules_.size(); ++i) {
                for (const Hotkey& hk : modules_[i]->spec.hotkeys) {
                    if (hk.id > 0 && hk.id < 100) {
                        UnregisterHotKey(hwnd_, kModuleHotkeyBase + i * 100 + hk.id);
                    }
                }
            }
        }
        if (assistantHotkeyRegistered_) {
            UnregisterHotKey(hwnd_, kHotkeyAssistant);
            assistantHotkeyRegistered_ = false;
        }
    }
    RemoveTray();

    if (fgHook_) {
        UnhookWinEvent(fgHook_);
        fgHook_ = nullptr;
    }
    for (auto& rt : modules_) {
        if (rt->worker.joinable()) rt->worker.join();
    }

    StopSaveThread();

    if (icon_) {
        DestroyIcon(icon_);
        icon_ = nullptr;
    }
    if (mutex_) {
        CloseHandle(mutex_);
        mutex_ = nullptr;
    }
    g_board = nullptr;
}

bool Board::ExportTestCanvas(int slot) {
    if (!testCanvasExport_ || configDir_.empty()) return false;
    if (slot < 0 || slot > 1) return false;
    if (!hwnd_ || quitting_) return false;

    // Repaint the board's own DIB first: this hook exists exactly because a
    // layered window may print as blank; it must never read the screen.
    Render();
    if (!canvas_.ready() || canvas_.width() <= 0 || canvas_.height() <= 0) return false;

    const int width = canvas_.width();
    const int height = canvas_.height();
    const DWORD imageBytes = (DWORD)width * (DWORD)height * 4u;

    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;  // "BM"
    file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + imageBytes;

    BITMAPINFOHEADER info{};
    info.biSize = sizeof(BITMAPINFOHEADER);
    info.biWidth = width;
    info.biHeight = -height;  // top-down, matching Canvas::data()
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = imageBytes;

    const std::wstring path =
        configDir_ + L"\\assistant-test-frame-" + std::to_wstring(slot) + L".bmp";
    const std::wstring temporary = path + L".tmp";
    HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        Log(LogLevel::Error, L"canvas", L"cannot create the test BMP");
        return false;
    }

    DWORD written = 0;
    bool ok = WriteFile(handle, &file, sizeof(file), &written, nullptr) != FALSE &&
              written == sizeof(file);
    if (ok) {
        ok = WriteFile(handle, &info, sizeof(info), &written, nullptr) != FALSE &&
             written == sizeof(info);
    }
    if (ok) {
        ok = WriteFile(handle, canvas_.data(), imageBytes, &written, nullptr) != FALSE &&
             written == imageBytes;
    }
    CloseHandle(handle);

    if (!ok) {
        DeleteFileW(temporary.c_str());
        Log(LogLevel::Error, L"canvas", L"cannot write the test BMP");
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        Log(LogLevel::Error, L"canvas", L"cannot publish the test BMP");
        return false;
    }
    return true;
}

void Board::FocusCard(int index) {
    if (index < 0 || index >= (int)modules_.size()) return;
    if (focusCard_ == index) return;
    BlurCard();

    focusCard_ = index;
    caretOn_ = true;
    modules_[index]->module->SetFocusState(true, true);
    modules_[index]->module->OnFocusChanged(true);
    if (hwnd_) SetTimer(hwnd_, kCaretTimerId, kCaretBlinkMs, nullptr);

    // Drop to the desktop layer *first*, then take the keyboard: moving the
    // window afterwards would undo the activation we just asked for.
    if (hwnd_) {
        PinToDesktop();
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
    }
    Render();
}

int Board::AssistantIndex() const {
    for (int i = 0; i < (int)modules_.size(); ++i) {
        if (modules_[i]->spec.id == L"assistant") return i;
    }
    return -1;
}

void Board::SummonAssistant() {
    // Toggle: a second Ctrl+Alt+A while summoned drops the board back to the
    // desktop layer and releases the keyboard.
    if (assistantSummoned_) {
        assistantSummoned_ = false;
        BlurCard();
        PinToDesktop();
        return;
    }

    const int index = AssistantIndex();
    if (index < 0) return;
    if (!modules_[index]->state.visible) AddModule(index);
    if (!modules_[index]->state.expanded) ToggleExpanded(index);

    FocusCard(index);
    // FocusCard drops to HWND_BOTTOM before taking the keyboard (the PS card
    // needs the same); raise again *after* that, so the answer is visible.
    if (hwnd_) {
        SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    assistantSummoned_ = true;
    Render();
}

void Board::BlurCard() {
    if (focusCard_ < 0) return;
    const int index = focusCard_;
    focusCard_ = -1;
    if (hwnd_) KillTimer(hwnd_, kCaretTimerId);
    modules_[index]->module->SetFocusState(false, false);
    modules_[index]->module->OnFocusChanged(false);
    Render();
}

bool Board::FocusedBodyRect(Rect* body) const {
    if (focusCard_ < 0) return false;
    for (const VisibleCard& vc : visible_) {
        if (vc.module != focusCard_) continue;
        if (body) *body = skin::CardBody(vc.rect);
        return true;
    }
    return false;
}

void Board::PasteIntoFocusedCard() {
    if (focusCard_ < 0) return;
    if (!OpenClipboard(hwnd_)) return;

    std::wstring text;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) {
            text = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (text.empty()) return;

    modules_[focusCard_]->module->OnPaste(text);
    Render();
}

void Board::SetClipboardText(std::wstring_view text) {
    if (text.empty()) return;
    if (!OpenClipboard(hwnd_)) return;
    EmptyClipboard();

    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* dst = GlobalLock(block)) {
            memcpy(dst, text.data(), text.size() * sizeof(wchar_t));
            static_cast<wchar_t*>(dst)[text.size()] = L'\0';
            GlobalUnlock(block);
            if (!SetClipboardData(CF_UNICODETEXT, block)) GlobalFree(block);
        } else {
            GlobalFree(block);
        }
    }
    CloseClipboard();
}

void Board::ForwardKey(WPARAM vk) {
    if (focusCard_ < 0) return;

    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    // Paste lives here: a module should not have to open the clipboard itself.
    if (ctrl && !alt && vk == 'V') {
        PasteIntoFocusedCard();
        return;
    }

    const KeyEvent key{(unsigned)vk, ctrl, alt, shift};
    if (modules_[focusCard_]->module->OnKey(key)) Render();
}

void Board::ForwardChar(wchar_t c) {
    if (focusCard_ < 0) return;
    if (c < 32 || c == 127) return;
    modules_[focusCard_]->module->OnChar(c);
    Render();
}

board::Hit Board::HitAt(int x, int y) {
    EnsureAddSlots();
    std::vector<Rect> cards;
    cards.reserve(visible_.size());
    for (const VisibleCard& card : visible_) cards.push_back(card.rect);
    return board::HitTest(x, y, boardRect_.w, boardRect_.h, cards, addSlots_);
}

void Board::UpdateCursor(const board::Hit& hit, int x, int y) {
    (void)y;
    LPCWSTR cursor = IDC_ARROW;
    switch (hit.kind) {
    case board::HitKind::Resize:
        cursor = IDC_SIZENWSE;
        break;
    case board::HitKind::BoardTitle:
        cursor = IDC_SIZEALL;
        break;
    case board::HitKind::AddSlot:
        cursor = IDC_HAND;
        break;
    case board::HitKind::Card:
        cursor = IDC_ARROW;
        break;
    case board::HitKind::CardTitle: {
        const auto& card = visible_[hit.card].rect;
        const auto& rt = *modules_[visible_[hit.card].module];
        if (rt.spec.hasDetails && x >= card.right() - 28) {
            cursor = IDC_HAND;
        } else {
            cursor = IDC_SIZEALL;
        }
        break;
    }
    case board::HitKind::Empty:
    default:
        cursor = IDC_ARROW;
        break;
    }
    SetCursor(LoadCursorW(nullptr, cursor));
}

void Board::ResetGesture() {
    pressKind_ = board::HitKind::Empty;
    pressCard_ = -1;
    pressMoved_ = false;
    pressToggle_ = false;
    dragCard_ = -1;
    draggingBoard_ = false;
    resizing_ = false;
}

void Board::CancelGesture() {
    const int drag = dragCard_;
    const bool hadDrag = drag >= 0 || draggingBoard_ || resizing_;
    if (drag >= 0) {
        // An interrupted card drag must not leave the card where the pointer
        // happened to be: it may sit on another card. Put it back.
        auto& rt = *modules_[drag];
        rt.state.x = pressCardX_;
        rt.state.y = pressCardY_;
    }
    ResetGesture();
    if (hadDrag) {
        SaveState();
        LayoutCards();
        Render();
    }
}

LONG_PTR Board::BeginMenu() {
    HWND previous = GetForegroundWindow();
    previousForeground_ = previous == hwnd_ ? nullptr : previous;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex & ~WS_EX_NOACTIVATE);
    SetForegroundWindow(hwnd_);
    return ex;
}

void Board::EndMenu(LONG_PTR oldExStyle) {
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, oldExStyle);
    if (previousForeground_ && IsWindow(previousForeground_) && previousForeground_ != hwnd_) {
        SetForegroundWindow(previousForeground_);
    }
    previousForeground_ = nullptr;
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    PinToDesktop();
}

void Board::ShowMenu(bool boardMenu, int cardIndex) {
    HMENU menu = CreatePopupMenu();

    if (boardMenu) {
        HMENU add = CreatePopupMenu();
        int count = 0;
        for (int i = 0; i < (int)modules_.size(); ++i) {
            if (modules_[i]->state.visible) continue;
            AppendMenuW(add, MF_STRING, kMenuAddBase + (UINT)i,
                        modules_[i]->spec.title.c_str());
            ++count;
        }
        if (count == 0) AppendMenuW(add, MF_STRING | MF_GRAYED, 0, L"ALL MODULES ADDED");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)add, L"Add module");

        if (AssistantIndex() >= 0) {
            AppendMenuW(menu,
                        MF_STRING | (assistantSummoned_ ? MF_CHECKED : 0),
                        kMenuSummonAssistant, L"Summon assistant");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        AppendMenuW(menu, MF_STRING, kMenuTidy, L"Tidy board");
        AppendMenuW(menu, MF_STRING, kMenuRefreshAll, L"Refresh all");
        AppendMenuW(menu, MF_STRING, kMenuFitCards, L"Fit cards (collapse details)");
        AppendMenuW(menu, MF_STRING, kMenuResetPanel, L"Reset to right standard panel");
        AppendMenuW(menu, restorePending_ ? MF_STRING : (MF_STRING | MF_GRAYED),
                    kMenuRestoreLayout, L"Restore previous layout");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (grid_ ? MF_CHECKED : 0), kMenuGrid, L"Show grid");
        AppendMenuW(menu, MF_STRING | (!solid_ ? MF_CHECKED : 0), kMenuSolid,
                    L"Empty area passes clicks");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu,
                    MF_STRING |
                        (appenv::AutostartEnabled(options_.title) ? MF_CHECKED : 0),
                    kMenuAutostart, L"Start with Windows");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");
    } else if (cardIndex >= 0 && cardIndex < (int)modules_.size()) {
        auto& rt = *modules_[cardIndex];
        const std::vector<MenuItem> items = rt.module->Menu();
        for (const MenuItem& item : items) {
            AppendMenuW(menu, MF_STRING | (item.checked ? MF_CHECKED : 0),
                        kMenuModuleBase + (UINT)item.id, item.label.c_str());
        }
        if (!items.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (rt.spec.hasDetails) {
            AppendMenuW(menu, MF_STRING | (rt.state.expanded ? MF_CHECKED : 0),
                        kMenuCardDetails, L"Details");
        }
        AppendMenuW(menu, MF_STRING, kMenuCardRefresh, L"Refresh");
        AppendMenuW(menu, MF_STRING, kMenuCardRemove, L"Remove card");
    } else {
        DestroyMenu(menu);
        return;
    }

    POINT pt{};
    GetCursorPos(&pt);
    const LONG_PTR oldEx = BeginMenu();
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_,
                                   nullptr);
    DestroyMenu(menu);
    EndMenu(oldEx);

    if (cmd == 0) return;

    if (boardMenu) {
        if (cmd >= (int)kMenuAddBase &&
            cmd < (int)kMenuAddBase + (int)modules_.size()) {
            AddModule(cmd - (int)kMenuAddBase);
            return;
        }
        switch (cmd) {
        case kMenuSummonAssistant:
            SummonAssistant();
            break;
        case kMenuTidy:
            Tidy();
            break;
        case kMenuFitCards:
            FitCards();
            break;
        case kMenuResetPanel:
            AdoptStandardPanel();
            break;
        case kMenuRestoreLayout:
            TryRestoreSavedLayout(true);
            break;
        case kMenuRefreshAll:
            RefreshAll();
            break;
        case kMenuGrid:
            grid_ = !grid_;
            SaveState();
            Render();
            break;
        case kMenuSolid:
            solid_ = !solid_;
            SaveState();
            Render();
            break;
        case kMenuAutostart:
            appenv::SetAutostart(options_.title,
                                 !appenv::AutostartEnabled(options_.title));
            break;
        case kMenuExit:
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        default:
            break;
        }
        return;
    }

    if (cmd >= (int)kMenuModuleBase) {
        modules_[cardIndex]->module->OnMenu(cmd - (int)kMenuModuleBase);
        PinToDesktop();
        StartRefresh(cardIndex);
        Render();
        return;
    }

    switch (cmd) {
    case kMenuCardDetails:
        ToggleExpanded(cardIndex);
        break;
    case kMenuCardRefresh:
        StartRefresh(cardIndex);
        ArmTimer();
        break;
    case kMenuCardRemove:
        RemoveModule(cardIndex);
        break;
    default:
        break;
    }
}

void Board::SaveRestoreSnapshot() {
    std::vector<POINT> positions;
    positions.reserve(modules_.size());
    for (const auto& rt : modules_) positions.push_back(POINT{rt->state.x, rt->state.y});
    SaveRestoreSnapshotOf(boardRect_, positions);
}

void Board::SaveRestoreSnapshotOf(const Rect& board, const std::vector<POINT>& positions) {
    // Kept under saved.* so a temporary display adaptation never destroys the
    // layout the user actually chose.
    boardStore_.SetInt(L"saved.board.x", board.x);
    boardStore_.SetInt(L"saved.board.y", board.y);
    boardStore_.SetInt(L"saved.board.w", board.w);
    boardStore_.SetInt(L"saved.board.h", board.h);
    for (std::size_t i = 0; i < modules_.size() && i < positions.size(); ++i) {
        const std::wstring prefix = L"saved.card." + modules_[i]->spec.id + L".";
        boardStore_.SetInt((prefix + L"x").c_str(), positions[i].x);
        boardStore_.SetInt((prefix + L"y").c_str(), positions[i].y);
    }
    boardStore_.SetInt(L"saved.pending", 1);
    restorePending_ = true;
}

void Board::SnapshotLayoutIfNoPending() {
    // The earliest un-restored layout wins: a second (smaller) display change
    // must not replace the layout the user still wants to come back to.
    if (restorePending_) return;
    SaveRestoreSnapshot();
    Log(LogLevel::Info, L"layout", L"restore snapshot taken before adapting the layout");
    Emit(BoardEventKind::LayoutRestorePending, board::PlanStatus::Ok, -1, boardRect_);
}

void Board::ClearRestoreSnapshot() {
    if (!restorePending_) return;
    boardStore_.RemovePrefix(L"saved.");
    restorePending_ = false;
}

bool Board::TryRestoreSavedLayout(bool notify) {
    if (!restorePending_) return false;
    const RECT work = hwnd_ ? MonitorWork() : TargetWork();
    if (!boardStore_.Has(L"saved.board.w")) {
        ClearRestoreSnapshot();
        return false;
    }

    // The ORIGINAL rectangle, never a clamped one: restoring a shrunken board
    // would look like success and then lose the real layout for good.
    Rect target;
    target.x = (int)boardStore_.GetInt(L"saved.board.x", boardRect_.x);
    target.y = (int)boardStore_.GetInt(L"saved.board.y", boardRect_.y);
    target.w = (int)boardStore_.GetInt(L"saved.board.w", boardRect_.w);
    target.h = (int)boardStore_.GetInt(L"saved.board.h", boardRect_.h);
    const int workW = work.right - work.left;
    const int workH = work.bottom - work.top;
    const bool fitsWorkArea =
        target.w > 0 && target.h > 0 && target.w <= workW && target.h <= workH &&
        target.x >= (int)work.left && target.y >= (int)work.top &&
        (long long)target.x + target.w <= (long long)work.right &&
        (long long)target.y + target.h <= (long long)work.bottom;

    std::vector<POINT> saved;
    std::vector<POINT> current;
    saved.reserve(modules_.size());
    current.reserve(modules_.size());
    for (const auto& rt : modules_) {
        const std::wstring prefix = L"saved.card." + rt->spec.id + L".";
        saved.push_back(POINT{(int)boardStore_.GetInt((prefix + L"x").c_str(), rt->state.x),
                              (int)boardStore_.GetInt((prefix + L"y").c_str(), rt->state.y)});
        current.push_back(POINT{rt->state.x, rt->state.y});
        rt->state.x = saved.back().x;
        rt->state.y = saved.back().y;
    }

    LayoutPlan plan;
    board::PlanStatus status = board::PlanStatus::NotFound;
    bool planned = fitsWorkArea && PlanLayout(target, -1, -1, false, &plan, &status);
    if (planned) {
        // Restoring means the original rectangles come back. If the planner had
        // to move a card (for example because its content grew while the small
        // display was in use), this is not a restoration: keep the snapshot.
        for (std::size_t i = 0; i < plan.indices.size(); ++i) {
            const int index = plan.indices[i];
            if (index < 0 || index >= (int)saved.size()) {
                planned = false;
                break;
            }
            if (plan.cards[i].x != saved[(std::size_t)index].x ||
                plan.cards[i].y != saved[(std::size_t)index].y) {
                planned = false;
                break;
            }
        }
    }
    if (!planned) {
        // Still not this display's size, or not restorable exactly: keep the
        // snapshot and the current (adapted) layout for a later attempt.
        for (std::size_t i = 0; i < modules_.size() && i < current.size(); ++i) {
            modules_[i]->state.x = current[i].x;
            modules_[i]->state.y = current[i].y;
        }
        if (notify) SetHint(L"PREVIOUS LAYOUT DOES NOT FIT THIS DISPLAY YET", kHintMs);
        Log(LogLevel::Info, L"layout", L"saved layout cannot be restored on this display");
        Emit(BoardEventKind::LayoutRestoreFailed, status, -1, boardRect_);
        return false;
    }
    ApplyLayout(plan);
    ClearRestoreSnapshot();
    SaveState();
    LayoutCards();
    Render();
    if (notify) SetHint(L"PREVIOUS LAYOUT RESTORED", kHintMs);
    Log(LogLevel::Info, L"layout", L"saved layout restored");
    Emit(BoardEventKind::LayoutRestored, board::PlanStatus::Ok, -1, boardRect_);
    return true;
}

void Board::AdaptToWorkArea(bool persist) {
    if (!hwnd_) return;
    const RECT work = MonitorWork();

    // A recovered display may be able to show the layout that was temporarily
    // adapted earlier; try that first instead of overwriting it.
    if (restorePending_) {
        TryRestoreSavedLayout(true);
        if (!restorePending_) {
            if (persist) SaveState();
            return;
        }
    }

    Rect target = board::ClampBoard(boardRect_, work, options_.minWidth, options_.minHeight);
    const int maxW = MaxBoardWidth(work);
    if (target.w > maxW) {
        target.w = std::max(options_.minWidth, std::min(target.w, maxW));
        target.x = std::max((int)work.left, (int)work.right - target.w);
    }
    const bool displaced = target.x != boardRect_.x || target.y != boardRect_.y ||
                           target.w != boardRect_.w || target.h != boardRect_.h;

    LayoutPlan plan;
    board::PlanStatus status = board::PlanStatus::NotFound;
    if (!PlanLayout(target, -1, -1, true, &plan, &status)) {
        if (displaced) {
            // Keep the window on screen even if the cards cannot be arranged:
            // the original rectangles stay in saved.* for a later restore. An
            // older un-restored snapshot is never replaced here.
            SnapshotLayoutIfNoPending();
            boardRect_ = target;
            if (hwnd_) {
                SetWindowPos(hwnd_, nullptr, boardRect_.x, boardRect_.y, boardRect_.w,
                             boardRect_.h, SWP_NOZORDER | SWP_NOACTIVATE);
                addSlotsDirty_ = true;
            }
            FallbackLayout(status);
        } else {
            LayoutError(L"The layout does not fit this display.",
                        status == board::PlanStatus::Insufficient
                            ? L"The cards need more space than the current work area provides."
                            : L"No arrangement was found within the layout search limits.");
        }
        if (persist) SaveState();
        LayoutCards();
        Render();
        return;
    }

    if (displaced) SnapshotLayoutIfNoPending();
    ApplyLayout(plan);
    if (displaced) {
        SetHint(L"DISPLAY CHANGED - MENU: RESTORE PREVIOUS LAYOUT", 8000);
        Emit(BoardEventKind::DisplayAdapted, board::PlanStatus::Ok, -1, boardRect_);
    }
    LayoutCards();
    Render();
    if (persist) SaveState();
}

void Board::HandleDpiChanged(const RECT* suggested) {
    if (!suggested) {
        AdaptToWorkArea(true);
        return;
    }
    if (!hwnd_) return;

    // Windows suggests a new rectangle for the new DPI. Everything else -
    // restore arbitration, snapshotting the old layout, planning, saving - is
    // the same flow as a work-area change, so a DPI round trip can bring the
    // previous layout back.
    Rect want{suggested->left, suggested->top, suggested->right - suggested->left,
              suggested->bottom - suggested->top};
    want = board::ClampBoard(want, MonitorWork(), options_.minWidth, options_.minHeight);
    const bool displaced = want.x != boardRect_.x || want.y != boardRect_.y ||
                           want.w != boardRect_.w || want.h != boardRect_.h;
    if (!displaced) return;

    if (restorePending_ && TryRestoreSavedLayout(true)) {
        SaveState();
        return;
    }
    SnapshotLayoutIfNoPending();

    LayoutPlan plan;
    board::PlanStatus status = board::PlanStatus::NotFound;
    if (!PlanLayout(want, -1, -1, true, &plan, &status)) {
        AdaptToWorkArea(true);
        return;
    }
    ApplyLayout(plan);
    SaveState();
    SetHint(L"DISPLAY CHANGED - MENU: RESTORE PREVIOUS LAYOUT", 8000);
    Emit(BoardEventKind::DisplayAdapted, board::PlanStatus::Ok, -1, boardRect_);
    LayoutCards();
    Render();
}

void Board::ReadaptToDisplay() {
    if (!started_) return;
    AdaptToWorkArea(true);
}

void Board::ApplyScriptedSuggestedRect() {
    if (!started_) return;
    // Compatibility shim used by the GUI harness: when the display source is a
    // scripted one, use its latest rect= line as the WM_DPICHANGED rectangle.
    auto scripted = std::dynamic_pointer_cast<ScriptedDisplaySource>(display_);
    RECT suggested{};
    if (scripted && scripted->SuggestedRect(&suggested)) {
        HandleDpiChanged(&suggested);
        return;
    }
    AdaptToWorkArea(true);
}

BoardMetrics Board::Metrics() const {
    BoardMetrics metrics;
    if (!started_) return metrics;
    metrics.board = boardRect_;
    metrics.interior = Interior();
    metrics.work = hwnd_ ? MonitorWork() : TargetWork();
    metrics.dpi = WindowDpi();
    return metrics;
}

bool Board::PreviewPlan(const Rect& board, std::vector<Rect>* cards) const {
    if (!started_ || !cards) return false;
    const Rect interior = InteriorFor(board);
    std::vector<Rect> desired;
    for (const auto& rt : modules_) {
        if (!rt->state.visible) continue;
        const int body = ClampBodyHeight(rt->module->BodyHeight(),
                                         rt->module->MinBodyHeight(),
                                         rt->module->MaxBodyHeight());
        const CardSize size =
            MeasureCardSize(rt->spec.sizePreset, rt->spec.width, body, interior.w);
        if (!size.representable) return false;
        desired.push_back(Rect{rt->state.x, rt->state.y, size.width, size.height});
    }
    std::vector<Rect> planned;
    if (!board::TryPlanLayout(desired, {}, interior, kCardGap, options_.grid, &planned)) {
        return false;
    }
    *cards = std::move(planned);
    return true;
}

void Board::Emit(BoardEventKind kind, board::PlanStatus status, int card, const Rect& rect,
                 const std::wstring& detail) {
    if (!options_.onEvent) return;
    if (emittingEvent_) {
        Log(LogLevel::Warn, L"board", L"nested BoardEvent dropped");
        return;
    }
    BoardEvent event;
    event.kind = kind;
    event.status = status;
    event.card = card;
    event.rect = rect;
    event.detail = detail;
    emittingEvent_ = true;
    try {
        options_.onEvent(event);
    } catch (...) {
        Log(LogLevel::Error, L"board", L"BoardEvent callback threw; continuing");
    }
    emittingEvent_ = false;
}

void Board::DispatchBodySizes() {
    for (std::size_t i = 0; i < modules_.size(); ++i) {
        auto& rt = *modules_[i];
        if (!rt.state.visible) {
            rt.hasBody_ = false;
            continue;
        }
        const Rect card{rt.state.x, rt.state.y, rt.size.width, rt.size.height};
        const Rect body = skin::CardBody(card);

        bool exceeded = false;
        ClampBodyHeight(rt.module->BodyHeight(), rt.module->MinBodyHeight(),
                        rt.module->MaxBodyHeight(), &exceeded);
        if (exceeded != rt.lastSizeExceeded_) {
            rt.lastSizeExceeded_ = exceeded;
            if (exceeded) {
                Log(LogLevel::Warn, L"layout", L"body height clamped to MaxBodyHeight");
                Emit(BoardEventKind::SizeRejected, board::PlanStatus::Ok, (int)i, body);
            }
        }

        if (rt.hasBody_ && rt.lastBody_.x == body.x && rt.lastBody_.y == body.y &&
            rt.lastBody_.w == body.w && rt.lastBody_.h == body.h) {
            continue;
        }
        rt.lastBody_ = body;
        rt.hasBody_ = true;
        try {
            rt.module->OnBodySizeChanged(body);
        } catch (...) {
            Log(LogLevel::Error, L"module", L"OnBodySizeChanged threw");
        }
    }
}

bool Board::ShowCard(int index, bool visible) {
    if (!started_ || index < 0 || index >= (int)modules_.size()) return false;
    if (modules_[index]->state.visible == visible) return true;
    if (visible) {
        AddModule(index);
    } else {
        RemoveModule(index);
    }
    return modules_[index]->state.visible == visible;
}

bool Board::SetCardExpanded(int index, bool expanded) {
    if (!started_ || index < 0 || index >= (int)modules_.size()) return false;
    if (!modules_[index]->spec.hasDetails) return false;
    if (modules_[index]->state.expanded == expanded) return true;
    ToggleExpanded(index);
    return modules_[index]->state.expanded == expanded;
}

bool Board::RefreshCardAt(int index) {
    if (!started_ || index < 0 || index >= (int)modules_.size()) return false;
    if (!modules_[index]->state.visible) return false;
    StartRefresh(index);
    ArmTimer();
    return true;
}

bool Board::ResetStandardPanel() {
    if (!started_) return false;
    return AdoptStandardPanel();
}

bool Board::RestorePreviousLayout() {
    if (!started_) return false;
    return TryRestoreSavedLayout(true);
}

LRESULT CALLBACK Board::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Board* host = g_board;
    if (!host) return DefWindowProcW(hwnd, msg, wp, lp);

    if (host->taskbarCreated_ && msg == host->taskbarCreated_) {
        if (host->tray_) Shell_NotifyIconW(NIM_DELETE, &host->nid_);
        if (host->options_.enableTray) host->AddTray();
        return 0;
    }

    switch (msg) {
    case kMsgTray:
        if (LOWORD(lp) == WM_RBUTTONUP) host->ShowMenu(true, -1);
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK) host->ToggleDetailsAll();
        return 0;

    case kMsgModuleDone:
        host->FinishRefresh((int)wp);
        return 0;

    case kMsgModuleRefresh:
        if (!host->quitting_) host->StartRefresh((int)wp);
        return 0;

    case kMsgTestCanvasBmp:
        return host->ExportTestCanvas((int)wp) ? 1 : 0;

    case kMsgDirty:
        if (!host->quitting_) host->Render();
        return 0;

    case kMsgTestDisplay:
        // Compatibility shim: the harness rewrites the scripted display file and
        // posts this message; the programmatic API is BoardHost::ReadaptToDisplay.
        host->ReadaptToDisplay();
        return 0;

    case kMsgTestDpi:
        host->ApplyScriptedSuggestedRect();
        return 0;

    case WM_TIMER:
        if (wp == kTimerId) {
            KillTimer(hwnd, kTimerId);
            const ULONGLONG now = GetTickCount64();
            for (int i = 0; i < (int)host->modules_.size(); ++i) {
                auto& rt = *host->modules_[i];
                if (!rt.state.visible || rt.spec.refreshSeconds <= 0) continue;
                if (rt.dueAt == 0 || rt.dueAt <= now) host->StartRefresh(i);
            }
            host->ArmTimer();
        } else if (wp == kCaretTimerId) {
            host->caretOn_ = !host->caretOn_;
            if (host->focusCard_ >= 0) {
                host->modules_[host->focusCard_]->module->SetFocusState(true, host->caretOn_);
            }
            host->Render();
        }
        return 0;

    case WM_HOTKEY:
        if (wp == (WPARAM)kHotkeyRefresh) {
            host->RefreshAll();
        } else if (wp == (WPARAM)kHotkeyMenu) {
            host->ShowMenu(true, -1);
        } else if (wp == (WPARAM)kHotkeyDetails) {
            host->ToggleDetailsAll();
        } else if (wp == (WPARAM)kHotkeyQuit) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if (wp == (WPARAM)kHotkeyAssistant) {
            host->SummonAssistant();
        } else {
            const int slot = (int)wp - kModuleHotkeyBase;
            if (slot >= 0) {
                const int index = slot / 100;
                const int local = slot % 100;
                if (index >= 0 && index < (int)host->modules_.size()) {
                    host->modules_[index]->module->OnHotkey(local);
                    host->Render();
                }
            }
        }
        return 0;

    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        const board::Hit hit = host->HitAt(pt.x, pt.y);
        if (hit.kind == board::HitKind::Empty && !host->solid_) return HTTRANSPARENT;
        return HTCLIENT;
    }

    case WM_MOUSEACTIVATE: {
        // Only a click the module claims for the keyboard may activate the
        // board; everything else keeps the "clicking a card steals nothing"
        // behaviour it had while the window was WS_EX_NOACTIVATE.
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        const board::Hit hit = host->HitAt(pt.x, pt.y);
        if (hit.kind == board::HitKind::Card && hit.card >= 0) {
            const auto& vc = host->visible_[hit.card];
            const Rect body = skin::CardBody(vc.rect);

            if (host->modules_[vc.module]->module->WantsKeyboard(pt.x - body.x,
                                                                 pt.y - body.y)) {
                return MA_ACTIVATE;
            }
        }
        return MA_NOACTIVATE;
    }

    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        const board::Hit hit = host->HitAt(x, y);

        if (hit.kind == board::HitKind::Empty) return 0;

        // Focus follows the click: a card that wants the keyboard takes it,
        // anything else drops it.
        if (hit.kind == board::HitKind::Card || hit.kind == board::HitKind::CardTitle) {
            const auto& vc = host->visible_[hit.card];
            bool wants = false;
            if (hit.kind == board::HitKind::Card) {
                const Rect body = skin::CardBody(vc.rect);
                wants = host->modules_[vc.module]->module->WantsKeyboard(x - body.x,
                                                                         y - body.y);
            }
            if (wants) {
                if (host->focusCard_ != vc.module) host->FocusCard(vc.module);
            } else {
                host->BlurCard();
            }
        } else {
            host->BlurCard();
        }

        host->pressKind_ = hit.kind;
        host->pressCard_ = hit.kind == board::HitKind::CardTitle ||
                                   hit.kind == board::HitKind::Card
                               ? hit.card
                               : -1;
        host->pressScreen_ = POINT{x + host->boardRect_.x, y + host->boardRect_.y};
        host->pressMoved_ = false;
        host->pressToggle_ = false;
        host->dragCard_ = -1;
        host->draggingBoard_ = false;
        host->resizing_ = false;

        if (hit.kind == board::HitKind::CardTitle || hit.kind == board::HitKind::Card) {
            const auto& vc = host->visible_[hit.card];
            auto& rt = *host->modules_[vc.module];
            if (hit.kind == board::HitKind::CardTitle) {
                const bool toggleZone =
                    rt.spec.hasDetails && x >= vc.rect.right() - 28;
                if (!toggleZone) {
                    host->dragCard_ = vc.module;
                    host->pressCardX_ = rt.state.x;
                    host->pressCardY_ = rt.state.y;
                } else {
                    host->pressToggle_ = true;
                }
            } else {
                // Card body: let the module start whatever gesture it owns
                // (the console anchors a text selection here).
                const Rect body = skin::CardBody(vc.rect);
                rt.module->OnPress(x - body.x, y - body.y);
            }
        } else if (hit.kind == board::HitKind::BoardTitle) {
            host->draggingBoard_ = true;
            host->pressBoard_ = POINT{host->boardRect_.x, host->boardRect_.y};
        } else if (hit.kind == board::HitKind::Resize) {
            host->resizing_ = true;
            host->resizeRejected_ = false;
            host->pressW_ = host->boardRect_.w;
            host->pressH_ = host->boardRect_.h;
        }

        SetCapture(hwnd);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        // The wheel follows the cursor; the card under it decides what to do.
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        const board::Hit hit = host->HitAt(pt.x, pt.y);
        if ((hit.kind == board::HitKind::Card || hit.kind == board::HitKind::CardTitle) &&
            hit.card >= 0) {
            const auto& vc = host->visible_[hit.card];
            const Rect body = skin::CardBody(vc.rect);
            // Preserve fractional wheel detents for precision wheels and
            // touchpads; the receiving module accumulates the raw deltas.
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            host->modules_[vc.module]->module->OnWheel(pt.x - body.x, pt.y - body.y, delta);
            return 0;
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);

        if (host->pressKind_ != board::HitKind::Empty && GetCapture() == hwnd) {
            // Use the screen-space delta. For the board itself the window moves
            // under the cursor, so client deltas feed back into the drag and
            // make the board oscillate.
            const int sx = x + host->boardRect_.x;
            const int sy = y + host->boardRect_.y;
            const int dx = sx - host->pressScreen_.x;
            const int dy = sy - host->pressScreen_.y;
            if (!host->pressMoved_ &&
                (dx > kClickSlop || dx < -kClickSlop || dy > kClickSlop ||
                 dy < -kClickSlop)) {
                host->pressMoved_ = true;
            }

            if (host->pressMoved_) {
                if (host->pressKind_ == board::HitKind::Card && host->pressCard_ >= 0) {
                    // A drag inside a card body is the card's business: the
                    // console uses it to extend a text selection.
                    const auto& vc = host->visible_[host->pressCard_];
                    const Rect body = skin::CardBody(vc.rect);
                    host->modules_[vc.module]->module->OnDrag(x - body.x, y - body.y);
                } else if (host->dragCard_ >= 0) {
                    // During the gesture the card follows the pointer clamped
                    // into the board. The legal-position search runs once, on
                    // release, so dragging never solves the layout per frame.
                    auto& rt = *host->modules_[host->dragCard_];
                    const Rect interior = host->Interior();
                    const int grid = host->options_.grid;
                    const auto place = [&](int value, int origin, int extent, int limit) {
                        int snapped = origin + board::Snap(value - origin, grid);
                        const int highest = std::max(origin, limit - extent);
                        return std::max(origin, std::min(snapped, highest));
                    };
                    const int nx = place(host->pressCardX_ + dx, interior.x, rt.size.width,
                                         interior.right());
                    const int ny = place(host->pressCardY_ + dy, interior.y, rt.size.height,
                                         interior.bottom());
                    if (nx != rt.state.x || ny != rt.state.y) {
                        rt.state.x = nx;
                        rt.state.y = ny;
                        host->LayoutCards();
                        host->RenderThrottled();
                    }
                } else if (host->draggingBoard_) {
                    const RECT work = host->MonitorWork();
                    Rect want{host->pressBoard_.x + dx, host->pressBoard_.y + dy,
                              host->boardRect_.w, host->boardRect_.h};
                    const Rect r = board::ClampBoard(want, work, host->options_.minWidth,
                                                     host->options_.minHeight);
                    if (r.x != host->boardRect_.x || r.y != host->boardRect_.y) {
                        host->boardRect_.x = r.x;
                        host->boardRect_.y = r.y;
                        SetWindowPos(hwnd, nullptr, r.x, r.y, 0, 0,
                                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    }
                } else if (host->resizing_) {
                    const RECT work = host->MonitorWork();
                    int nw = std::max(host->options_.minWidth, host->pressW_ + dx);
                    int nh = std::max(host->options_.minHeight, host->pressH_ + dy);
                    if (host->boardRect_.x + nw > work.right)
                        nw = work.right - host->boardRect_.x;
                    if (host->boardRect_.y + nh > work.bottom)
                        nh = work.bottom - host->boardRect_.y;
                    if (nw != host->boardRect_.w || nh != host->boardRect_.h) {
                        Rect candidate = host->boardRect_;
                        candidate.w = nw;
                        candidate.h = nh;
                        LayoutPlan plan;
                        if (host->PlanLayout(candidate, -1, -1, false, &plan)) {
                            host->resizeRejected_ = false;
                            host->ApplyLayout(plan);
                            host->LayoutCards();
                            host->RenderThrottled();
                        } else {
                            host->resizeRejected_ = true;
                        }
                    } else {
                        host->resizeRejected_ = false;
                    }
                }
            }
            return 0;
        }

        const board::Hit hit = host->HitAt(x, y);
        const int hovered =
            (hit.kind == board::HitKind::Card || hit.kind == board::HitKind::CardTitle) &&
                    hit.card >= 0
                ? host->visible_[hit.card].module
                : -1;
        if (hovered != host->hoverCard_) {
            host->hoverCard_ = hovered;
            host->Render();
        }
        host->UpdateCursor(hit, x, y);
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (host->pressKind_ == board::HitKind::Empty) return 0;

        const board::HitKind kind = host->pressKind_;
        const int card = host->pressCard_;
        const bool moved = host->pressMoved_;
        const bool toggle = host->pressToggle_;
        const int drag = host->dragCard_;
        const int dragFromX = host->pressCardX_;
        const int dragFromY = host->pressCardY_;
        const bool boardDrag = host->draggingBoard_;
        const bool resize = host->resizing_;
        const bool resizeRejected = host->resizeRejected_;
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);

        host->ResetGesture();
        ReleaseCapture();

        if (!moved) {
            if (kind == board::HitKind::CardTitle && toggle && card >= 0) {
                host->ToggleExpanded(host->visible_[card].module);
            } else if (kind == board::HitKind::Card && card >= 0) {
                const auto& vc = host->visible_[card];
                const Rect body = skin::CardBody(vc.rect);
                host->modules_[vc.module]->module->OnClick(x - body.x, y - body.y);
            } else if (kind == board::HitKind::AddSlot) {
                host->ShowMenu(true, -1);
            }
            return 0;
        }

        if (drag >= 0) {
            // Release snaps to the nearest legal slot for the release point; if
            // this board has no legal slot left, the card goes back to where it
            // was picked up and the footer says why.
            auto& rt = *host->modules_[drag];
            std::vector<Rect> occupied;
            occupied.reserve(host->visible_.size());
            for (const auto& card : host->visible_) {
                if (card.module != drag) occupied.push_back(card.rect);
            }
            POINT point{};
            const bool placed = board::TryResolveDrop(
                rt.state.x, rt.state.y, rt.size.width, rt.size.height, occupied,
                host->Interior(), kCardGap, host->options_.grid, &point);
            if (placed) {
                const bool movedCard = point.x != rt.state.x || point.y != rt.state.y;
                rt.state.x = point.x;
                rt.state.y = point.y;
                if (movedCard || point.x != dragFromX || point.y != dragFromY) {
                    host->ClearRestoreSnapshot();
                }
                host->SaveState();
                host->LayoutCards();
                if (host->layoutIncomplete_ && host->CardsAreClear()) {
                    host->layoutIncomplete_ = false;
                    for (auto& module : host->modules_) module->layoutFailed = false;
                }
                host->SetHint(host->layoutIncomplete_
                                  ? L"LAYOUT INCOMPLETE - RIGHT CLICK: FIT CARDS / RESET PANEL"
                                  : L"",
                              0);
            } else {
                rt.state.x = dragFromX;
                rt.state.y = dragFromY;
                host->SaveState();
                host->LayoutCards();
                host->SetHint(L"NO LEGAL POSITION FOR THIS CARD - IT STAYED WHERE IT WAS",
                              kHintMs);
            }
        } else if (boardDrag || resize) {
            host->AdaptToWorkArea(true);
            host->SaveState();
            host->LayoutCards();
            host->Render();
            host->PinToDesktop();
            if (resize && resizeRejected)
                host->LayoutError(L"The board cannot be made smaller with these cards.");
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (host->pressKind_ != board::HitKind::Empty && (HWND)lp != hwnd) {
            host->CancelGesture();
        }
        return 0;

    case WM_RBUTTONUP: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        const board::Hit hit = host->HitAt(x, y);
        if ((hit.kind == board::HitKind::Card || hit.kind == board::HitKind::CardTitle) &&
            hit.card >= 0) {
            host->ShowMenu(false, host->visible_[hit.card].module);
        } else {
            host->ShowMenu(true, -1);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (host->hoverCard_ != -1) {
            host->hoverCard_ = -1;
            host->Render();
        }
        return 0;

    case WM_KILLFOCUS:

        // Focus left the board (another app, a menu): stop owning the keyboard.
        host->assistantSummoned_ = false;
        host->BlurCard();
        host->PinToDesktop();
        return 0;

    case WM_ACTIVATE:

        // Activation must not lift the board out of the desktop layer.
        if (LOWORD(wp) != WA_INACTIVE) host->PinToDesktop();
        return 0;


    case WM_KEYDOWN:
        if (host->focusCard_ >= 0) {
            host->ForwardKey(wp);
            return 0;
        }
        break;

    case WM_CHAR:
        if (host->focusCard_ >= 0) {
            host->ForwardChar((wchar_t)wp);
            return 0;
        }
        break;

    case WM_IME_CHAR:
        if (host->focusCard_ >= 0) {
            host->ForwardChar((wchar_t)wp);
            return 0;
        }
        break;

    case WM_IME_STARTCOMPOSITION: {
        if (host->focusCard_ < 0) return 0;
        int cx = 0;
        int cy = 0;
        Rect body;
        if (host->modules_[host->focusCard_]->module->CaretPos(&cx, &cy) &&
            host->FocusedBodyRect(&body)) {
            if (HIMC himc = ImmGetContext(hwnd)) {
                COMPOSITIONFORM cf{};
                cf.dwStyle = CFS_POINT;
                cf.ptCurrentPos.x = body.x + cx;
                cf.ptCurrentPos.y = body.y + cy;
                ImmSetCompositionWindow(himc, &cf);
                ImmReleaseContext(hwnd, himc);
            }
        }
        return 0;
    }

    case WM_DPICHANGED:
        host->HandleDpiChanged((const RECT*)lp);
        return 0;

    case WM_DISPLAYCHANGE:
        host->AdaptToWorkArea(true);
        return 0;

    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA) host->AdaptToWorkArea(true);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_CLOSE:
        host->quitting_ = true;
        host->RemoveTray();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct BoardHost::Impl {
    Board board;
    std::vector<std::unique_ptr<Module>> modules;
    BoardOptions options;
    bool started = false;
};

BoardHost::BoardHost(std::vector<std::unique_ptr<Module>> modules, BoardOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->modules = std::move(modules);
    impl_->options = std::move(options);
}

BoardHost::~BoardHost() {
    if (!impl_) return;
    if (impl_->started) {
        impl_->board.Finish();
        impl_->started = false;
    }
}

BoardHost::BoardHost(BoardHost&&) noexcept = default;
BoardHost& BoardHost::operator=(BoardHost&&) noexcept = default;

BoardHost::StartResult BoardHost::Start() {
    if (!impl_) return StartResult::Failed;
    if (impl_->started) return StartResult::AlreadyRunning;
    const StartResult result = impl_->board.Start(std::move(impl_->modules), impl_->options);
    impl_->started = result == StartResult::Started;
    return result;
}

void BoardHost::Run() {
    if (!impl_ || !impl_->started) return;
    impl_->board.Run();
    impl_->started = false;
}

void BoardHost::Quit() {
    if (impl_ && impl_->started) impl_->board.Quit();
}

bool BoardHost::running() const {
    return impl_ && impl_->started && impl_->board.started();
}

HWND BoardHost::hwnd() const { return impl_ ? impl_->board.Window() : nullptr; }

BoardMetrics BoardHost::Metrics() const {
    return impl_ ? impl_->board.Metrics() : BoardMetrics{};
}

bool BoardHost::PlanPreview(const Rect& board, std::vector<Rect>* cards) const {
    if (!impl_ || !impl_->started) return false;
    return impl_->board.PreviewPlan(board, cards);
}

void BoardHost::ReadaptToDisplay() {
    if (impl_) impl_->board.ReadaptToDisplay();
}

void BoardHost::ApplySuggestedRect(const RECT& suggested) {
    if (impl_) impl_->board.HandleDpiChanged(&suggested);
}

bool BoardHost::ShowCard(int index, bool visible) {
    if (!impl_ || !impl_->started) return false;
    return impl_->board.ShowCard(index, visible);
}

bool BoardHost::SetCardExpanded(int index, bool expanded) {
    if (!impl_ || !impl_->started) return false;
    return impl_->board.SetCardExpanded(index, expanded);
}

bool BoardHost::RefreshCard(int index) {
    if (!impl_ || !impl_->started) return false;
    return impl_->board.RefreshCardAt(index);
}

void BoardHost::Tidy() {
    if (impl_ && impl_->started) impl_->board.Tidy();
}

bool BoardHost::ResetToStandardPanel() {
    if (!impl_ || !impl_->started) return false;
    return impl_->board.ResetStandardPanel();
}

bool BoardHost::RestorePreviousLayout() {
    if (!impl_ || !impl_->started) return false;
    return impl_->board.RestorePreviousLayout();
}

int RunBoard(std::vector<std::unique_ptr<Module>> modules, BoardOptions options) {
    BoardHost host(std::move(modules), std::move(options));
    const BoardHost::StartResult result = host.Start();
    if (result == BoardHost::StartResult::Failed) return 1;
    if (result == BoardHost::StartResult::Started) host.Run();
    return 0;
}

} // namespace wg
