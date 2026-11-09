#include "ps_view.h"

#include "widget/cjk.h"
#include "widget/size.h"
#include "widget/skin.h"

#include <algorithm>
#include <cstdint>

namespace ps_view {

using namespace wg;

// Geometry self-check, compiled into every build and every test binary: the
// 80-column grid, the Wide preset width, the 8px card step and the 48px chrome
// must all agree with the 168/392px card heights (tests/desk_render.cpp asserts
// the same numbers at runtime). If a font token, the grid or the preset table
// is edited alone, this fails to compile instead of letting the board clip the
// terminal or hand it a body rect that is not exactly BodyHeight() pixels.
static_assert(ps::kCols == 80, "POWERSHELL is an 80-column terminal");
static_assert(CardWidth() == 568, "Wide preset width must equal the 80-cell grid");
static_assert(BodyHeight(false) == 120, "8 rows + padding must be a 120px body");
static_assert(BodyHeight(true) == 344, "24 rows + padding must be a 344px body");
static_assert(wg::kCardChromeHeight + BodyHeight(false) == 168,
              "collapsed card must be the Wide preset minimum (168px)");
static_assert(wg::kCardChromeHeight + BodyHeight(true) == 392,
              "expanded card must be 392px");
static_assert((wg::kCardChromeHeight + BodyHeight(false)) % wg::kCardSizeStep == 0,
              "card height must sit on the 8px step so the body is not padded");
static_assert((wg::kCardChromeHeight + BodyHeight(true)) % wg::kCardSizeStep == 0,
              "card height must sit on the 8px step so the body is not padded");

namespace {

const Color kSelectionFill = Rgba(0x1D4ED8, 150);
Rect BottomButton(const Rect& body) {
    return Rect{body.right() - 78, body.bottom() - skin::kConsolePad - 12, 72, 12};
}

// ANSI palette: 16 base colours matching the theme's semantics, then the xterm
// 6x6x6 cube and grey ramp.
Color PaletteColor(int index) {
    static const Color kBase[16] = {
        Rgba(0x101014), Rgba(0xF87171), Rgba(0x4ADE80), Rgba(0xFBBF24),
        Rgba(0x60A5FA), Rgba(0xC084FC), Rgba(0x22D3EE), Rgba(0xE5E7EB),
        Rgba(0x6B7280), Rgba(0xFCA5A5), Rgba(0x86EFAC), Rgba(0xFDE68A),
        Rgba(0x93C5FD), Rgba(0xD8B4FE), Rgba(0x67E8F9), Rgba(0xFFFFFF),
    };
    if (index >= 0 && index < 16) return kBase[index];

    if (index >= 16 && index <= 231) {
        static const int kSteps[6] = {0, 95, 135, 175, 215, 255};
        const int n = index - 16;
        const int r = kSteps[(n / 36) % 6];
        const int g = kSteps[(n / 6) % 6];
        const int b = kSteps[n % 6];
        return Rgba((std::uint32_t)((r << 16) | (g << 8) | b));
    }
    if (index >= 232 && index <= 255) {
        const int v = 8 + (index - 232) * 10;
        return Rgba((std::uint32_t)((v << 16) | (v << 8) | v));
    }
    return kBase[7];
}

// Screen colours are "default", "palette index" or 24-bit; resolve here.
Color Resolve(std::uint32_t color, bool foreground, const Theme& theme) {
    if (color == ps::kColorDefault) {
        return foreground ? theme.text : skin::kConsoleBg;
    }
    const std::uint32_t kind = color & 0xFF000000u;
    if (kind == ps::kColorPalette) return PaletteColor((int)(color & 0xFFu));
    if (kind == ps::kColorRgb) return 0xFF000000u | (color & 0x00FFFFFFu);
    return foreground ? theme.text : skin::kConsoleBg;
}

} // namespace

void DrawBody(const ps::View& view, const ps::Selection& selection, bool focused,
              bool caretOn, wg::Canvas& canvas, const wg::Rect& body, const wg::Theme& theme) {
    skin::ConsoleFrame(canvas, body, theme);
    const Rect inner = skin::ConsoleInner(body);
    if (inner.empty() || view.cols <= 0) return;

    canvas.SetClip(inner.x, inner.y, inner.w, inner.h);

    ps::Pos selFrom{};
    ps::Pos selTo{};
    const bool hasSelection = ps::Normalize(selection, &selFrom, &selTo);

    // One pass per row: background, selection wash, then the glyphs on top.
    for (int r = 0; r < view.rows && r < (int)view.lines.size(); ++r) {
        const ps::ViewRow& row = view.lines[(std::size_t)r];
        const int y = inner.y + r * skin::kCellH;

        if (hasSelection && row.id >= selFrom.id && row.id <= selTo.id) {
            const int from = row.id == selFrom.id ? selFrom.col : 0;
            const int to = row.id == selTo.id ? selTo.col : (int)row.cells.size();
            const int x0 = inner.x + std::clamp(from, 0, view.cols) * skin::kCellW;
            const int x1 = inner.x + std::clamp(to, 0, view.cols) * skin::kCellW;
            if (x1 > x0) canvas.Fill(x0, y, x1 - x0, skin::kCellH, kSelectionFill);
        }

        const int cells = std::min<int>((int)row.cells.size(), view.cols);
        for (int c = 0; c < cells; ++c) {
            const ps::Cell& cell = row.cells[(std::size_t)c];
            const int x = inner.x + c * skin::kCellW;

            const bool inverse = (cell.flags & ps::kFlagInverse) != 0;
            const Color fg = Resolve(cell.fg, !inverse, theme);
            const Color bg = Resolve(cell.bg, inverse, theme);

            if (cell.bg != ps::kColorDefault || inverse) {
                canvas.Fill(x, y, skin::kCellW, skin::kCellH, bg);
            }
            if (cell.wideTail || cell.ch == 0 || cell.ch < 32) continue;

            const Color glyph = fg;
            // cjk::TextTop anchors the black box at the cell top. A 14px CJK
            // mask placed on the 7px ASCII baseline (the old cjk::Text path)
            // started above the cell and the first terminal row was clipped.
            const int glyphTop = cjk::IsWide(cell.ch) ? y : y + (skin::kCellH - 9) / 2;
            cjk::TextTop(canvas, std::wstring_view(&cell.ch, 1), (float)x, (float)glyphTop,
                         glyph, skin::kConsoleScale);
            if ((cell.flags & ps::kFlagUnderline) != 0) {
                canvas.Fill(x, y + skin::kCellH - 2, skin::kCellW, 1, glyph);
            }
        }
    }

    if (focused && caretOn && view.cursorVisible) {
        const int cx = inner.x + view.cursorCol * skin::kCellW;
        const int cy = inner.y + view.cursorRow * skin::kCellH;
        canvas.Fill(cx, cy + 1, 2, skin::kCellH - 3, theme.borderEdit);
    }

    canvas.ResetClip();

    if (!view.alternate && view.scrollbackLines > 0) {
        // The track occupies only the existing right padding, not a text cell.
        const int trackY = inner.y;
        const int trackH = std::max(1, inner.h);
        const int total = view.scrollbackLines + view.rows;
        const int thumbH = std::max(8, trackH * view.rows / std::max(1, total));
        const int maxY = std::max(0, trackH - thumbH);
        const int thumbY = maxY * (view.scrollbackLines - view.scrollOffset) /
                           std::max(1, view.scrollbackLines);
        canvas.Fill(inner.right() + 1, trackY, 2, trackH, theme.track);
        canvas.Fill(inner.right() + 1, trackY + thumbY, 2, thumbH, theme.faint);
    }
    if (view.scrollOffset > 0) {
        const Rect button = BottomButton(body);
        canvas.Fill(button.x, button.y, button.w, button.h, skin::kConsoleBg);
        canvas.TextRight(L"\x2193 BOTTOM", button.right() - 3, (float)(button.y + 3),
                         theme.warn, 1.0f);
    }
}

Hit HitTest(const ps::View& view, int x, int y, const wg::Rect& body) {
    Hit hit;
    const Rect inner = skin::ConsoleInner(body);
    if (inner.empty() || view.cols <= 0) return hit;
    if (!inner.contains(x, y)) return hit;

    const int row = (y - inner.y) / skin::kCellH;
    const int col = (x - inner.x) / skin::kCellW;
    if (row < 0 || row >= (int)view.lines.size()) return hit;
    if (col < 0 || col >= view.cols) return hit;

    hit.ok = true;
    hit.pos.id = view.lines[(std::size_t)row].id;
    hit.pos.col = col;
    hit.row = row;
    return hit;
}

bool BottomHit(const ps::View& view, int x, int y, const Rect& body) {
    return view.scrollOffset > 0 && BottomButton(body).contains(x, y);
}

bool CellPoint(const ps::View& view, const wg::Rect& body, int* x, int* y) {
    if (view.cols <= 0) return false;
    const Rect inner = skin::ConsoleInner(body);
    if (x) *x = inner.x + std::clamp(view.cursorCol, 0, view.cols - 1) * skin::kCellW;
    if (y) *y = inner.y + std::clamp(view.cursorRow, 0, view.rows - 1) * skin::kCellH + 2;
    return true;
}

wg::StatusChip Status(bool running, bool started, const std::wstring& label,
                      int scrollOffset, const wg::Theme& theme) {
    if (scrollOffset > 0) {
        return wg::StatusChip{L"\x2193 " + std::to_wstring(scrollOffset), theme.warn};
    }
    if (running) {
        return wg::StatusChip{label.empty() ? std::wstring(L"READY") : label, theme.ok};
    }
    if (started) return wg::StatusChip{L"EXITED", theme.bad};
    return wg::StatusChip{L"NO SHELL", theme.bad};
}

} // namespace ps_view
