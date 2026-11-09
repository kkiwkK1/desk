// The terminal: a pseudo console, a VT parser and the screen they feed.
//
// One reader thread decodes the PTY byte stream into the screen under a mutex;
// the UI thread only ever takes a snapshot of the visible window. That is the
// whole concurrency story - the parser owns the screen, the card owns the pixels.
#pragma once

#include "data/ps/screen.h"
#include "data/ps/selection.h"
#include "data/ps/vt.h"

#include "widget/canvas.h"
#include "widget/conpty.h"
#include "widget/store.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ps {

// Geometry the card is built around: 80 columns, a short and a tall card.
constexpr int kCols = 80;
constexpr int kRowsCollapsed = 8;
constexpr int kRowsExpanded = 24;

struct ViewRow {
    unsigned long long id = 0;
    std::vector<Cell> cells;
};

// Everything the view needs, copied out in one lock.
struct View {
    int cols = 0;
    int rows = 0;
    std::vector<ViewRow> lines;   // visible window, top to bottom
    int cursorRow = 0;
    int cursorCol = 0;
    bool cursorVisible = false;
    bool alternate = false;
    bool bracketedPaste = false;
    bool mouseTracking = false;
    bool sgrMouse = false;
    int scrollOffset = 0;      // lines scrolled back from the bottom
    int scrollbackLines = 0;
};

// Keep the same history row in view as new rows enter the bounded scrollback.
int AnchorScrollOffset(int offset, unsigned long long newRows, int historyLines);

class Terminal {
public:
    Terminal() = default;
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    // Called from the reader thread when new output landed (coalesced).
    void SetOnOutput(std::function<void()> fn);

    bool Start(const wg::Store& store, int cols, int rows);
    void Stop();
    bool running() const;

    void Write(std::string_view bytes);
    void Resize(int cols, int rows);

    void ScrollBy(int lines);
    void ScrollToBottom();
    int scrollOffset() const;

    View Snapshot() const;
    std::wstring Text(const Selection& sel) const;
    std::vector<Row> RowsById() const;   // scrollback + screen, oldest first
    void ClearScrollback();

    std::wstring label() const { return label_; }
    std::wstring path() const { return path_; }
    std::wstring error() const;
    int cols() const;
    int rows() const;
    bool appCursorKeys() const;    // DECCKM, for arrow-key encoding
    unsigned long long bytesRead() const { return bytesRead_.load(); }
    DWORD exitCode() const;
    DWORD processId() const;
    bool bracketedPaste() const;   // DECSET 2004

private:
    void ReadLoop();
    void Notify();

    wg::Pty pty_;
    mutable std::mutex mu_;
    Screen screen_;
    Parser parser_;
    int scrollOffset_ = 0;

    std::wstring path_;
    std::wstring label_;
    std::wstring error_;
    std::atomic<bool> running_{false};
    std::atomic<unsigned long long> bytesRead_{0};
    std::thread reader_;

    std::mutex cbMu_;
    std::function<void()> onOutput_;
    std::atomic<unsigned long long> lastNotify_{0};
};

} // namespace ps
