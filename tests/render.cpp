// Headless check: renders the standard list layout without a window and writes
// a BMP, so layout/font regressions are visible in CI or on a build machine.
#include "bmp.h"
#include "widget/view.h"

#include <cstdio>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    const wchar_t* path = argc > 1 ? argv[1] : L"build\\preview.bmp";

    wg::Theme theme;
    wg::ListStyle style;
    const std::vector<wg::Row> rows = {
        wg::Row{L"DEEPSEEK", L"CNY 6.75", L"1.24M TOK  LOCAL", -1.0f, 0,
                L"TODAY -0.45", theme.label},
        wg::Row{L"CODEX", L"6% / 1%", L"", 0.06f, 0, L"", 0},
    };

    const int height = wg::ListHeight(style, rows.size());
    if (height != 71) {
        wprintf(L"[test] unexpected height: %d\n", height);
        return 1;
    }

    wg::Canvas canvas;
    if (!canvas.Resize(280, height)) return 1;
    canvas.Clear();
    wg::DrawList(canvas, rows, style, theme, false);

    if (!test::WriteBmp(path, canvas, wg::Rgba(0x20242C))) {
        wprintf(L"[test] cannot write %ls\n", path);
        return 1;
    }
    wprintf(L"[test] wrote %ls (%dx%d)\n", path, canvas.width(), canvas.height());
    return 0;
}
