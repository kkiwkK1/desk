#include "data/ps/selection.h"

#include <algorithm>

namespace ps {
namespace {

bool Before(const Pos& a, const Pos& b) {
    return a.id != b.id ? a.id < b.id : a.col < b.col;
}

int ClampCol(int col, std::size_t length) {
    if (col < 0) return 0;
    if ((std::size_t)col > length) return (int)length;
    return col;
}

std::wstring RowText(const Row& row, int from, int to) {
    const int n = (int)row.cells.size();
    from = ClampCol(from, (std::size_t)n);
    to = ClampCol(to, (std::size_t)n);
    if (to <= from) return std::wstring();

    std::wstring out;
    out.reserve((std::size_t)(to - from));
    for (int i = from; i < to; ++i) {
        const Cell& cell = row.cells[(std::size_t)i];
        if (cell.wideTail) continue;
        out.push_back(cell.ch == 0 ? L' ' : cell.ch);
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

} // namespace

bool Normalize(const Selection& sel, Pos* from, Pos* to) {
    const Pos& a = sel.anchor;
    const Pos& b = sel.focus;
    const Pos& first = Before(a, b) ? a : b;
    const Pos& last = Before(a, b) ? b : a;
    if (first.id == last.id && first.col == last.col) return false;
    if (from) *from = first;
    if (to) *to = last;
    return true;
}

std::wstring ExtractText(const std::vector<Row>& rows, const Selection& sel) {
    Pos from;
    Pos to;
    if (!Normalize(sel, &from, &to)) return std::wstring();

    std::wstring out;
    for (const Row& row : rows) {
        if (row.id < from.id || row.id > to.id) continue;
        const int begin = row.id == from.id ? from.col : 0;
        const int end = row.id == to.id ? to.col : (int)row.cells.size();
        if (!out.empty()) out += L"\r\n";
        out += RowText(row, begin, end);
    }
    return out;
}

} // namespace ps
