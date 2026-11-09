// Input-path probe: feeds Chinese text through the real PromptText control via
// the clipboard (Ctrl+V), then Enter, and verifies the returned string.
// The widget's "Ask AiMini..." dialog is exactly this control.
#include "widget/prompt.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

constexpr const wchar_t* kSample = L"我的 C 盘还剩多少空间？";

std::string Utf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string out((std::size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), out.data(), size, nullptr,
                        nullptr);
    return out;
}

void SetClipboardText(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void* dst = GlobalLock(mem);
        if (dst) {
            std::memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
    }
    CloseClipboard();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring mode = argc > 1 ? argv[1] : L"char";
    if (mode == L"paste") SetClipboardText(kSample);

    std::thread driver([mode] {
        HWND hwnd = nullptr;
        for (int i = 0; i < 80 && !hwnd; ++i) {
            hwnd = FindWindowW(L"wg.Prompt", L"Input");
            if (!hwnd) Sleep(50);
        }
        if (!hwnd) return;

        Sleep(200);
        if (mode == L"paste") {
            PostMessageW(hwnd, WM_PASTE, 0, 0);
        } else {
            // Feed the exact WM_CHAR stream an IME commit produces.
            for (const wchar_t* p = kSample; *p; ++p) {
                PostMessageW(hwnd, WM_CHAR, (WPARAM)*p, 0);
                Sleep(30);
            }
        }
        Sleep(250);
        PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0);

        // Watchdog: never leave the probe blocked on a modal dialog.
        for (int i = 0; i < 100; ++i) {
            Sleep(100);
            if (!IsWindowVisible(hwnd)) return;
        }
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    });

    wg::PromptSpec spec;
    spec.label = L"PASTE TEST";
    spec.hint = L"CTRL+V   ENTER OK";
    spec.width = 460;

    std::wstring out;
    const bool ok = wg::PromptText(spec, out);
    driver.join();

    const std::string utf8 = Utf8(out);
    std::printf("mode=%s ok=%d text=%s\n", Utf8(mode).c_str(), ok ? 1 : 0, utf8.c_str());
    return (ok && out == kSample) ? 0 : 1;
}
