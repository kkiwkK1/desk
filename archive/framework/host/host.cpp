#include "widget/host.h"

#include "widget/appenv.h"
#include "widget/layout.h"

#include <shellapi.h>

#include <string>
#include <thread>

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

namespace wg {
namespace {

constexpr UINT kCommitMsg = WM_APP + 1;
constexpr UINT kTrayMsg = WM_APP + 2;
constexpr UINT kStreamMsg = WM_APP + 3;
constexpr UINT_PTR kRefreshTimer = 1;

constexpr int kHotkeyEdit = 1;
constexpr int kHotkeyQuit = 2;
constexpr int kHotkeyDetails = 3;

constexpr UINT kMenuRefresh = 10;
constexpr UINT kMenuSettings = 11;
constexpr UINT kMenuAutostart = 12;
constexpr UINT kMenuExit = 13;
constexpr UINT kMenuDetails = 14;
constexpr UINT kMenuActionBase = 100;  // widget-owned actions

} // namespace

class Host {
public:
    int Exec(std::unique_ptr<Widget> widget, Options options);

private:
    void Render();
    void SetEdit(bool on);
    void StartRefresh();
    void FinishRefresh();
    void PinToDesktop();
    void SavePos();
    void AvoidOverlap(bool persist);
    void OpenSettings();
    void ShowMenu();
    void AddTray();
    void RemoveTray();
    HICON BuildIcon();
    void Shutdown();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void CALLBACK OnForeground(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                      LONG a, LONG b, DWORD c, DWORD d);

    std::unique_ptr<Widget> widget_;
    Options options_;
    Desc desc_;
    Store store_;
    Canvas canvas_;

    HWND hwnd_ = nullptr;
    std::wstring className_;
    int x_ = 0;
    int y_ = 0;
    bool edit_ = false;
    bool busy_ = false;
    bool refreshAgain_ = false;
    bool quitting_ = false;
    std::thread worker_;

    NOTIFYICONDATAW nid_{};
    bool tray_ = false;
    HICON icon_ = nullptr;
    UINT taskbarCreated_ = 0;
    HWINEVENTHOOK fgHook_ = nullptr;
    HANDLE mutex_ = nullptr;
};

namespace {
Host* g_host = nullptr;
}

int Host::Exec(std::unique_ptr<Widget> widget, Options options) {
    widget_ = std::move(widget);
    options_ = options;
    desc_ = widget_->Describe();
    if (desc_.width < 1) desc_.width = 1;

    // One process per widget: the mutex name is the widget id, so launching the
    // same widget twice just focuses nothing and exits quietly.
    const std::wstring mutexName = desc_.id + L"SingleInstance";
    mutex_ = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (mutex_ && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex_);
        mutex_ = nullptr;
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const bool hadState =
        store_.Open(appenv::ConfigPath(desc_.id + L".ini", desc_.title));
    widget_->store_ = &store_;
    widget_->Load();

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    x_ = (int)store_.GetInt(L"x", screenW - desc_.width - 40);
    y_ = (int)store_.GetInt(L"y", screenH - 60 - 80);
    if (!hadState && options_.startWithWindows) appenv::SetAutostart(desc_.title, true);

    if (x_ < 0 || x_ > screenW - 60 || y_ < 0 || y_ > screenH - 40) {
        x_ = screenW - desc_.width - 40;
        y_ = screenH - 60 - 80;
    }

    className_ = L"wg." + desc_.id;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Host::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = className_.c_str();
    RegisterClassExW(&wc);

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    if (!options_.focusable) {
        exStyle |= WS_EX_NOACTIVATE;
        if (options_.clickThrough) exStyle |= WS_EX_TRANSPARENT;
    }

    int height = widget_->Height();
    if (height < 1) height = 1;

    hwnd_ = CreateWindowExW(exStyle, className_.c_str(), desc_.title.c_str(), WS_POPUP,
                            x_, y_, desc_.width, height, nullptr, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) return 1;

    g_host = this;
    widget_->SetRepaintHook([this] {
        if (hwnd_) PostMessageW(hwnd_, kStreamMsg, 0, 0);
    });
    AvoidOverlap(true);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    AddTray();
    PinToDesktop();
    fgHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                              &Host::OnForeground, 0, 0, WINEVENT_OUTOFCONTEXT);

    RegisterHotKey(hwnd_, kHotkeyEdit, options_.editHotkeyMods, options_.editHotkeyVk);
    RegisterHotKey(hwnd_, kHotkeyQuit, options_.quitHotkeyMods, options_.quitHotkeyVk);
    if (widget_->HasDetails()) {
        RegisterHotKey(hwnd_, kHotkeyDetails, options_.detailsHotkeyMods,
                       options_.detailsHotkeyVk);
    }
    for (const auto& hotkey : desc_.hotkeys) {
        if (hotkey.id >= 100 && hotkey.vk != 0) {
            RegisterHotKey(hwnd_, hotkey.id, hotkey.mods, hotkey.vk);
        }
    }
    if (options_.refreshSeconds > 0) {
        SetTimer(hwnd_, kRefreshTimer, (UINT)options_.refreshSeconds * 1000, nullptr);
    }

    Render();
    StartRefresh();

    if (wcsstr(GetCommandLineW(), L"--settings")) OpenSettings();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Shutdown();
    return 0;
}

void Host::Render() {
    if (!hwnd_ || quitting_) return;

    const int width = desc_.width;
    int height = widget_->Height();
    if (height < 1) height = 1;

    bool resized = false;
    if (canvas_.width() != width || canvas_.height() != height) {
        const int screenH = GetSystemMetrics(SM_CYSCREEN);
        if (y_ + height > screenH) y_ = screenH - height - 16;
        if (y_ < 0) y_ = 0;
        if (!canvas_.Resize(width, height)) return;
        SetWindowPos(hwnd_, nullptr, x_, y_, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        resized = true;
    }
    if (resized) AvoidOverlap(true);

    canvas_.Clear();
    widget_->Paint(canvas_);
    canvas_.Present(hwnd_, x_, y_);
}

void Host::SetEdit(bool on) {
    if (!hwnd_ || edit_ == on) return;
    edit_ = on;
    widget_->editing_ = on;

    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (options_.focusable) ex &= ~(LONG_PTR)WS_EX_TRANSPARENT;
    else if (on || !options_.clickThrough) ex &= ~(LONG_PTR)WS_EX_TRANSPARENT;
    else ex |= WS_EX_TRANSPARENT;
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    Render();
}

void Host::StartRefresh() {
    if (!hwnd_ || quitting_) return;
    if (busy_) {
        refreshAgain_ = true;
        return;
    }
    busy_ = true;
    try {
        worker_ = std::thread([this] {
            try {
                widget_->Refresh();
            } catch (...) {
                // A provider must never take the widget down with it.
            }
            if (hwnd_) PostMessageW(hwnd_, kCommitMsg, 0, 0);
        });
    } catch (...) {
        busy_ = false;
        refreshAgain_ = false;
    }
}

void Host::FinishRefresh() {
    if (worker_.joinable()) worker_.join();
    busy_ = false;
    if (quitting_) return;
    widget_->Commit();
    Render();

    // A timer tick or a settings change arrived while we were fetching.
    if (refreshAgain_) {
        refreshAgain_ = false;
        StartRefresh();
    }
}

void Host::PinToDesktop() {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Host::OnForeground(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (g_host && event == EVENT_SYSTEM_FOREGROUND && g_host->hwnd_ && hwnd != g_host->hwnd_) {
        g_host->PinToDesktop();
    }
}

void Host::SavePos() {
    store_.SetInt(L"x", x_);
    store_.SetInt(L"y", y_);
    store_.Save();
}

void Host::AvoidOverlap(bool persist) {
    if (!hwnd_ || quitting_) return;

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    RECT work{};
    if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
        work = monitor.rcWork;
    } else {
        work = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }

    const int height = widget_->Height();
    if (height < 1) return;

    // The named mutex makes "enumerate -> pick -> move" atomic across widget
    // processes, so two widgets starting together cannot choose the same slot.
    LayoutGuard guard;
    const std::vector<Rect> others = OtherWidgetRects(hwnd_);
    const POINT spot = FindFreeSpot(x_, y_, desc_.width, height, others, work, 8);
    if (spot.x == x_ && spot.y == y_) return;

    if (!SetWindowPos(hwnd_, nullptr, spot.x, spot.y, 0, 0,
                      SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        return;
    }
    x_ = spot.x;
    y_ = spot.y;

    if (persist) {
        store_.SetInt(L"x", x_);
        store_.SetInt(L"y", y_);
        store_.Save();
    }
}

void Host::OpenSettings() {
    if (!widget_->HasSettings()) return;
    widget_->OnSettings();
    Render();
    StartRefresh();
}

void Host::ShowMenu() {
    const std::vector<std::wstring> actions = widget_->Actions();

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuRefresh, L"Refresh");
    if (widget_->HasSettings()) AppendMenuW(menu, MF_STRING, kMenuSettings, L"Settings...");
    if (widget_->HasDetails()) AppendMenuW(menu, MF_STRING, kMenuDetails, L"Details...");
    if (!actions.empty()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        for (std::size_t i = 0; i < actions.size(); ++i) {
            AppendMenuW(menu, MF_STRING, kMenuActionBase + (UINT)i, actions[i].c_str());
        }
    }
    AppendMenuW(menu, MF_STRING | (appenv::AutostartEnabled(desc_.title) ? MF_CHECKED : 0),
                kMenuAutostart, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                                   hwnd_, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd_, WM_NULL, 0, 0);

    if (cmd >= (int)kMenuActionBase &&
        cmd < (int)kMenuActionBase + (int)actions.size()) {
        widget_->OnAction(cmd - (int)kMenuActionBase);
        if (options_.focusable) {
            SetForegroundWindow(hwnd_);
            SetFocus(hwnd_);
        }
        return;
    }

    switch (cmd) {
    case kMenuRefresh: StartRefresh(); break;
    case kMenuSettings: OpenSettings(); break;
    case kMenuDetails: widget_->ShowDetails(); break;
    case kMenuAutostart:
        appenv::SetAutostart(desc_.title, !appenv::AutostartEnabled(desc_.title));
        break;
    case kMenuExit: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    default: break;
    }
}

HICON Host::BuildIcon() {
    Canvas icon;
    if (!icon.Resize(16, 16)) return nullptr;
    icon.Clear(0);

    if (!widget_->TrayIcon(icon)) {
        Theme theme;
        const Color colors[3] = {theme.ok, theme.warn, theme.bad};
        const int heights[3] = {5, 9, 13};
        for (int i = 0; i < 3; ++i) {
            icon.Fill(3 + i * 4, 14 - heights[i], 2, heights[i] + 1, colors[i]);
        }
    }
    return icon.ToIcon();
}

void Host::AddTray() {
    if (!icon_) icon_ = BuildIcon();

    nid_ = {};
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = kTrayMsg;
    nid_.hIcon = icon_ ? icon_ : LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(nid_.szTip, desc_.title.c_str(), 128);
    tray_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
}

void Host::RemoveTray() {
    if (tray_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        tray_ = false;
    }
}

void Host::Shutdown() {
    quitting_ = true;

    if (hwnd_) {
        KillTimer(hwnd_, kRefreshTimer);
        UnregisterHotKey(hwnd_, kHotkeyEdit);
        UnregisterHotKey(hwnd_, kHotkeyQuit);
        UnregisterHotKey(hwnd_, kHotkeyDetails);
        for (const auto& hotkey : desc_.hotkeys) {
            if (hotkey.id >= 100) UnregisterHotKey(hwnd_, hotkey.id);
        }
    }
    RemoveTray();

    if (fgHook_) {
        UnhookWinEvent(fgHook_);
        fgHook_ = nullptr;
    }
    if (worker_.joinable()) worker_.join();

    SavePos();

    if (icon_) {
        DestroyIcon(icon_);
        icon_ = nullptr;
    }
    if (mutex_) {
        CloseHandle(mutex_);
        mutex_ = nullptr;
    }
    g_host = nullptr;
}

LRESULT CALLBACK Host::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Host* host = g_host;
    if (!host) return DefWindowProcW(hwnd, msg, wp, lp);

    if (host->taskbarCreated_ && msg == host->taskbarCreated_) {
        if (host->tray_) Shell_NotifyIconW(NIM_DELETE, &host->nid_);
        host->AddTray();
        return 0;
    }

    switch (msg) {
    case kTrayMsg:
        if (LOWORD(lp) == WM_RBUTTONUP) host->ShowMenu();
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK) host->SetEdit(!host->edit_);
        return 0;

    case kCommitMsg:
        host->FinishRefresh();
        return 0;

    case kStreamMsg:
        if (!host->quitting_) host->Render();
        return 0;

    case WM_HOTKEY:
        if (wp == kHotkeyEdit) host->SetEdit(!host->edit_);
        else if (wp == kHotkeyQuit) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        else if (wp == kHotkeyDetails) host->widget_->ShowDetails();
        else {
            for (const auto& hotkey : host->desc_.hotkeys) {
                if (wp == (WPARAM)hotkey.id) {
                    host->widget_->OnHotkey(hotkey.id);
                    if (host->options_.focusable) {
                        SetForegroundWindow(hwnd);
                        SetFocus(hwnd);
                    }
                    break;
                }
            }
        }
        return 0;

    case WM_TIMER:
        if (wp == kRefreshTimer) host->StartRefresh();
        return 0;

    case WM_EXITSIZEMOVE: {
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        host->x_ = rc.left;
        host->y_ = rc.top;
        host->SavePos();
        host->AvoidOverlap(true);
        host->Render();
        host->PinToDesktop();
        return 0;
    }

    case WM_MOUSEACTIVATE:
        return host->options_.focusable ? MA_ACTIVATE : MA_NOACTIVATE;

    case WM_LBUTTONDOWN:
        if (host->edit_) {
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            host->x_ = rc.left;
            host->y_ = rc.top;
        } else if (host->options_.focusable) {
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
            host->widget_->OnClick((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
        }
        return 0;

    case WM_SETFOCUS:
        host->widget_->OnFocus(true);
        host->PinToDesktop();
        host->Render();
        return 0;

    case WM_KILLFOCUS:
        host->widget_->OnFocus(false);
        host->Render();
        return 0;

    case WM_CHAR:
        if (host->options_.focusable && !host->edit_) {
            host->widget_->OnChar((wchar_t)wp);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (host->options_.focusable && !host->edit_) {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (host->widget_->OnKey((UINT)wp, ctrl, alt, shift)) return 0;
        }
        break;

    case WM_MOUSEWHEEL:
        if (host->options_.focusable) {
            host->widget_->OnScroll((int)(short)HIWORD(wp));
            return 0;
        }
        break;

    case WM_RBUTTONUP:
        if (host->edit_) host->ShowMenu();
        return 0;

    case WM_CLOSE:
        host->quitting_ = true;
        host->RemoveTray();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

int Run(std::unique_ptr<Widget> widget, Options options) {
    Host host;
    return host.Exec(std::move(widget), options);
}

} // namespace wg
