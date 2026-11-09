// Programmatic host for the desktop board.
//
// RunBoard() remains the one-line entry point. BoardHost exposes the same board
// as a lifecycle object so a host application (or a test) can start it without
// entering the message loop, drive it, inspect it, and shut it down.
#pragma once

#include "widget/board.h"

#include <memory>

namespace wg {

struct BoardMetrics {
    Rect board;      // board window rectangle, screen pixels
    Rect interior;   // card area, board-local pixels
    RECT work{};     // current monitor work area, screen pixels
    int dpi = 96;
};

class BoardHost {
public:
    enum class StartResult {
        Started,         // window created, first layout committed
        AlreadyRunning,  // another board with the same id owns the mutex
        Failed,          // configuration/window creation failed
    };

    explicit BoardHost(std::vector<std::unique_ptr<Module>> modules,
                       BoardOptions options = {});
    ~BoardHost();

    BoardHost(BoardHost&&) noexcept;
    BoardHost& operator=(BoardHost&&) noexcept;
    BoardHost(const BoardHost&) = delete;
    BoardHost& operator=(const BoardHost&) = delete;

    // Creates the window, restores state, runs the first layout, installs the
    // tray/hotkeys. Does not enter a message loop. Calling it twice returns
    // AlreadyRunning. BoardEvent callbacks may fire during Start().
    StartResult Start();
    // Runs the message loop until Quit()/WM_QUIT. Returns immediately when the
    // host was not started.
    void Run();
    // Graceful shutdown: save state, remove the tray, unregister hotkeys, join
    // workers. Safe before Start() and idempotent.
    void Quit();

    bool running() const;
    HWND hwnd() const;
    // Zeroed metrics before Start().
    BoardMetrics Metrics() const;

    // Plans every visible card for the given board rectangle without changing
    // any state. The rect is in screen coordinates (only w/h are used); *cards
    // receives board-local rectangles in registration order. On failure cards
    // is left untouched and false is returned.
    bool PlanPreview(const Rect& board, std::vector<Rect>* cards) const;

    // Same work-area path as WM_DISPLAYCHANGE / SPI_SETWORKAREA.
    void ReadaptToDisplay();
    // Same path as WM_DPICHANGED with the given suggested rectangle.
    void ApplySuggestedRect(const RECT& suggested);

    // Programmatic card operations (the menu equivalents). Each returns false
    // when the index is invalid, the host is not started, or the operation was
    // rejected by the layout planner (the previous state is then kept).
    bool ShowCard(int index, bool visible);
    bool SetCardExpanded(int index, bool expanded);
    bool RefreshCard(int index);
    void Tidy();
    bool ResetToStandardPanel();
    bool RestorePreviousLayout();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wg
