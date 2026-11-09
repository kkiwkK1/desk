// wg::DrawList - the "label / value / progress" layout shared by every widget
// in this repo. Build a vector<Row> in your data struct and this draws it.
#pragma once

#include "widget/canvas.h"
#include "widget/geom.h"

#include <string>
#include <vector>

namespace wg {

struct ListStyle {
    int   pad       = 8;   // inner padding on all sides
    int   rowPitch  = 33;  // vertical distance between rows
    int   barHeight = 4;
    int   barGap    = 8;   // gap between a row's text and its bar/note
    float scale     = kTextScale;

    int TextHeight() const { return (int)(7.0f * scale); }
};

struct Row {
    std::wstring label;          // left, dim
    std::wstring value;          // right, bright
    std::wstring note;           // small right-aligned caption; hides the bar
    float        frac  = -1.0f;  // 0..1 progress; < 0 = no bar
    Color        accent = 0;     // 0 = theme.ok
    std::wstring noteLeft;       // optional left caption on the same line
    Color        noteColor = 0;  // note color; 0 = theme.faint
    Color        valueColor = 0; // 0 = theme.text
};

// Exact height the widget needs for this many rows (bar included).
int ListHeight(const ListStyle& style, size_t rows);

// Rows only, inside an arbitrary rectangle: no outer panel. Board cards use
// this so their body can be clipped to the card.
void DrawRows(Canvas& canvas, const Rect& area, const std::vector<Row>& rows,
              const ListStyle& style, const Theme& theme);

// Panel + rows. DrawList uses the canvas width, so it fills whatever surface
// the host gave you.
void DrawList(Canvas& canvas, const std::vector<Row>& rows, const ListStyle& style,
              const Theme& theme, bool editing);

} // namespace wg
