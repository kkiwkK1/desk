#include "widget/skin.h"

#include <algorithm>
#include <cmath>

namespace wg {
namespace skin {
namespace {

const Color kBoardTint = Rgba(0x101218, 100);
const Color kBoardBorder = Rgba(0x6C7280, 235);
const Color kBoardRim = Rgba(0xA2A9B8, 90);
const Color kBoardDotColor = Rgba(0xC5CDD7, 40);
const Color kBoardTitle = Rgba(0xC7CAD1);
const Color kGrip = Rgba(0x858B98);
const Color kAddBorder = Rgba(0x747B89, 190);
const Color kAddText = Rgba(0x9BA0AC);

// A pixel-stepped rounded rectangle. The cost is ~2*radius fills, not an
// anti-aliased path: this keeps the pixel-art look and the per-frame budget.
void ShapeFill(Canvas& canvas, const Rect& r, int radius, Color color) {
    if (r.w <= 0 || r.h <= 0) return;
    radius = std::max(0, std::min(radius, std::min(r.w, r.h) / 2));
    if (radius == 0) {
        canvas.Fill(r.x, r.y, r.w, r.h, color);
        return;
    }

    // Straight middle band: full width, so the left/right edges (and the
    // border along them) are not left as transparent strips.
    canvas.Fill(r.x, r.y + radius, r.w, r.h - radius * 2, color);
    for (int i = 0; i < radius; ++i) {
        const int dy = radius - i;
        const int inset =
            radius - (int)std::lround(std::sqrt((double)(radius * radius - dy * dy)));
        const int x0 = r.x + std::max(0, inset);
        const int w = r.w - 2 * std::max(0, inset);
        if (w <= 0) continue;
        canvas.Fill(x0, r.y + i, w, 1, color);
        canvas.Fill(x0, r.bottom() - 1 - i, w, 1, color);
    }
}

void RoundRect(Canvas& canvas, const Rect& r, int radius, Color fill, Color border,
               int borderWidth) {
    if (borderWidth <= 0) {
        ShapeFill(canvas, r, radius, fill);
        return;
    }
    ShapeFill(canvas, r, radius, border);
    Rect inner{r.x + borderWidth, r.y + borderWidth,
               r.w - borderWidth * 2, r.h - borderWidth * 2};
    if (inner.w > 0 && inner.h > 0) {
        ShapeFill(canvas, inner, std::max(0, radius - borderWidth), fill);
    }
}

void Grip(Canvas& canvas, int x, int y, Color color) {
    for (int i = 0; i < 3; ++i) {
        canvas.Fill(x, y + i * 3, 1, 2, color);
        canvas.Fill(x + 4, y + i * 3, 1, 2, color);
    }
}

void DashedH(Canvas& canvas, int x0, int x1, int y, Color color) {
    for (int x = x0; x < x1; x += 8) {
        canvas.HLine(x, std::min(x + 4, x1), y, color);
    }
}

void DashedV(Canvas& canvas, int x, int y0, int y1, Color color) {
    for (int y = y0; y < y1; y += 8) {
        canvas.Fill(x, y, 1, std::min(4, y1 - y), color);
    }
}

} // namespace

int CardHeight(int bodyH) {
    return kCardHeaderH + kCardPadTop + bodyH + kCardPadBottom;
}

Rect CardBody(const Rect& card) {
    Rect body;
    body.x = card.x;
    body.y = card.y + kCardHeaderH + kCardPadTop;
    body.w = card.w;
    body.h = std::max(0, card.h - kCardHeaderH - kCardPadTop - kCardPadBottom);
    return body;
}

int RowsHeight(int rows) {
    return std::max(rows, 0) * kRowPitch;
}

void BoardChrome(Canvas& canvas, const Rect& board, const std::wstring& title,
                 const std::wstring& hint, const Theme& theme, bool grid, int dotGrid) {
    RoundRect(canvas, board, 10, kBoardTint, kBoardBorder, 2);
    // A thin top rim keeps the 2px border from looking flat.
    canvas.HLine(board.x + 14, board.right() - 14, board.y + 2, kBoardRim);

    if (grid) {
        const int spacing = std::clamp(dotGrid, 2, 256);
        for (int y = board.y + kBoardTitleH + 4; y < board.bottom() - 8; y += spacing) {
            for (int x = board.x + 10; x < board.right() - 8; x += spacing) {
                canvas.Fill(x, y, 1, 1, kBoardDotColor);
            }
        }
    }

    Grip(canvas, board.x + 12, board.y + 11, kGrip);
    canvas.Text(title, (float)(board.x + 24), (float)(board.y + 10), kBoardTitle, 1.0f);
    if (!hint.empty()) {
        canvas.TextRight(hint, board.right() - 14, (float)(board.y + 10), theme.faint, 1.0f);
    }

    for (int i = 0; i < 3; ++i) {
        canvas.Fill(board.right() - 16 + i * 4, board.bottom() - 10, 3, 1, kGrip);
        canvas.Fill(board.right() - 10, board.bottom() - 16 + i * 4, 1, 3, kGrip);
    }
}

void CardChrome(Canvas& canvas, const Rect& card, const std::wstring& title,
                const std::wstring& status, Color statusColor, const Theme& theme,
                bool hasDetails, bool expanded, bool hovered, bool dragging) {
    const Color border = dragging ? theme.borderEdit : theme.border;
    RoundRect(canvas, card, 6, theme.panelSolid, border, 1);

    const Color grip = dragging ? theme.borderEdit : (hovered ? theme.label : kGrip);
    Grip(canvas, card.x + 10, card.y + 11, grip);

    canvas.Text(title, (float)(card.x + 22), (float)(card.y + 9), theme.text, kTitleScale);

    const int right = card.right() - 12;
    if (hasDetails) {
        canvas.Text(expanded ? L"-" : L"+", (float)right, (float)(card.y + 11),
                    hovered ? theme.label : theme.faint, 1.0f);
    }
    const int statusRight = hasDetails ? right - 12 : right;
    if (!status.empty()) {
        const int sx = statusRight - (int)canvas.TextWidth(status, 1.0f);
        const Color color = statusColor ? statusColor : theme.faint;
        canvas.Fill(sx - 7, card.y + 13, 3, 3, color);
        canvas.Text(status, (float)sx, (float)(card.y + 11), color, 1.0f);
    }

    canvas.HLine(card.x + 12, right, card.y + kCardHeaderH, theme.border);
    if (hovered && !dragging) {
        canvas.HLine(card.x + 12, right, card.y + kCardHeaderH - 1,
                     Rgba(0x60A5FA, 110));
    }
}

void Rows(Canvas& canvas, const Rect& body, const std::vector<Row>& rows,
          const Theme& theme) {
    const int left = body.x + kCardPadX;
    const int right = body.right() - kCardPadX;
    int y = body.y;

    for (const Row& row : rows) {
        canvas.Text(row.label, (float)left, (float)y, theme.label, kValueScale);
        canvas.TextRight(row.value, right, (float)y,
                         row.valueColor ? row.valueColor : theme.text, kValueScale);

        if (!row.note.empty() || !row.noteLeft.empty()) {
            if (!row.noteLeft.empty()) {
                canvas.Text(row.noteLeft, (float)left, (float)(y + 16), theme.faint,
                            kMetaScale);
            }
            if (!row.note.empty()) {
                canvas.TextRight(row.note, right, (float)(y + 16),
                                 row.noteColor ? row.noteColor : theme.faint, kMetaScale);
            }
        } else {
            canvas.Bar(left, y + 24, right - left, 4, row.frac,
                       row.accent ? row.accent : theme.ok, theme.track);
        }
        y += kRowPitch;
    }
}

void Divider(Canvas& canvas, int x0, int x1, int y, const Theme& theme) {
    canvas.HLine(x0, x1, y, theme.border);
}

void ConsoleFrame(Canvas& canvas, const Rect& body, const Theme& theme) {
    // Near-opaque so the board's dot grid does not read through the text, but
    // one step darker than a card so the body reads as a screen, not a panel.
    canvas.Fill(body.x, body.y, body.w, body.h, kConsoleBg);
    canvas.Rect(body.x, body.y, body.w, body.h, theme.track);
}

Rect ConsoleInner(const Rect& body) {
    return Rect{body.x + kConsolePad, body.y + kConsolePad, body.w - kConsolePad * 2,
                body.h - kConsolePad * 2};
}

void AddSlot(Canvas& canvas, const Rect& slot, const Theme& theme) {
    (void)theme;
    DashedH(canvas, slot.x, slot.right(), slot.y, kAddBorder);
    DashedH(canvas, slot.x, slot.right(), slot.bottom() - 1, kAddBorder);
    DashedV(canvas, slot.x, slot.y, slot.bottom(), kAddBorder);
    DashedV(canvas, slot.right() - 1, slot.y, slot.bottom(), kAddBorder);

    canvas.TextCentered(L"+", slot.x + slot.w / 2, (float)(slot.y + slot.h / 2 - 20),
                        kAddBorder, kBigScale);
    canvas.TextCentered(L"ADD MODULE", slot.x + slot.w / 2,
                        (float)(slot.y + slot.h / 2 + 4), kAddText, 1.0f);
}

void MiniBars(Canvas& canvas, const Rect& rect, const std::vector<float>& values,
              Color fill, Color track) {
    if (rect.w <= 0 || rect.h <= 0 || values.empty()) return;

    const int count = (int)values.size();
    const int gap = 2;
    int barW = (rect.w - gap * (count - 1)) / count;
    if (barW < 1) barW = 1;

    float scaleMax = 100.0f;
    for (float v : values) {
        if (v > scaleMax) scaleMax = v;
    }
    if (scaleMax <= 0.0f) scaleMax = 100.0f;

    // Baseline: one track-coloured line so an all-empty week still reads as a
    // chart area rather than a blank strip.
    canvas.HLine(rect.x, rect.right(), rect.bottom() - 1, track);

    for (int i = 0; i < count; ++i) {
        const float v = values[i];
        if (v < 0.0f) continue;
        int h = (int)std::lround((double)v * (double)(rect.h - 1) / (double)scaleMax);
        h = std::clamp(h, 0, rect.h - 1);
        if (h <= 0) continue;
        const int x = rect.x + i * (barW + gap);
        canvas.Fill(x, rect.bottom() - 1 - h, barW, h, fill);
    }
}

std::wstring FitText(const Canvas& canvas, std::wstring text, int maxWidth, float scale) {
    if (maxWidth <= 0) return std::wstring();
    if ((int)canvas.TextWidth(text, scale) <= maxWidth) return text;

    const std::wstring dots = L"...";
    while (!text.empty() && (int)canvas.TextWidth(text + dots, scale) > maxWidth) {
        text.pop_back();
    }
    return text + dots;
}

} // namespace skin
} // namespace wg
