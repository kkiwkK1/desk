// TERMINAL card: paints a ps::View (grid + cursor) and maps pixels back to a
// cell. Font-free of state, so the live card and the offline render test share
// the same code.
#pragma once

#include "data/ps/selection.h"
#include "data/ps/terminal.h"

#include "widget/canvas.h"
#include "widget/module.h"
#include "widget/skin.h"

#include <string>

namespace ps_view {

// Where a click landed.
struct Hit {
    bool ok = false;
    ps::Pos pos;
    int row = 0;
};

// Card geometry is fixed by the terminal grid: 80 columns * 7px + 2 * 4px of
// console padding = 568px (the Wide preset width), and 8/24 rows * 14px + the
// same padding = 120/344px of body. constexpr so ps_view.cpp can static_assert
// that the grid, the Wide preset and the 48px card chrome agree; the board
// hands the module exactly BodyHeight() pixels because that height is a
// multiple of the 8px card step, which is what lets PaintBody, HitTest and
// CaretPos share one rect.
constexpr int BodyHeight(bool expanded) {
    const int rows = expanded ? ps::kRowsExpanded : ps::kRowsCollapsed;
    return rows * wg::skin::kCellH + wg::skin::kConsolePad * 2;
}
// 80 cells plus the console frame.
constexpr int CardWidth() {
    return ps::kCols * wg::skin::kCellW + wg::skin::kConsolePad * 2;
}

void DrawBody(const ps::View& view, const ps::Selection& selection, bool focused,
              bool caretOn, wg::Canvas& canvas, const wg::Rect& body, const wg::Theme& theme);

Hit HitTest(const ps::View& view, int x, int y, const wg::Rect& body);
bool BottomHit(const ps::View& view, int x, int y, const wg::Rect& body);

// Cursor cell in body-local pixels (IME composition anchor).
bool CellPoint(const ps::View& view, const wg::Rect& body, int* x, int* y);

// '\u2193n' while scrolled back, otherwise the shell label.
wg::StatusChip Status(bool running, bool started, const std::wstring& label,
                      int scrollOffset, const wg::Theme& theme);

} // namespace ps_view
