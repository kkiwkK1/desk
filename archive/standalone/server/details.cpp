#include "details.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>

namespace {

constexpr int kWidth = 440;
constexpr int kHeader = 38;      // first host baseline
constexpr int kRowPitch = 38;
constexpr int kMaxRows = 8;
constexpr const wchar_t* kClass = L"wg.server.details";

DetailsPanel* g_panel = nullptr;

int VisibleRows(const server::ServerData& data) {
    if (data.hosts.empty()) return 1;
    return (int)std::min<size_t>(data.hosts.size(), kMaxRows);
}

std::wstring Truncate(std::wstring text, size_t max) {
    if (text.size() > max) text.resize(max);
    return text;
}

std::wstring Rtt(int ms) {
    wchar_t buf[16];
    if (ms >= 1000) swprintf(buf, 16, L"%.1fS", ms / 1000.0);
    else swprintf(buf, 16, L"%dMS", ms);
    return buf;
}

int Worst(const server::HostStatus& host) {
    int worst = -1;
    if (host.cpuPercent > worst) worst = host.cpuPercent;
    if (host.memPercent > worst) worst = host.memPercent;
    if (host.diskPercent > worst) worst = host.diskPercent;
    return worst;
}

std::wstring Percent(int value) {
    if (value < 0) return L"--";
    wchar_t buf[8];
    swprintf(buf, 8, L"%d%%", value);
    return buf;
}

// The pixel font has no backslashes or parentheses; present paths in the
// slash form and keep failure text to glyphs that actually exist.
std::wstring DisplayPath(std::wstring path) {
    for (wchar_t& c : path) {
        if (c == L'\\') c = L'/';
    }
    return path;
}

std::wstring MessageFor(const server::HostStatus& host) {
    if (host.ok) return {};
    if (host.state == L"WAIT") return L"PROBING";
    if (host.state == L"AUTH") return L"PERMISSION DENIED - CHECK KEY AND USER";
    if (host.state == L"DNS") return L"CANNOT RESOLVE HOSTNAME";
    if (host.state == L"REFUSED") return L"SSH PORT CLOSED OR REFUSED";
    if (host.state == L"HOSTKEY") return L"HOST KEY CHANGED - CHECK KNOWN_HOSTS";
    if (host.state == L"UNREACH") return L"NO ROUTE TO HOST";
    if (host.state == L"CLOSED") return L"CONNECTION CLOSED BY REMOTE";
    if (host.state == L"NO SSH") return L"OPENSSH CLIENT NOT FOUND";
    if (host.state == L"NO DATA") return L"NO /PROC METRICS - LINUX ONLY";
    if (host.state == L"TIMEOUT" && host.error.empty()) return L"NO RESPONSE FROM HOST";
    return host.error.empty() ? std::wstring(L"SSH FAILED") : host.error;
}

std::wstring DetailLine(const server::HostStatus& host) {
    if (!host.ok) return MessageFor(host);

    wchar_t buf[160];
    swprintf(buf, 160, L"CPU %ls  MEM %ls  DSK %ls  LOAD %ls  CORES %d  UP %ls",
             Percent(host.cpuPercent).c_str(), Percent(host.memPercent).c_str(),
             Percent(host.diskPercent).c_str(), server::FormatLoad(host.load1).c_str(),
             host.cores, server::FormatUptime(host.uptimeSec).c_str());
    return buf;
}

} // namespace

int DetailsHeight(const server::ServerData& data) {
    return kHeader + VisibleRows(data) * kRowPitch + 28;
}

void DrawDetails(const server::ServerData& data, const wg::Theme& theme, wg::Canvas& canvas) {
    const int width = canvas.width();
    const int right = width - 8;

    canvas.Panel(theme, false, true);

    canvas.Text(L"SERVER STATUS", 8.0f, 10.0f, theme.text, wg::kTextScale);
    const std::wstring stamp = data.updated.empty() ? L"--" : L"UPDATED " + data.updated;
    canvas.TextRight(stamp, right, 15.0f, theme.faint, 1.0f);
    canvas.HLine(8, right, 30, theme.border);

    if (data.hosts.empty()) {
        canvas.TextCentered(L"NO SERVERS CONFIGURED", width / 2, 50.0f, theme.label,
                            wg::kTextScale);
        canvas.TextCentered(L"TRAY > SETTINGS", width / 2, 74.0f, theme.faint, 1.0f);
    } else {
        int i = 0;
        for (const server::HostStatus& host : data.hosts) {
            if (i++ >= kMaxRows) break;
            const int y = kHeader + (i - 1) * kRowPitch;

            canvas.Text(Truncate(host.name, 28), 8.0f, (float)y, theme.text, wg::kTextScale);

            const wg::Color color = host.state == L"WAIT" ? theme.faint
                                  : !host.ok ? theme.bad
                                  : Worst(host) >= 90 ? theme.bad
                                  : Worst(host) >= 75 ? theme.warn
                                                      : theme.ok;
            const std::wstring value =
                host.ok ? L"UP " + Rtt(host.rttMs) : host.state;
            canvas.TextRight(value, right, (float)y, color, wg::kTextScale);

            const std::wstring detail = DetailLine(host);
            canvas.Text(Truncate(detail, 70), 8.0f, (float)(y + 16),
                        host.ok ? theme.faint : theme.label, 1.0f);
        }
    }

    const int rule = kHeader + VisibleRows(data) * kRowPitch + 2;
    canvas.HLine(8, right, rule, theme.border);

    const std::wstring ssh = data.sshFound
                                 ? L"SSH " + Truncate(DisplayPath(data.sshPath), 62)
                                 : std::wstring(L"SSH CLIENT NOT FOUND");
    canvas.Text(ssh, 8.0f, (float)(rule + 6), data.sshFound ? theme.faint : theme.bad, 1.0f);
    canvas.TextRight(L"ESC CLOSE", right, (float)(rule + 6), theme.faint, 1.0f);
}

DetailsPanel::~DetailsPanel() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (g_panel == this) g_panel = nullptr;
}

bool DetailsPanel::visible() const {
    return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE;
}

void DetailsPanel::Toggle(const server::ServerData& data, const wg::Theme& theme,
                          wg::Store& store) {
    if (visible()) {
        Hide();
        return;
    }
    Show(data, theme, store);
}

void DetailsPanel::Show(const server::ServerData& data, const wg::Theme& theme,
                        wg::Store& store) {
    const int height = DetailsHeight(data);
    if (!hwnd_) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &DetailsPanel::WndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kClass;
            RegisterClassExW(&wc);
            registered = true;
        }

        g_panel = this;
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST, kClass,
                                L"Server Status", WS_POPUP, 0, 0, kWidth, height, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd_) {
            g_panel = nullptr;
            return;
        }
    }

    Place(store, data);
    Redraw(data, theme);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
}

void DetailsPanel::Place(wg::Store& store, const server::ServerData& data) {
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int widgetW = server::kPanelWidth;
    const wg::ListStyle style;
    const int widgetH = wg::ListHeight(style, std::max<size_t>(1, data.rows.size()));
    const int height = DetailsHeight(data);

    int widgetX = (int)store.GetInt(L"x", screenW - widgetW - 40);
    int widgetY = (int)store.GetInt(L"y", screenH - widgetH - 80);

    int x = widgetX + widgetW - kWidth;
    int y = widgetY - height - 8;
    if (y < 8) y = widgetY + widgetH + 8;
    if (x < 8) x = 8;
    if (x + kWidth > screenW - 8) x = screenW - kWidth - 8;
    if (y + height > screenH - 8) y = screenH - height - 8;
    if (y < 8) y = 8;

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, kWidth, height, SWP_NOACTIVATE);
}

void DetailsPanel::Redraw(const server::ServerData& data, const wg::Theme& theme) {
    if (!hwnd_) return;
    const int height = DetailsHeight(data);
    if (!canvas_.Resize(kWidth, height)) return;

    canvas_.Clear();
    DrawDetails(data, theme, canvas_);

    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    canvas_.Present(hwnd_, rc.left, rc.top);
}

void DetailsPanel::Refresh(const server::ServerData& data, const wg::Theme& theme,
                           wg::Store& store) {
    if (!visible()) return;
    // The host count decides the panel height, and Settings can change it
    // while the panel is open.
    Place(store, data);
    Redraw(data, theme);
}

void DetailsPanel::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

LRESULT CALLBACK DetailsPanel::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && g_panel) g_panel->Hide();
        return 0;
    case WM_CLOSE:
        if (g_panel) g_panel->Hide();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
