#include "widget/view.h"

namespace wg {

int ListHeight(const ListStyle& style, size_t rows) {
    if (rows == 0) rows = 1;
    const int content = style.TextHeight() + style.barGap + style.barHeight;
    return style.pad * 2 + (int)(rows - 1) * style.rowPitch + content;
}

void DrawRows(Canvas& canvas, const Rect& area, const std::vector<Row>& rows,
              const ListStyle& style, const Theme& theme) {
    const int left = area.x + style.pad;
    const int right = area.x + area.w - style.pad;
    int y = area.y + style.pad;

    for (const Row& row : rows) {
        canvas.Text(row.label, (float)left, (float)y, theme.label, style.scale);
        canvas.TextRight(row.value, right, (float)y,
                         row.valueColor ? row.valueColor : theme.text, style.scale);

        const int barY = y + style.TextHeight() + style.barGap;
        if (!row.note.empty() || !row.noteLeft.empty()) {
            if (!row.noteLeft.empty()) {
                canvas.Text(row.noteLeft, (float)left, (float)barY, theme.faint, 1.0f);
            }
            if (!row.note.empty()) {
                canvas.TextRight(row.note, right, (float)barY,
                                 row.noteColor ? row.noteColor : theme.faint, 1.0f);
            }
        } else {
            canvas.Bar(left, barY, right - left, style.barHeight, row.frac,
                       row.accent ? row.accent : theme.ok, theme.track);
        }
        y += style.rowPitch;
    }
}

void DrawList(Canvas& canvas, const std::vector<Row>& rows, const ListStyle& style,
              const Theme& theme, bool editing) {
    canvas.Panel(theme, editing);
    DrawRows(canvas, Rect{0, 0, canvas.width(), canvas.height()}, rows, style, theme);
}

} // namespace wg
