#include "data/ps/terminal.h"

#include "data/ps/shell.h"
#include "data/ps/text.h"

#include <algorithm>
#include <string>
#include <vector>

namespace ps {
namespace {

constexpr unsigned long long kNotifyIntervalMs = 33;

std::wstring Env(const wchar_t* name) {
    wchar_t value[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(name, value, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return std::wstring();
    return value;
}

// "<config dir>\ps-history.txt", derived from the module's own ini path.
std::wstring HistoryPath(const wg::Store& store) {
    const std::wstring ini = store.path();
    if (ini.empty()) return std::wstring();
    const std::size_t slash = ini.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    return ini.substr(0, slash + 1) + L"ps-history.txt";
}

} // namespace

int AnchorScrollOffset(int offset, unsigned long long newRows, int historyLines) {
    if (offset <= 0 || historyLines <= 0) return 0;
    const int extra = (int)std::min<unsigned long long>(newRows, (unsigned long long)historyLines);
    return (int)std::min<long long>((long long)historyLines, (long long)offset + extra);
}

Terminal::~Terminal() { Stop(); }

void Terminal::SetOnOutput(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(cbMu_);
    onOutput_ = std::move(fn);
}

bool Terminal::Start(const wg::Store& store, int cols, int rows) {
    Stop();
    {
        std::lock_guard<std::mutex> lock(mu_);
        error_.clear();
        scrollOffset_ = 0;
        screen_ = Screen(cols, rows);
        parser_.Reset();
    }

    path_ = ResolveShell(store.Get(L"shell"));
    if (path_.empty()) {
        std::lock_guard<std::mutex> lock(mu_);
        error_ = L"SHELL NOT FOUND";
        return false;
    }
    label_ = ShellLabel(path_);

    // A real console: no -Command - and no -NonInteractive. PSReadLine gives
    // the card line editing, history and completion for free.
    //
    // History goes to a file next to ps.ini rather than to the shared
    // ConsoleHost_history.txt: the card then keeps its own commands across
    // restarts, never touches the user's terminal history, and stays quiet when
    // the profile directory is not writable (restricted tokens, roaming
    // profiles). Both pwsh and 5.1 understand Set-PSReadLineOption; if the
    // module is missing the try/catch keeps the shell usable.
    const std::wstring history = HistoryPath(store);
    std::wstring args = L"-NoLogo -NoProfile";
    if (!history.empty()) {
        std::wstring quoted;
        quoted.reserve(history.size() + 2);
        quoted.push_back(L'\'');
        for (const wchar_t c : history) {
            if (c == L'\'') quoted.push_back(L'\'');
            quoted.push_back(c);
        }
        quoted.push_back(L'\'');
        args += L" -NoExit -Command \"try { Set-PSReadLineOption -HistorySavePath ";
        args += quoted;
        args += L" } catch { }\"";
    }

    DWORD err = 0;
    if (!pty_.Start(path_, args, cols, rows, Env(L"USERPROFILE"), &err)) {
        std::lock_guard<std::mutex> lock(mu_);
        error_ = L"CONPTY START FAILED";
        return false;
    }

    running_ = true;
    try {
        reader_ = std::thread([this] { ReadLoop(); });
    } catch (...) {
        running_ = false;
        pty_.Shutdown();
        pty_.Close();
        std::lock_guard<std::mutex> lock(mu_);
        error_ = L"THREAD FAILED";
        return false;
    }
    return true;
}

void Terminal::Stop() {
    running_ = false;
    pty_.Shutdown();
    if (reader_.joinable()) reader_.join();
    pty_.Close();
}

bool Terminal::running() const { return running_.load() && pty_.running(); }

void Terminal::Write(std::string_view bytes) {
    if (bytes.empty()) return;
    pty_.Write(bytes);
}

void Terminal::Resize(int cols, int rows) {
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (cols == screen_.cols() && rows == screen_.rows()) return;
        screen_.Resize(cols, rows);
        scrollOffset_ = 0;
    }
    pty_.Resize(cols, rows);
}

void Terminal::ScrollBy(int lines) {
    std::lock_guard<std::mutex> lock(mu_);
    const int maxOffset = (int)screen_.scrollback().size();
    scrollOffset_ = std::clamp(scrollOffset_ + lines, 0, maxOffset);
}

void Terminal::ScrollToBottom() {
    std::lock_guard<std::mutex> lock(mu_);
    scrollOffset_ = 0;
}

int Terminal::scrollOffset() const {
    std::lock_guard<std::mutex> lock(mu_);
    return scrollOffset_;
}

View Terminal::Snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    View view;
    view.cols = screen_.cols();
    view.rows = screen_.rows();
    view.cursorRow = screen_.cursorRow();
    view.cursorCol = screen_.cursorCol();
    view.alternate = screen_.alternate();
    view.bracketedPaste = screen_.bracketedPaste();
    view.mouseTracking = screen_.mouseTracking();
    view.sgrMouse = screen_.sgrMouse();
    view.scrollOffset = scrollOffset_;
    view.scrollbackLines = (int)screen_.scrollback().size();

    const std::vector<Row> visible = screen_.visibleRows();
    const auto& back = screen_.scrollback();
    const int rows = view.rows;
    const int base = std::max(0, (int)back.size() - scrollOffset_);

    for (int i = 0; i < rows; ++i) {
        const int index = base + i;
        ViewRow row;
        if (index < (int)back.size()) {
            row.id = back[(std::size_t)index].id;
            row.cells = back[(std::size_t)index].cells;
        } else {
            const int v = index - (int)back.size();
            if (v >= 0 && v < (int)visible.size()) {
                row.id = visible[(std::size_t)v].id;
                row.cells = visible[(std::size_t)v].cells;
            } else {
                row.cells.assign((std::size_t)view.cols, Cell{});
            }
        }
        view.lines.push_back(std::move(row));
    }

    view.cursorVisible = scrollOffset_ == 0 && screen_.cursorVisible();
    return view;
}

std::wstring Terminal::Text(const Selection& sel) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Row> rows(screen_.scrollback().begin(), screen_.scrollback().end());
    for (const Row& row : screen_.visibleRows()) rows.push_back(row);
    return ExtractText(rows, sel);
}

std::vector<Row> Terminal::RowsById() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Row> rows(screen_.scrollback().begin(), screen_.scrollback().end());
    for (const Row& row : screen_.visibleRows()) rows.push_back(row);
    return rows;
}

void Terminal::ClearScrollback() {
    std::lock_guard<std::mutex> lock(mu_);
    screen_.ClearScrollback();
    scrollOffset_ = 0;
}

std::wstring Terminal::error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
}

int Terminal::cols() const {
    std::lock_guard<std::mutex> lock(mu_);
    return screen_.cols();
}

int Terminal::rows() const {
    std::lock_guard<std::mutex> lock(mu_);
    return screen_.rows();
}

DWORD Terminal::exitCode() const { return pty_.exitCode(); }

DWORD Terminal::processId() const { return pty_.processId(); }

bool Terminal::appCursorKeys() const {
    std::lock_guard<std::mutex> lock(mu_);
    return screen_.applicationCursorKeys();
}

bool Terminal::bracketedPaste() const {
    std::lock_guard<std::mutex> lock(mu_);
    return screen_.bracketedPaste();
}

void Terminal::ReadLoop() {
    char buffer[16384];
    for (;;) {
        const int n = pty_.Read(buffer, (int)sizeof(buffer));
        if (n <= 0) break;
        bytesRead_ += (unsigned long long)n;
        const std::wstring text = DecodeChunk(std::string_view(buffer, (std::size_t)n));
        if (text.empty()) continue;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const auto before = screen_.scrollbackWrites();
            const bool wasAlternate = screen_.alternate();
            parser_.Feed(text, screen_);
            if (wasAlternate != screen_.alternate()) {
                scrollOffset_ = 0;
            } else {
                scrollOffset_ = AnchorScrollOffset(scrollOffset_, screen_.scrollbackWrites() - before,
                                                   (int)screen_.scrollback().size());
            }
        }
        Notify();
    }
    running_ = false;
}

void Terminal::Notify() {
    const unsigned long long now = GetTickCount64();
    const unsigned long long last = lastNotify_.load();
    if (last != 0 && now - last < kNotifyIntervalMs) {
        Sleep((DWORD)(kNotifyIntervalMs - (now - last)));
    }
    lastNotify_ = GetTickCount64();

    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lock(cbMu_);
        fn = onOutput_;
    }
    if (fn) fn();
}

} // namespace ps
