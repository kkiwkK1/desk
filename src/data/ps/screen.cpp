#include "data/ps/screen.h"

#include "widget/cjk.h"

#include <algorithm>
#include <utility>

namespace ps {
namespace {

bool IsWide(wchar_t c) { return wg::cjk::IsWide(c); }

} // namespace

Screen::Screen(int cols, int rows) {
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    cols_ = cols;
    rows_ = rows;
    main_.resize((std::size_t)rows_);
    for (Row& row : main_) {
        row.id = NewRowId();
        row.cells.assign((std::size_t)cols_, Cell{});
    }
    alt_ = main_;
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
}

unsigned long long Screen::NewRowId() { return nextId_++; }

void Screen::BlankRow(int row) {
    Row& r = Grid()[(std::size_t)row];
    r.id = NewRowId();
    r.cells.assign((std::size_t)cols_, Cell{});
}

void Screen::ClearRange(int row, int from, int to) {
    from = std::max(0, from);
    to = std::min(cols_, to);
    for (int c = from; c < to; ++c) At(row, c) = Cell{};
}

void Screen::Rebuild(int cols, int rows, std::vector<Row>& grid) {
    std::vector<Row> out((std::size_t)rows);
    for (int r = 0; r < rows; ++r) {
        const bool have = r < (int)grid.size();
        out[(std::size_t)r].id = have && grid[(std::size_t)r].id != 0 ? grid[(std::size_t)r].id
                                                                     : NewRowId();
        out[(std::size_t)r].cells.assign((std::size_t)cols, Cell{});
        if (have) {
            const std::vector<Cell>& src = grid[(std::size_t)r].cells;
            const int copy = std::min<int>(cols, (int)src.size());
            for (int c = 0; c < copy; ++c) {
                out[(std::size_t)r].cells[(std::size_t)c] = src[(std::size_t)c];
            }
        }
    }
    grid.swap(out);
}

void Screen::Resize(int cols, int rows) {
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    if (cols == cols_ && rows == rows_) return;

    cols_ = cols;
    rows_ = rows;
    Rebuild(cols, rows, main_);
    Rebuild(cols, rows, alt_);

    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    row_ = std::min(row_, rows_ - 1);
    col_ = std::min(col_, cols_ - 1);
    wrapPending_ = false;
}

void Screen::PutAt(int col, wchar_t c, bool wideTail) {
    if (col < 0 || col >= cols_) return;
    Cell& cell = At(row_, col);
    cell = Cell{};
    cell.ch = c;
    cell.fg = attr_.fg;
    cell.bg = attr_.bg;
    cell.flags = attr_.flags;
    cell.wideTail = wideTail;
}

void Screen::Print(wchar_t c) {
    const int width = IsWide(c) ? 2 : 1;

    if (wrapPending_ && autoWrap_) {
        col_ = 0;
        LineFeed();
        wrapPending_ = false;
    }

    if (col_ + width > cols_) {
        if (autoWrap_) {
            col_ = 0;
            LineFeed();
        } else {
            col_ = std::max(0, cols_ - width);
        }
    }

    // Detach this cell from any wide pair it used to be part of: if we land on
    // a tail, its head must go; if our right neighbour is an orphaned tail, it
    // must go too.
    if (At(row_, col_).wideTail && col_ > 0) {
        Cell& head = At(row_, col_ - 1);
        if (head.ch != 0 && IsWide(head.ch)) head = Cell{};
    }
    if (col_ + width < cols_ && At(row_, col_ + width).wideTail) {
        At(row_, col_ + width) = Cell{};
    }

    PutAt(col_, c, false);
    if (width == 2 && col_ + 1 < cols_) PutAt(col_ + 1, 0, true);
    col_ += width;
    if (col_ >= cols_) {
        col_ = cols_ - 1;
        wrapPending_ = autoWrap_;
    }
}

void Screen::CarriageReturn() {
    col_ = 0;
    wrapPending_ = false;
}

void Screen::LineFeed() {
    wrapPending_ = false;
    if (row_ == scrollBottom_) {
        ScrollRegionUp(1);
    } else if (row_ < rows_ - 1) {
        ++row_;
    }
}

void Screen::NextLine() {
    CarriageReturn();
    LineFeed();
}

void Screen::ReverseIndex() {
    wrapPending_ = false;
    if (row_ == scrollTop_) {
        ScrollRegionDown(1);
    } else if (row_ > 0) {
        --row_;
    }
}

void Screen::Backspace() {
    if (col_ > 0) --col_;
    wrapPending_ = false;
}

void Screen::Tab() {
    const int stop = ((col_ / 8) + 1) * 8;
    col_ = std::min(cols_ - 1, stop);
    wrapPending_ = false;
}

void Screen::BackTab() {
    col_ = std::max(0, ((col_ - 1) / 8) * 8);
    wrapPending_ = false;
}

void Screen::MoveTo(int row, int col) {
    int r = std::clamp(row, 0, rows_ - 1);
    if (originMode_) {
        r = std::clamp(scrollTop_ + row, scrollTop_, scrollBottom_);
    }
    row_ = r;
    col_ = std::clamp(col, 0, cols_ - 1);
    wrapPending_ = false;
}

void Screen::MoveUp(int n) {
    const int top = row_ >= scrollTop_ && row_ <= scrollBottom_ ? scrollTop_ : 0;
    row_ = std::max(top, row_ - std::max(1, n));
    wrapPending_ = false;
}

void Screen::MoveDown(int n) {
    const int bottom = row_ >= scrollTop_ && row_ <= scrollBottom_ ? scrollBottom_ : rows_ - 1;
    row_ = std::min(bottom, row_ + std::max(1, n));
    wrapPending_ = false;
}

void Screen::MoveLeft(int n) {
    col_ = std::max(0, col_ - std::max(1, n));
    wrapPending_ = false;
}

void Screen::MoveRight(int n) {
    col_ = std::min(cols_ - 1, col_ + std::max(1, n));
    wrapPending_ = false;
}

void Screen::MoveToColumn(int col) {
    col_ = std::clamp(col, 0, cols_ - 1);
    wrapPending_ = false;
}

void Screen::MoveToRow(int row) {
    int r = std::clamp(row, 0, rows_ - 1);
    if (originMode_) r = std::clamp(scrollTop_ + row, scrollTop_, scrollBottom_);
    row_ = r;
    wrapPending_ = false;
}

void Screen::SaveCursor() {
    saved_.row = row_;
    saved_.col = col_;
    saved_.attr = attr_;
    saved_.wrapPending = wrapPending_;
}

void Screen::RestoreCursor() {
    row_ = std::clamp(saved_.row, 0, rows_ - 1);
    col_ = std::clamp(saved_.col, 0, cols_ - 1);
    attr_ = saved_.attr;
    wrapPending_ = saved_.wrapPending;
}

void Screen::ScrollRegionUp(int n) {
    n = std::max(1, n);
    const int top = std::clamp(scrollTop_, 0, rows_ - 1);
    const int bottom = std::clamp(scrollBottom_, top, rows_ - 1);
    for (int i = 0; i < n; ++i) {
        if (top == 0 && !alternate_) {
            scrollback_.push_back(Grid()[(std::size_t)top]);
            ++scrollbackWrites_;
            while (scrollback_.size() > kMaxScrollback) scrollback_.pop_front();
        }
        Grid().erase(Grid().begin() + top);
        Row fresh;
        fresh.id = NewRowId();
        fresh.cells.assign((std::size_t)cols_, Cell{});
        Grid().insert(Grid().begin() + bottom, std::move(fresh));
    }
}

void Screen::ScrollRegionDown(int n) {
    n = std::max(1, n);
    const int top = std::clamp(scrollTop_, 0, rows_ - 1);
    const int bottom = std::clamp(scrollBottom_, top, rows_ - 1);
    for (int i = 0; i < n; ++i) {
        Grid().erase(Grid().begin() + bottom);
        Row fresh;
        fresh.id = NewRowId();
        fresh.cells.assign((std::size_t)cols_, Cell{});
        Grid().insert(Grid().begin() + top, std::move(fresh));
    }
}

void Screen::ScrollUp(int n) { ScrollRegionUp(n); }
void Screen::ScrollDown(int n) { ScrollRegionDown(n); }

void Screen::SetScrollRegion(int top, int bottom) {
    if (top < 0) top = 0;
    if (bottom < 0) bottom = rows_ - 1;
    if (top > bottom) std::swap(top, bottom);
    scrollTop_ = std::clamp(top, 0, rows_ - 1);
    scrollBottom_ = std::clamp(bottom, scrollTop_, rows_ - 1);
    row_ = originMode_ ? scrollTop_ : 0;
    col_ = 0;
    wrapPending_ = false;
}

void Screen::ResetScrollRegion() {
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
}

void Screen::SetOriginMode(bool on) {
    originMode_ = on;
    row_ = on ? scrollTop_ : 0;
    col_ = 0;
    wrapPending_ = false;
}

void Screen::SetAlternateScreen(bool on) {
    if (on == alternate_) return;
    if (on) {
        SaveCursor();
        alt_.clear();
        alt_.resize((std::size_t)rows_);
        for (Row& row : alt_) {
            row.id = NewRowId();
            row.cells.assign((std::size_t)cols_, Cell{});
        }
        alternate_ = true;
    } else {
        alternate_ = false;
        RestoreCursor();
    }
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    wrapPending_ = false;
}

void Screen::EraseInDisplay(int mode) {
    if (mode == 2 || mode == 3) {
        for (int r = 0; r < rows_; ++r) BlankRow(r);
        if (mode == 3) scrollback_.clear();
        row_ = 0;
        col_ = 0;
        wrapPending_ = false;
        return;
    }
    if (mode == 1) {
        for (int r = 0; r < row_; ++r) BlankRow(r);
        ClearRange(row_, 0, col_ + 1);
        return;
    }
    // mode 0 (and anything unknown): cursor to the end.
    ClearRange(row_, col_, cols_);
    for (int r = row_ + 1; r < rows_; ++r) BlankRow(r);
}

void Screen::EraseInLine(int mode) {
    if (mode == 1) {
        ClearRange(row_, 0, col_ + 1);
    } else if (mode == 2) {
        ClearRange(row_, 0, cols_);
    } else {
        ClearRange(row_, col_, cols_);
    }
}

void Screen::EraseChars(int n) {
    n = std::max(1, n);
    ClearRange(row_, col_, std::min(col_ + n, cols_));
}

void Screen::InsertChars(int n) {
    n = std::clamp(n, 1, cols_ - col_);
    std::vector<Cell>& cells = Grid()[(std::size_t)row_].cells;
    cells.insert(cells.begin() + col_, (std::size_t)n, Cell{});
    cells.resize((std::size_t)cols_);
}

void Screen::DeleteChars(int n) {
    n = std::clamp(n, 1, cols_ - col_);
    std::vector<Cell>& cells = Grid()[(std::size_t)row_].cells;
    cells.erase(cells.begin() + col_, cells.begin() + col_ + n);
    cells.resize((std::size_t)cols_, Cell{});
}

void Screen::InsertLines(int n) {
    if (row_ < scrollTop_ || row_ > scrollBottom_) return;
    n = std::clamp(n, 1, scrollBottom_ - row_ + 1);
    std::vector<Row>& grid = Grid();
    for (int i = 0; i < n; ++i) {
        grid.erase(grid.begin() + scrollBottom_);
        Row fresh;
        fresh.id = NewRowId();
        fresh.cells.assign((std::size_t)cols_, Cell{});
        grid.insert(grid.begin() + row_, std::move(fresh));
    }
    col_ = 0;
    wrapPending_ = false;
}

void Screen::DeleteLines(int n) {
    if (row_ < scrollTop_ || row_ > scrollBottom_) return;
    n = std::clamp(n, 1, scrollBottom_ - row_ + 1);
    std::vector<Row>& grid = Grid();
    for (int i = 0; i < n; ++i) {
        grid.erase(grid.begin() + row_);
        Row fresh;
        fresh.id = NewRowId();
        fresh.cells.assign((std::size_t)cols_, Cell{});
        grid.insert(grid.begin() + scrollBottom_, std::move(fresh));
    }
    col_ = 0;
    wrapPending_ = false;
}

std::vector<Row> Screen::visibleRows() const { return Grid(); }

std::wstring Screen::lineText(unsigned long long id, int from, int to) const {
    const std::vector<Cell>* cells = nullptr;
    for (const Row& row : scrollback_) {
        if (row.id == id) {
            cells = &row.cells;
            break;
        }
    }
    if (!cells) {
        for (const Row& row : Grid()) {
            if (row.id == id) {
                cells = &row.cells;
                break;
            }
        }
    }
    if (!cells) return std::wstring();

    const int n = (int)cells->size();
    from = std::clamp(from, 0, n);
    to = std::clamp(to, from, n);

    std::wstring out;
    out.reserve((std::size_t)(to - from));
    for (int i = from; i < to; ++i) {
        const Cell& cell = (*cells)[(std::size_t)i];
        if (cell.wideTail) continue;
        out.push_back(cell.ch == 0 ? L' ' : cell.ch);
    }
    // Trailing blanks are noise when copying a line.
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

} // namespace ps
