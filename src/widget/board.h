// wg::RunBoard / wg::BoardHost - a desktop pegboard that hosts module cards.
//
// One board window, one DIB, one timer, one tray icon. Modules are rectangles
// inside the board; the board owns card chrome, dragging, snapping, menus,
// persistence and the refresh workers.
//
// RunBoard() is the simple entry point. BoardHost (widget/host.h) exposes the
// same board programmatically: Start()/Run()/Quit(), metrics, layout preview,
// display re-adaptation and card operations, plus a BoardEvent stream for
// applications and tests.
#pragma once

#include "widget/board_plan.h"
#include "widget/module.h"

#include <functional>
#include <memory>
#include <vector>

namespace wg {

class DisplaySource;

// What happened on the board. Delivered on the UI thread through
// BoardOptions::onEvent; see docs/framework.md for the exact order.
enum class BoardEventKind {
    LayoutApplied,         // a layout plan was committed
    LayoutIncomplete,      // not every card fits; the board still runs (NO SPACE)
    LayoutRestorePending,  // a restore snapshot was just taken
    LayoutRestored,        // the saved layout was restored exactly
    LayoutRestoreFailed,   // restoring is still not possible; snapshot kept
    DisplayAdapted,        // work area / DPI change re-planned the layout
    MigrationApplied,      // one-time config migration succeeded
    MigrationDeferred,     // migration could not be applied; retried later
    CardShown,
    CardHidden,
    SizeRejected,          // body height was clamped down to MaxBodyHeight()
};

struct BoardEvent {
    BoardEventKind kind = BoardEventKind::LayoutApplied;
    board::PlanStatus status = board::PlanStatus::Ok;
    int card = -1;       // registration index for card events, else -1
    Rect rect{};         // board rect or card rect (screen coords), when relevant
    std::wstring detail; // optional human-readable note (logs/assertions only)
};

struct BoardOptions {
    std::wstring id = L"widget-board";
    std::wstring title = L"Widget Board";
    std::wstring configFile = L"board.ini";
    std::wstring boardTitle = L"WIDGET BOARD";
    std::wstring boardHint = L"RIGHT CLICK FOR MODULES";
    // First-run frame size, measured at 96 DPI and scaled by the target
    // monitor's DPI. The board is placed flush against the right edge of the
    // work area and is never auto-grown past the right half of that work area
    // (or past width/height). Later runs read the saved rectangle instead.
    int width = 1280;
    int height = 1310;
    int minWidth = 400;
    int minHeight = 420;
    int grid = 8;             // card snap
    int dotGrid = 16;         // visual dot spacing; clamped to 2..256 px
    bool startWithWindows = false;
    // Optional legacy Run values to migrate on the first run of this app.
    // The framework never removes another app's startup entry by default.
    std::vector<std::wstring> legacyAutostartNames;
    UINT refreshHotkeyVk = 'R';
    UINT menuHotkeyVk = 'W';
    UINT detailsHotkeyVk = 'D';
    UINT quitHotkeyVk = 'Q';
    // Summon/hide hotkey. It is only registered when a module whose spec.id is
    // "assistant" was registered, so SDK consumers without the assistant never
    // take the chord away from another application.
    UINT assistantHotkeyModifiers = MOD_CONTROL | MOD_ALT;
    UINT assistantHotkeyVk = 'A';   // Ctrl+Alt+A by default

    // ---- added for programmatic embedding (all optional) -------------------
    // Called on the UI thread whenever the board reaches a notable state. It
    // must not throw and must not call Start/Quit/ReadaptToDisplay from inside
    // the callback (nested events are dropped).
    std::function<void(const BoardEvent&)> onEvent;
    // Work-area/DPI provider; null uses the system desktop. A test may inject a
    // ScriptedDisplaySource (see widget/display.h).
    std::shared_ptr<DisplaySource> display;
    // Turn the tray icon / global hotkeys off (embedding, tests).
    bool enableTray = true;
    bool enableHotkeys = true;
    // Test-only gate: register the assistant summon chord and nothing else.
    // The application only sets this for an explicit isolated physical-hotkey
    // probe; normal test instances run with enableHotkeys=false.
    bool assistantOnlyHotkeys = false;
};

int RunBoard(std::vector<std::unique_ptr<Module>> modules, BoardOptions options = {});

} // namespace wg
