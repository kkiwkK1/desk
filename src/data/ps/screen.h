// The character grid a VT stream is painted into.
//
// No Windows, no theme, no window: printable text, cursor movement, erase,
// scroll regions, SGR attributes, the alternate screen and a scrollback ring.
// The card turns this into pixels; the tests drive it with byte streams.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace ps {

// Colours are resolved later, by the view: the screen only knows "default",
// "palette index" or "24-bit value", so it stays independent of the theme.
constexpr std::uint32_t kColorDefault = 0xFFFFFFFFu;
constexpr std::uint32_t kColorPalette = 0x01000000u;   // | index 0..255
constexpr std::uint32_t kColorRgb = 0x02000000u;       // | 0xRRGGBB

constexpr std::size_t kMaxScrollback = 2000;

enum CellFlag : std::uint8_t {
    kFlagBold = 1u << 0,
    kFlagFaint = 1u << 1,
    kFlagUnderline = 1u << 2,
    kFlagInverse = 1u << 3,
};

struct Cell {
    wchar_t ch = L' ';
    std::uint32_t fg = kColorDefault;
    std::uint32_t bg = kColorDefault;
    std::uint8_t flags = 0;
    bool wideTail = false;   // right half of a double-width glyph
};

struct Row {
    unsigned long long id = 0;   // stable while the row scrolls into the ring
    std::vector<Cell> cells;
};

struct Attr {
    std::uint32_t fg = kColorDefault;
    std::uint32_t bg = kColorDefault;
    std::uint8_t flags = 0;
};

class Screen {
public:
    Screen() = default;
    Screen(int cols, int rows);

    void Resize(int cols, int rows);
    int cols() const { return cols_; }
    int rows() const { return rows_; }

    // Text.
    void Print(wchar_t c);
    void CarriageReturn();
    void LineFeed();
    void NextLine();
    void ReverseIndex();
    void Backspace();
    void Tab();
    void BackTab();

    // Cursor.
    void MoveTo(int row, int col);            // 0-based; origin mode respected
    void MoveUp(int n);
    void MoveDown(int n);
    void MoveLeft(int n);
    void MoveRight(int n);
    void MoveToColumn(int col);
    void MoveToRow(int row);
    void SaveCursor();
    void RestoreCursor();
    int cursorRow() const { return row_; }
    int cursorCol() const { return col_; }
    bool cursorVisible() const { return cursorVisible_; }

    // Erase / edit.
    void EraseInDisplay(int mode);
    void EraseInLine(int mode);
    void EraseChars(int n);
    void InsertChars(int n);
    void DeleteChars(int n);
    void InsertLines(int n);
    void DeleteLines(int n);
    void ScrollUp(int n);      // CSI S
    void ScrollDown(int n);    // CSI T

    // Modes and regions.
    void SetScrollRegion(int top, int bottom);   // 0-based, inclusive
    void ResetScrollRegion();
    void SetAutoWrap(bool on) { autoWrap_ = on; }
    bool autoWrap() const { return autoWrap_; }
    void SetOriginMode(bool on);
    void SetApplicationCursorKeys(bool on) { appCursor_ = on; }
    bool applicationCursorKeys() const { return appCursor_; }
    void SetBracketedPaste(bool on) { bracketedPaste_ = on; }
    bool bracketedPaste() const { return bracketedPaste_; }
    void SetMouseTracking(int mode, bool on) {
        const unsigned bit = mode == 1000 ? 1u : mode == 1002 ? 2u : mode == 1003 ? 4u : 0u;
        if (on) mouseTracking_ |= bit;
        else mouseTracking_ &= ~bit;
    }
    bool mouseTracking() const { return mouseTracking_ != 0; }
    void SetSgrMouse(bool on) { sgrMouse_ = on; }
    bool sgrMouse() const { return sgrMouse_; }
    void ResetMouseModes() { mouseTracking_ = 0; sgrMouse_ = false; }
    void SetAlternateScreen(bool on);
    bool alternate() const { return alternate_; }
    void SetCursorVisible(bool on) { cursorVisible_ = on; }

    // Attributes.
    void SetAttr(const Attr& attr) { attr_ = attr; }
    void ResetAttr() { attr_ = Attr{}; }
    const Attr& attr() const { return attr_; }

    // View + scrollback.
    std::vector<Row> visibleRows() const;
    const std::deque<Row>& scrollback() const { return scrollback_; }
    unsigned long long scrollbackWrites() const { return scrollbackWrites_; }
    std::wstring lineText(unsigned long long id, int from, int to) const;
    void ClearScrollback() { scrollback_.clear(); }

private:
    std::vector<Row>& Grid() { return alternate_ ? alt_ : main_; }
    const std::vector<Row>& Grid() const { return alternate_ ? alt_ : main_; }
    Cell& At(int row, int col) { return Grid()[(std::size_t)row].cells[(std::size_t)col]; }
    const Cell& At(int row, int col) const {
        return Grid()[(std::size_t)row].cells[(std::size_t)col];
    }
    void BlankRow(int row);
    void ClearRange(int row, int from, int to);   // [from, to)
    void PutAt(int col, wchar_t c, bool wideTail);
    void ScrollRegionUp(int n);
    void ScrollRegionDown(int n);
    void Rebuild(int cols, int rows, std::vector<Row>& grid);
    unsigned long long NewRowId();

    int cols_ = 0;
    int rows_ = 0;
    std::vector<Row> main_;
    std::vector<Row> alt_;
    bool alternate_ = false;
    std::deque<Row> scrollback_;
    unsigned long long scrollbackWrites_ = 0;

    int row_ = 0;
    int col_ = 0;
    bool wrapPending_ = false;
    bool cursorVisible_ = true;
    bool autoWrap_ = true;
    bool originMode_ = false;
    bool appCursor_ = false;
    bool bracketedPaste_ = false;
    unsigned mouseTracking_ = 0;
    bool sgrMouse_ = false;

    int scrollTop_ = 0;
    int scrollBottom_ = 0;

    Attr attr_;

    struct Saved {
        int row = 0;
        int col = 0;
        Attr attr;
        bool wrapPending = false;
    };
    Saved saved_;

    unsigned long long nextId_ = 1;
};

} // namespace ps
