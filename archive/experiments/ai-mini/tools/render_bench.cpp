// Real-path render bench: Canvas::Clear + cjk::Text, 1000 frames.
#include "widget/canvas.h"
#include "widget/cjk.h"

#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <string>

namespace {

long long WorkingSetKb() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
    return (long long)pmc.WorkingSetSize / 1024;
}

double Bench(wg::Canvas& canvas, const std::wstring& text, const wg::Theme& theme) {
    const ULONGLONG start = GetTickCount64();
    for (int pass = 0; pass < 1000; ++pass) {
        canvas.Clear();
        for (int line = 0; line < 4; ++line) {
            wg::cjk::Text(canvas, text, 8.0f, 8.0f + line * 15.0f, theme.text, 1.5f);
        }
    }
    return (double)(GetTickCount64() - start) / 1000.0;
}

} // namespace

int wmain() {
    wg::Canvas canvas;
    if (!canvas.Resize(300, 160)) return 1;

    wg::Theme theme;
    const std::wstring chinese =
        L"你的 C 盘剩余 329.3 GB，总空间 450.0 GB，约占 73%。";
    const std::wstring ascii =
        L"Your C: drive has 329.3 GB free of 450.0 GB, which is 73% free.";

    const long long wsStart = WorkingSetKb();

    // Warm the glyph cache so the measured frames are the steady state.
    for (int i = 0; i < 50; ++i) {
        canvas.Clear();
        wg::cjk::Text(canvas, chinese, 8.0f, 8.0f, theme.text, 1.5f);
    }
    const long long wsWarm = WorkingSetKb();

    const double cjkMs = Bench(canvas, chinese, theme);
    const double asciiMs = Bench(canvas, ascii, theme);

    std::printf("cjk:   %.3f ms/frame (4 lines)\n", cjkMs);
    std::printf("ascii: %.3f ms/frame (4 lines)\n", asciiMs);
    std::printf("working set: start %lld KB, after cjk warmup %lld KB (delta %lld KB)\n",
                wsStart, wsWarm, wsWarm - wsStart);
    return 0;
}
