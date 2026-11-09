#include "details.h"

#include "data/aimeter/providers.h"

#include <cstdio>
#include <cwchar>
#include <string>

namespace {

constexpr int kWidth = 400;
constexpr int kHeight = 184;
constexpr const wchar_t* kClass = L"wg.aimeter.details";

const wg::Color kInput = wg::Rgba(0x60A5FA);
const wg::Color kWrite = wg::Rgba(0xA78BFA);

DetailsPanel* g_panel = nullptr;

std::wstring SpendText(long long cents) {
    const wchar_t* sign = cents > 0 ? L"-" : (cents < 0 ? L"+" : L"");
    const long long magnitude = cents < 0 ? -cents : cents;
    return std::wstring(sign) + aimeter::Money(magnitude) + L" CNY";
}

void Draw(const aimeter::AimeterData& data, const wg::Theme& t, wg::Canvas& c) {
    c.Panel(t, false, true);

    c.Text(L"DEEPSEEK", 8.0f, 8.0f, t.label, wg::kTextScale);
    c.TextRight(data.balanceText.empty() ? L"--" : data.balanceText.c_str(), 392, 8.0f,
                t.text, wg::kTextScale);

    c.Text(L"TOKENS TODAY", 8.0f, 32.0f, t.faint, 1.0f);

    const unsigned long long total = data.tokens.total();
    if (data.tokensOk) {
        const std::wstring tokenText = aimeter::FormatTokens(total);
        c.Text(tokenText, 8.0f, 42.0f, t.text, 3.0f);
        c.Text(L"TOK", 8.0f + c.TextWidth(tokenText, 3.0f) + 6.0f, 47.0f, t.faint, 1.5f);
        c.TextRight(L"LOCAL", 392, 47.0f, t.label, 1.0f);

        const int barX = 8, barY = 76, barW = 384;
        if (total == 0) {
            c.Fill(barX, barY, barW, 6, t.track);
        } else {
            const int w1 = (int)((double)barW * data.tokens.uncachedInput / total);
            const int w2 = (int)((double)barW * data.tokens.cacheRead / total);
            const int w3 = (int)((double)barW * data.tokens.cacheWrite / total);
            const int w4 = barW - w1 - w2 - w3;
            c.Fill(barX, barY, w1, 6, kInput);
            c.Fill(barX + w1, barY, w2, 6, t.ok);
            c.Fill(barX + w1 + w2, barY, w3, 6, kWrite);
            c.Fill(barX + w1 + w2 + w3, barY, w4, 6, t.warn);
        }

        struct Item {
            const wchar_t* name;
            unsigned long long value;
            wg::Color color;
        };
        const Item items[4] = {
            {L"INPUT", data.tokens.uncachedInput, kInput},
            {L"CACHE", data.tokens.cacheRead, t.ok},
            {L"WRITE", data.tokens.cacheWrite, kWrite},
            {L"OUTPUT", data.tokens.output, t.warn},
        };

        int x = 8;
        for (const Item& item : items) {
            c.Fill(x, 90, 3, 3, item.color);
            c.Text(item.name, (float)(x + 6), 88.0f, t.faint, 1.0f);
            const int nameW = (int)c.TextWidth(item.name, 1.0f) + 6;
            const std::wstring value = aimeter::FormatTokens(item.value);
            c.Text(value, (float)(x + 6 + nameW + 2), 88.0f, t.label, 1.0f);
            x += 6 + nameW + 2 + (int)c.TextWidth(value, 1.0f) + 14;
        }
    } else {
        c.Text(L"N/A", 8.0f, 42.0f, t.text, 3.0f);
        c.Text(L"NO DSH SESSION LOG", 8.0f, 76.0f, t.faint, 1.0f);
    }

    c.HLine(8, 392, 104, t.border);
    c.Text(L"SPEND TODAY", 8.0f, 112.0f, t.label, wg::kTextScale);
    c.TextRight(data.balanceOk ? SpendText(data.spendCents).c_str() : L"--", 392, 112.0f,
                t.text, wg::kTextScale);

    c.Text(L"CACHE HIT", 8.0f, 134.0f, t.faint, 1.0f);
    if (data.tokensOk) {
        const unsigned long long prompt =
            data.tokens.uncachedInput + data.tokens.cacheRead + data.tokens.cacheWrite;
        if (prompt > 0) {
            wchar_t pctText[16];
            const double pct = 100.0 * (double)data.tokens.cacheRead / (double)prompt;
            swprintf(pctText, 16, L"%.1f%%", pct);
            c.TextRight(pctText, 392, 134.0f, t.label, 1.0f);
        } else {
            c.TextRight(L"--", 392, 134.0f, t.label, 1.0f);
        }
    } else {
        c.TextRight(L"--", 392, 134.0f, t.label, 1.0f);
    }

    c.HLine(8, 392, 148, t.border);
    c.Text(data.tokensOk ? L"TOKENS  DSH LOG - THIS MACHINE" : L"TOKENS  NO LOCAL SOURCE",
           8.0f, 156.0f, t.faint, 1.0f);
    c.Text(L"SPEND   DEEPSEEK ACCOUNT", 8.0f, 170.0f, t.faint, 1.0f);
    c.TextRight(L"ESC CLOSE", 392, 170.0f, t.faint, 1.0f);
}

} // namespace

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

void DetailsPanel::Toggle(const aimeter::AimeterData& data, const wg::Theme& theme,
                          wg::Store& store, int widgetHeight) {
    if (visible()) {
        Hide();
        return;
    }
    Show(data, theme, store, widgetHeight);
}

void DetailsPanel::Show(const aimeter::AimeterData& data, const wg::Theme& theme,
                        wg::Store& store, int widgetHeight) {
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
                                L"AiMeter Details", WS_POPUP, 0, 0, kWidth, kHeight, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd_) {
            g_panel = nullptr;
            return;
        }
    }

    Place(store, widgetHeight);
    Redraw(data, theme);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
}

void DetailsPanel::Place(wg::Store& store, int widgetHeight) {
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int widgetW = 280;
    const int widgetH = widgetHeight > 0 ? widgetHeight : 71;

    int widgetX = (int)store.GetInt(L"x", screenW - widgetW - 40);
    int widgetY = (int)store.GetInt(L"y", screenH - widgetH - 80);

    int x = widgetX + widgetW - kWidth;
    int y = widgetY - kHeight - 8;
    if (y < 8) y = widgetY + widgetH + 8;
    if (x < 8) x = 8;
    if (x + kWidth > screenW - 8) x = screenW - kWidth - 8;
    if (y + kHeight > screenH - 8) y = screenH - kHeight - 8;
    if (y < 8) y = 8;

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, kWidth, kHeight, SWP_NOACTIVATE);
}

void DetailsPanel::Redraw(const aimeter::AimeterData& data, const wg::Theme& theme) {
    if (!hwnd_) return;
    if (!canvas_.Resize(kWidth, kHeight)) return;

    canvas_.Clear();
    Draw(data, theme, canvas_);

    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    canvas_.Present(hwnd_, rc.left, rc.top);
}

void DetailsPanel::Refresh(const aimeter::AimeterData& data, const wg::Theme& theme) {
    if (visible()) Redraw(data, theme);
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
