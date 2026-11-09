// Text selection over the terminal grid.
//
// Anchored to (row id, column): the screen hands every row a stable id when it
// is created, so new output, scrolling and the scrollback ring can never shift
// a selection out from under the mouse.
#pragma once

#include "data/ps/screen.h"

#include <string>
#include <vector>

namespace ps {

struct Pos {
    unsigned long long id = 0;
    int col = 0;
};

struct Selection {
    bool active = false;
    Pos anchor;
    Pos focus;

    void Clear() {
        active = false;
        anchor = Pos{};
        focus = Pos{};
    }
};

// Orders the two ends. False when nothing is selected.
bool Normalize(const Selection& sel, Pos* from, Pos* to);

// Selected text: rows oldest first, lines joined with CRLF, columns clamped to
// each row and trailing blanks trimmed. Rows that already left the ring are
// skipped.
std::wstring ExtractText(const std::vector<Row>& rows, const Selection& sel);

} // namespace ps
