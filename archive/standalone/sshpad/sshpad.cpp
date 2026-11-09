// SSH Pad - a desktop SSH terminal.
//
// A Windows console is allocated but hidden; ssh.exe runs inside it and the
// widget reads the console screen buffer instead of opening a visible console
// window. The widget only repaints when the buffer changes, so an idle session
// costs one sleeping poll thread and no drawing.
#include "widget/host.h"
#include "widget/prompt.h"
#include "widget/proc.h"
#include "widget/store.h"
#include "data/server/providers.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kHotkeyBase = 100;
constexpr int kMaxRows = 8;

constexpr int kCols = 80;
constexpr int kRows = 24;
constexpr int kCellW = 9;
constexpr int kCellH = 14;
constexpr int kWidth = 744;
constexpr int kHeader = 36;
constexpr int kTermX = 8;
constexpr int kTermY = 40;
constexpr int kTermW = kCols * kCellW;
constexpr int kTermH = kRows * kCellH;
constexpr int kFooterY = kTermY + kTermH + 6;
constexpr int kHeight = kFooterY + 18;
constexpr int kListRowH = 24;

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    const std::size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(0, slash);
}

std::wstring ServerIni() {
    return ExeDir() + L"\\server.ini";
}

std::wstring Trim(const std::wstring& text) {
    std::size_t begin = 0, end = text.size();
    while (begin < end && std::iswspace(text[begin])) ++begin;
    while (end > begin && std::iswspace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::wstring Unquote(const std::wstring& text) {
    if (text.size() >= 2 && ((text.front() == L'"' && text.back() == L'"') ||
                             (text.front() == L'\'' && text.back() == L'\''))) {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

struct SshData {
    std::vector<std::wstring> hosts;
    std::wstring ssh;
    std::wstring key;
};

struct Cell {
    wchar_t ch = L' ';
    WORD attr = 7;
};

struct Screen {
    int cols = kCols;
    int rows = kRows;
    std::vector<Cell> cells = std::vector<Cell>((std::size_t)kCols * kRows);
    int cursorX = 0;
    int cursorY = 0;
    bool cursorVisible = true;
    bool valid = false;
};

bool SameScreen(const Screen& a, const Screen& b) {
    if (a.valid != b.valid || a.cols != b.cols || a.rows != b.rows ||
        a.cursorX != b.cursorX || a.cursorY != b.cursorY ||
        a.cursorVisible != b.cursorVisible) {
        return false;
    }
    if (a.cells.size() != b.cells.size()) return false;
    for (std::size_t i = 0; i < a.cells.size(); ++i) {
        if (a.cells[i].ch != b.cells[i].ch || a.cells[i].attr != b.cells[i].attr) return false;
    }
    return true;
}

BOOL WINAPI IgnoreCtrlC(DWORD type) {
    return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT;
}

class SshPad : public wg::WidgetT<SshData> {
public:
    ~SshPad() override {
        stop_ = true;
        if (poll_.joinable()) poll_.join();
        StopSession(false);
        if (consoleOut_) CloseHandle(consoleOut_);
        if (consoleIn_) CloseHandle(consoleIn_);
        FreeConsole();
    }

    wg::Desc Describe() override {
        wg::Desc desc;
        desc.id = L"sshpad";
        desc.title = L"SSH Pad";
        desc.width = kWidth;
        for (int i = 0; i < kMaxRows; ++i) {
            desc.hotkeys.push_back(
                wg::Hotkey{kHotkeyBase + i, MOD_CONTROL | MOD_ALT, (UINT)(L'1' + i)});
        }
        return desc;
    }

    bool TrayIcon(wg::Canvas& canvas) override {
        canvas.Fill(2, 2, 12, 12, theme_.border);
        canvas.Fill(3, 3, 10, 10, theme_.panelSolid);
        canvas.Text(L">", 4.0f, 3.0f, theme_.ok, 1.0f);
        canvas.Text(L"_", 9.0f, 8.0f, theme_.label, 1.0f);
        return true;
    }

    bool HasSettings() const override { return true; }

    void OnSettings() override {
        wg::Store ini;
        ini.Open(ServerIni());
        const std::wstring current =
            server::JoinHosts(server::ParseHosts(ini.Get(L"hosts")));

        wg::PromptSpec spec;
        spec.label = L"SSH HOSTS";
        spec.hint = L"WEB-01, DB-01   TYPE - TO CLEAR";
        spec.initial = current;
        spec.saved = !current.empty();
        spec.width = 420;

        std::wstring input;
        if (!wg::PromptText(spec, input)) return;

        if (input == L"-") ini.Set(L"hosts", L"");
        else ini.Set(L"hosts", server::JoinHosts(server::ParseHosts(input)));
        ini.Save();
    }

    std::vector<std::wstring> Actions() override {
        std::vector<std::wstring> actions;
        const auto& hosts = shown().hosts;
        for (std::size_t i = 0; i < hosts.size(); ++i) {
            actions.push_back(L"Connect  " + server::DisplayName(hosts[i]) + L"   (Ctrl+Alt+" +
                              std::to_wstring(i + 1) + L")");
        }
        return actions;
    }

    bool OnAction(int index) override {
        Connect(index);
        return true;
    }

    bool OnHotkey(int id) override {
        Connect(id - kHotkeyBase);
        return true;
    }

    void OnClick(int, int y) override {
        if (connected_) return;
        if (!hasSession_) {
            const int top = HostListTop(shown().hosts.size());
            const int index = (y - top) / kListRowH;
            if (index >= 0 && index < (int)shown().hosts.size()) Connect(index);
        }
    }

    void OnFocus(bool focused) override {
        focused_ = focused;
        RequestRepaint();
    }

    void OnChar(wchar_t ch) override {
        if (!connected_ || !focused_) return;
        if (ch < 32) return;
        if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) return;
        SendChar(ch);
    }

    bool OnKey(UINT vk, bool ctrl, bool alt, bool shift) override {
        if (!connected_ || !focused_) return false;

        DWORD state = 0;
        if (ctrl) state |= LEFT_CTRL_PRESSED;
        if (alt) state |= LEFT_ALT_PRESSED;
        if (shift) state |= SHIFT_PRESSED;

        if (ctrl && vk >= 'A' && vk <= 'Z') {
            SendKey(vk, (wchar_t)(vk - 'A' + 1), state);
            return true;
        }

        switch (vk) {
            case VK_RETURN: SendKey(vk, L'\r', state); return true;
            case VK_BACK: SendKey(vk, L'\b', state); return true;
            case VK_TAB: SendKey(vk, L'\t', state); return true;
            case VK_ESCAPE: SendKey(vk, 27, state); return true;
            case VK_UP:
            case VK_DOWN:
            case VK_LEFT:
            case VK_RIGHT:
            case VK_HOME:
            case VK_END:
            case VK_INSERT:
            case VK_DELETE:
            case VK_PRIOR:
            case VK_NEXT:
            case VK_F1:
            case VK_F2:
            case VK_F3:
            case VK_F4:
            case VK_F5:
            case VK_F6:
            case VK_F7:
            case VK_F8:
            case VK_F9:
            case VK_F10:
            case VK_F11:
            case VK_F12:
                SendKey(vk, 0, state);
                return true;
            default:
                return false;
        }
    }

    void OnScroll(int) override {}

protected:
    void LoadData(SshData& data) override {
        LoadConfig(data);
        autoConnect_ = store().GetInt(L"auto", 1) != 0;
        InitConsole();
        if (!poll_.joinable()) poll_ = std::thread([this] { PollLoop(); });
    }

    void RefreshData(SshData& data) override { LoadConfig(data); }

    void OnCommit() override {
        if (autoConnect_ && !autoTried_ && !hasSession_ && !shown().hosts.empty()) {
            autoTried_ = true;
            Connect(0);
        }
    }

    int HeightOf(const SshData&) const override { return kHeight; }

    void PaintData(const SshData& data, wg::Canvas& canvas) override {
        Screen screen;
        std::wstring status;
        bool error = false;
        bool hasSession = false;
        {
            std::lock_guard<std::mutex> lock(stateMu_);
            screen = screen_;
            status = status_;
            error = error_;
            hasSession = hasSession_;
        }

        canvas.Panel(theme_, focused_ || editing());
        const int right = canvas.width() - 8;

        canvas.Text(L"SSH PAD", 8.0f, 8.0f, theme_.text, 1.5f);

        std::wstring rightText = status;
        wg::Color rightColor = error ? theme_.bad : theme_.ok;
        if (connected_ && !data.hosts.empty()) {
            std::wstring host = server::DisplayName(sessionHost_);
            while (host.size() > 1 && canvas.TextWidth(host, 1.0f) > 120.0f) host.pop_back();
            rightText = host + L" - CONNECTED";
        } else if (data.hosts.empty()) {
            rightText = L"NO HOSTS";
            rightColor = theme_.warn;
        }
        canvas.TextRight(rightText, right, 10.0f, rightColor, 1.0f);
        canvas.HLine(8, right, 30, theme_.border);

        if (!hasSession) {
            DrawHostList(data, canvas);
        } else {
            DrawTerminal(screen, canvas);
        }

        canvas.HLine(8, right, kFooterY - 8, theme_.border);

        std::wstring hint;
        wg::Color hintColor = theme_.faint;
        if (error) {
            hint = status;
            hintColor = theme_.bad;
        } else if (!hasSession) {
            hint = data.hosts.empty() ? L"EDIT SERVER.INI OR TRAY > SETTINGS"
                                      : L"CLICK A HOST OR CTRL+ALT+1..8";
        } else if (!connected_) {
            hint = L"SESSION ENDED  ·  CTRL+ALT+1..8 TO RECONNECT";
        } else if (!focused_) {
            hint = L"CLICK TO TYPE  ·  CTRL+ALT+H EDIT";
        } else {
            hint = L"CTRL+ALT+H EDIT  ·  CTRL+ALT+SHIFT+H QUIT";
        }
        canvas.Text(hint, 8.0f, (float)kFooterY, hintColor, 1.0f);
    }

private:
    void LoadConfig(SshData& data) {
        data = SshData{};
        wg::Store ini;
        ini.Open(ServerIni());
        data.hosts = server::ParseHosts(ini.Get(L"hosts"));
        data.ssh = ini.Get(L"ssh");
        data.key = ini.Get(L"key");
    }

    static int HostListTop(std::size_t count) {
        const int rows = (int)std::min<std::size_t>(std::max<std::size_t>(1, count), kMaxRows);
        return kTermY + (kTermH - rows * kListRowH) / 2;
    }

    void DrawHostList(const SshData& data, wg::Canvas& canvas) {
        if (data.hosts.empty()) {
            canvas.TextCentered(L"NO SSH HOSTS", canvas.width() / 2, 110.0f, theme_.label,
                                wg::kTextScale);
            canvas.TextCentered(L"EDIT SERVER.INI", canvas.width() / 2, 134.0f, theme_.faint,
                                1.0f);
            return;
        }

        int y = HostListTop(data.hosts.size());
        for (std::size_t i = 0; i < data.hosts.size() && i < (std::size_t)kMaxRows; ++i) {
            canvas.Text(std::to_wstring(i + 1), 10.0f, (float)y, theme_.ok, 1.0f);
            std::wstring label = server::DisplayName(data.hosts[i]);
            while (label.size() > 1 && canvas.TextWidth(label, 1.0f) > 150.0f) label.pop_back();
            canvas.Text(label, 28.0f, (float)y, theme_.text, 1.0f);
            canvas.TextRight(L"CTRL+ALT+" + std::to_wstring(i + 1), 300.0f, (float)y,
                             theme_.faint, 1.0f);
            y += kListRowH;
        }
    }

    void DrawTerminal(const Screen& screen, wg::Canvas& canvas) {
        const int cols = std::min(screen.cols, kCols);
        const int rows = std::min(screen.rows, kRows);
        const bool hasCursor = connected_ && screen.cursorVisible;

        for (int row = 0; row < rows; ++row) {
            const int y = kTermY + row * kCellH;
            for (int col = 0; col < cols; ++col) {
                const std::size_t index = (std::size_t)row * (std::size_t)screen.cols + col;
                if (index >= screen.cells.size()) continue;
                Cell cell = screen.cells[index];

                WORD attr = cell.attr;
                if (attr & COMMON_LVB_REVERSE_VIDEO) {
                    const WORD fg = (WORD)(attr & 0x0F);
                    const WORD bg = (WORD)((attr >> 4) & 0x0F);
                    attr = (WORD)((fg << 4) | bg);
                }

                wg::Color fg = AttrColor(attr & 0x0F);
                wg::Color bg = AttrColor((attr >> 4) & 0x0F);
                const bool cursor = hasCursor && col == screen.cursorX && row == screen.cursorY;

                if (bg != theme_.panelSolid) canvas.Fill(kTermX + col * kCellW, y, kCellW, kCellH, bg);
                if (cursor) {
                    canvas.Fill(kTermX + col * kCellW, y, kCellW, kCellH, theme_.text);
                    fg = theme_.panelSolid;
                }

                wchar_t ch = cell.ch;
                if (ch < 32) ch = L' ';
                if (ch > 0x7E) ch = L'?';
                if (ch != L' ' || cursor) {
                    canvas.Text(std::wstring(1, ch), (float)(kTermX + col * kCellW), (float)y,
                                fg, 1.5f);
                }
            }
        }
    }

    wg::Color AttrColor(int index) const {
        switch (index) {
            case 0: return theme_.panelSolid;
            case 1: return wg::Rgba(0x60A5FA);
            case 2: return theme_.ok;
            case 3: return wg::Rgba(0x22D3EE);
            case 4: return theme_.bad;
            case 5: return wg::Rgba(0xE879F9);
            case 6: return theme_.warn;
            case 7: return theme_.text;
            case 8: return theme_.faint;
            case 9: return wg::Rgba(0x93C5FD);
            case 10: return wg::Rgba(0x86EFAC);
            case 11: return wg::Rgba(0x67E8F9);
            case 12: return wg::Rgba(0xFCA5A5);
            case 13: return wg::Rgba(0xF0ABFC);
            case 14: return wg::Rgba(0xFDE68A);
            default: return theme_.text;
        }
    }

    bool InitConsole() {
        if (consoleOut_ && consoleIn_) return true;

        FreeConsole();  // detach from any launching console; we want our own hidden one
        if (!AllocConsole()) return false;

        HWND consoleWindow = GetConsoleWindow();
        if (consoleWindow) {
            LONG_PTR ex = GetWindowLongPtrW(consoleWindow, GWL_EXSTYLE);
            ex |= WS_EX_TOOLWINDOW;
            ex &= ~WS_EX_APPWINDOW;
            SetWindowLongPtrW(consoleWindow, GWL_EXSTYLE, ex);
            ShowWindow(consoleWindow, SW_HIDE);
        }

        consoleOut_ = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                  nullptr);
        consoleIn_ = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                 nullptr);
        if (consoleOut_ == INVALID_HANDLE_VALUE || consoleIn_ == INVALID_HANDLE_VALUE) {
            if (consoleOut_ && consoleOut_ != INVALID_HANDLE_VALUE) CloseHandle(consoleOut_);
            if (consoleIn_ && consoleIn_ != INVALID_HANDLE_VALUE) CloseHandle(consoleIn_);
            consoleOut_ = nullptr;
            consoleIn_ = nullptr;
            return false;
        }

        SetConsoleCtrlHandler(IgnoreCtrlC, TRUE);
        SetConsoleScreenBufferSize(consoleOut_, {(SHORT)kCols, 400});
        SMALL_RECT rect{0, 0, (SHORT)(kCols - 1), (SHORT)(kRows - 1)};
        SetConsoleWindowInfo(consoleOut_, TRUE, &rect);

        DWORD mode = 0;
        if (GetConsoleMode(consoleOut_, &mode)) {
            SetConsoleMode(consoleOut_, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                            ENABLE_PROCESSED_OUTPUT);
        }
        ClearConsole();
        return true;
    }

    void ClearConsole() {
        if (!consoleOut_) return;
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(consoleOut_, &info)) return;
        const DWORD cells = (DWORD)info.dwSize.X * (DWORD)info.dwSize.Y;
        DWORD written = 0;
        COORD home{0, 0};
        FillConsoleOutputCharacterW(consoleOut_, L' ', cells, home, &written);
        FillConsoleOutputAttribute(consoleOut_, 7, cells, home, &written);
        SetConsoleCursorPosition(consoleOut_, home);
    }

    bool ReadScreen(Screen& screen) {
        if (!consoleOut_) return false;
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(consoleOut_, &info)) return false;

        const int cols = info.srWindow.Right - info.srWindow.Left + 1;
        const int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (cols < 1 || rows < 1 || cols > kCols || rows > kRows) return false;

        std::vector<CHAR_INFO> buffer((std::size_t)cols * rows);
        COORD size{(SHORT)cols, (SHORT)rows};
        COORD origin{0, 0};
        SMALL_RECT region{info.srWindow.Left, info.srWindow.Top, info.srWindow.Right,
                          info.srWindow.Bottom};
        if (!ReadConsoleOutputW(consoleOut_, buffer.data(), size, origin, &region)) return false;

        screen.cols = cols;
        screen.rows = rows;
        screen.cells.resize((std::size_t)cols * rows);
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            screen.cells[i].ch = buffer[i].Char.UnicodeChar;
            screen.cells[i].attr = buffer[i].Attributes;
        }
        screen.cursorX = info.dwCursorPosition.X - info.srWindow.Left;
        screen.cursorY = info.dwCursorPosition.Y - info.srWindow.Top;
        CONSOLE_CURSOR_INFO cursor{};
        screen.cursorVisible = GetConsoleCursorInfo(consoleOut_, &cursor) && cursor.bVisible;
        screen.valid = true;
        return true;
    }

    void PollLoop() {
        while (!stop_) {
            HANDLE process = process_.load();
            if (process) {
                DWORD code = 0;
                if (GetExitCodeProcess(process, &code) && code != STILL_ACTIVE) {
                    if (process_.compare_exchange_strong(process, nullptr)) CloseHandle(process);
                    connected_ = false;
                    {
                        std::lock_guard<std::mutex> lock(stateMu_);
                        status_ = L"SESSION ENDED";
                        error_ = false;
                    }
                    RequestRepaint();
                } else {
                    Screen screen;
                    if (ReadScreen(screen)) {
                        bool changed = false;
                        {
                            std::lock_guard<std::mutex> lock(stateMu_);
                            changed = !SameScreen(screen_, screen);
                            if (changed) screen_ = screen;
                        }
                        if (changed) RequestRepaint();
                    }
                }
            }
            Sleep(process ? 40 : 150);
        }
    }

    void StopSession(bool clear) {
        HANDLE process = process_.exchange(nullptr);
        if (process) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 1000);
            CloseHandle(process);
        }
        connected_ = false;
        if (clear) {
            std::lock_guard<std::mutex> lock(stateMu_);
            screen_ = Screen{};
            status_ = L"READY";
            error_ = false;
        }
    }

    void Connect(int index) {
        if (index < 0 || index >= (int)shown().hosts.size()) return;

        StopSession(false);
        if (!consoleOut_ && !InitConsole()) {
            std::lock_guard<std::mutex> lock(stateMu_);
            status_ = L"NO CONSOLE";
            error_ = true;
            hasSession_ = true;
            RequestRepaint();
            return;
        }

        const std::wstring& target = shown().hosts[(std::size_t)index];
        const std::wstring ssh = server::SshPath(shown().ssh);
        if (ssh.empty()) {
            std::lock_guard<std::mutex> lock(stateMu_);
            status_ = L"NO SSH CLIENT";
            error_ = true;
            hasSession_ = true;
            RequestRepaint();
            return;
        }

        ClearConsole();
        {
            std::lock_guard<std::mutex> lock(stateMu_);
            screen_ = Screen{};
            status_ = L"CONNECTING";
            error_ = false;
            hasSession_ = true;
        }

        std::wstring args =
            L"-tt -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new"
            L" -o UpdateHostKeys=no -o LogLevel=ERROR";
        const std::wstring key = Unquote(Trim(shown().key));
        if (!key.empty()) args += L" -i " + wg::QuoteArg(key);
        args += L" " + wg::QuoteArg(target);

        std::wstring cmdline = wg::QuoteArg(ssh) + L" " + args;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(ssh.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            nullptr, &si, &pi)) {
            std::lock_guard<std::mutex> lock(stateMu_);
            status_ = L"CANNOT START SSH";
            error_ = true;
            RequestRepaint();
            return;
        }

        CloseHandle(pi.hThread);
        process_.store(pi.hProcess);
        sessionHost_ = target;
        connected_ = true;
        {
            std::lock_guard<std::mutex> lock(stateMu_);
            status_ = L"CONNECTED";
            error_ = false;
        }
        RequestRepaint();
    }

    void SendKey(WORD vk, wchar_t ch, DWORD controlState) {
        if (!consoleIn_) return;
        INPUT_RECORD record{};
        record.EventType = KEY_EVENT;
        record.Event.KeyEvent.bKeyDown = TRUE;
        record.Event.KeyEvent.wRepeatCount = 1;
        record.Event.KeyEvent.wVirtualKeyCode = vk;
        record.Event.KeyEvent.wVirtualScanCode = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        record.Event.KeyEvent.uChar.UnicodeChar = ch;
        record.Event.KeyEvent.dwControlKeyState = controlState;
        DWORD written = 0;
        WriteConsoleInputW(consoleIn_, &record, 1, &written);

        record.Event.KeyEvent.bKeyDown = FALSE;
        WriteConsoleInputW(consoleIn_, &record, 1, &written);
    }

    void SendChar(wchar_t ch) {
        if (!consoleIn_) return;
        const SHORT scan = VkKeyScanW(ch);
        WORD vk = 0;
        if (scan != -1) vk = (WORD)(scan & 0xFF);
        SendKey(vk, ch, 0);
    }

    HANDLE consoleOut_ = nullptr;
    HANDLE consoleIn_ = nullptr;
    std::atomic<HANDLE> process_{nullptr};
    std::atomic<bool> connected_{false};
    std::atomic<bool> focused_{false};
    std::atomic<bool> stop_{false};

    std::thread poll_;
    std::mutex stateMu_;
    Screen screen_;
    std::wstring status_ = L"READY";
    std::wstring sessionHost_;
    bool error_ = false;
    bool hasSession_ = false;
    bool autoConnect_ = true;
    bool autoTried_ = false;

    wg::Theme theme_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wg::Options options;
    options.refreshSeconds = 0;
    options.startWithWindows = false;
    options.focusable = true;
    options.clickThrough = false;
    options.editHotkeyVk = 'H';
    options.quitHotkeyMods = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    options.quitHotkeyVk = 'H';
    return wg::Run(std::make_unique<SshPad>(), options);
}
