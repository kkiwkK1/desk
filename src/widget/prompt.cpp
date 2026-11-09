#include "widget/prompt.h"

#include "widget/canvas.h"
#include "widget/cjk.h"

#include <windows.h>
#include <imm.h>

namespace wg {
namespace {

constexpr int kHeight = 92;
constexpr int kFieldX = 10;
constexpr int kFieldY = 34;
constexpr int kFieldH = 24;

struct PromptState {
    const PromptSpec* spec = nullptr;
    Canvas canvas;
    std::wstring input;
    bool done = false;
    bool ok = false;
};

PromptState* g_state = nullptr;
HWND g_dialog = nullptr;   // re-entrancy guard: prompt is modal by design

std::wstring Masked(const std::wstring& s) {
    if (s.size() <= 8) return std::wstring(s.size(), L'*');
    return s.substr(0, 4) + L"***" + s.substr(s.size() - 4);
}

void Repaint(PromptState& st, HWND hwnd) {
    const PromptSpec& spec = *st.spec;
    const int w = spec.width;

    if (!st.canvas.ready() || st.canvas.width() != w || st.canvas.height() != kHeight) {
        if (!st.canvas.Resize(w, kHeight)) return;
    }
    st.canvas.Clear();

    Theme theme;
    st.canvas.Panel(theme, true, true);
    st.canvas.Text(spec.label, 8.0f, 12.0f, theme.text, kTextScale);
    if (spec.saved) st.canvas.TextRight(L"SAVED", w - 8, 15.0f, theme.ok, 1.0f);

    const int fieldW = w - kFieldX * 2;
    st.canvas.Fill(kFieldX, kFieldY, fieldW, kFieldH, Rgba(0x15151C));
    st.canvas.Rect(kFieldX, kFieldY, fieldW, kFieldH, Rgba(0x33333F));

    const std::wstring shown = spec.mask ? Masked(st.input) : st.input;
    cjk::Text(st.canvas, shown, (float)kFieldX + 6, (float)kFieldY + 7, Rgba(0xE8E8EE),
              kTextScale);

    const float caretX =
        (float)kFieldX + 6 + cjk::TextWidth(shown, kTextScale) + 2.0f;
    st.canvas.Fill((int)caretX, kFieldY + 6, 1, (int)(7.0f * kTextScale) + 2, theme.borderEdit);

    st.canvas.Text(spec.hint, 8.0f, 66.0f, theme.faint, 1.0f);

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    st.canvas.Present(hwnd, rc.left, rc.top);
}

void Paste(PromptState& st) {
    if (!OpenClipboard(nullptr)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            for (const wchar_t* q = p; *q && st.input.size() < 256; ++q) {
                // Anything printable, including Han; a pasted newline becomes
                // a space because the field is single-line.
                wchar_t c = *q;
                if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
                if (c >= 32 && c != 127) st.input.push_back(c);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PromptState* st = g_state;
    (void)lp;

    switch (msg) {
    case WM_KEYDOWN:
        if (!st) return 0;
        if (wp == VK_ESCAPE) {
            st->done = true;
            st->ok = false;
            return 0;
        }
        if (wp == VK_RETURN) {
            st->done = true;
            st->ok = true;
            return 0;
        }
        if (wp == VK_BACK) {
            if (!st->input.empty()) {
                st->input.pop_back();
                // Keep surrogate pairs (emoji, rare CJK) whole.
                if (st->input.size() >= 2 &&
                    st->input.back() >= 0xDC00 && st->input.back() <= 0xDFFF) {
                    const wchar_t high = st->input[st->input.size() - 2];
                    if (high >= 0xD800 && high <= 0xDBFF) st->input.pop_back();
                }
            }
            Repaint(*st, hwnd);
            return 0;
        }
        if (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            Paste(*st);
            Repaint(*st, hwnd);
            return 0;
        }
        return 0;

    case WM_PASTE:
        if (st) {
            Paste(*st);
            Repaint(*st, hwnd);
        }
        return 0;

    case WM_CHAR:
        // WM_CHAR carries UTF-16: accepting the whole printable range is what
        // makes Chinese (via paste or IME) work. Surrogates arrive as two
        // messages and are appended in order.
        if (st && wp >= 32 && wp != 127 && st->input.size() < 256) {
            st->input.push_back((wchar_t)wp);
            Repaint(*st, hwnd);
        }
        return 0;

    case WM_IME_STARTCOMPOSITION: {
        if (!st) return 0;
        HIMC himc = ImmGetContext(hwnd);
        if (himc) {
            const std::wstring shown = st->spec->mask ? Masked(st->input) : st->input;
            COMPOSITIONFORM cf{};
            cf.dwStyle = CFS_POINT;
            cf.ptCurrentPos.x = kFieldX + 6 + (LONG)cjk::TextWidth(shown, kTextScale);
            cf.ptCurrentPos.y = kFieldY + 6;
            ImmSetCompositionWindow(himc, &cf);
            ImmReleaseContext(hwnd, himc);
        }
        return 0;
    }

    case WM_CLOSE:
        if (st) {
            st->done = true;
            st->ok = false;
        }
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

void RegisterPromptClass() {
    static bool registered = false;
    if (registered) return;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PromptProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"wg.Prompt";
    RegisterClassExW(&wc);
    registered = true;
}

} // namespace

bool PromptText(const PromptSpec& spec, std::wstring& out) {
    if (g_dialog) {
        SetForegroundWindow(g_dialog);
        return false;
    }

    PromptState st;
    st.spec = &spec;
    st.input = spec.initial;
    if (!st.canvas.Resize(spec.width, kHeight)) return false;

    RegisterPromptClass();

    const int x = (GetSystemMetrics(SM_CXSCREEN) - spec.width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - kHeight) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST,
                                L"wg.Prompt", L"Input", WS_POPUP, x, y, spec.width, kHeight,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return false;

    g_state = &st;
    g_dialog = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    Repaint(st, hwnd);

    MSG msg;
    while (!st.done) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage(0);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_state = nullptr;
    g_dialog = nullptr;
    DestroyWindow(hwnd);

    if (!st.ok) return false;

    size_t begin = st.input.find_first_not_of(L" \t");
    if (begin == std::wstring::npos) return false;
    size_t end = st.input.find_last_not_of(L" \t");
    out = st.input.substr(begin, end - begin + 1);
    return !out.empty();
}

} // namespace wg
