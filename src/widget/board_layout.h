// Pure board geometry: grid snap, card rects, overlap resolution and hit
// testing. No HWND, so the headless test can drive every case.
#pragma once

#include "widget/geom.h"
#include "widget/layout.h"

#include <vector>

namespace wg {
namespace board {

int Snap(int value, int grid);

Rect CardRect(int x, int y, int width, int bodyHeight);

// Keeps a card inside the board interior; if the card is larger than the
// interior it is pinned to the top-left of the interior.
Rect ClampCard(int x, int y, int w, int h, const Rect& interior);

// Find the grid-aligned point nearest (x, y) whose card fits inside interior
// and stays at least gap pixels from every occupied card. Grid coordinates
// are anchored at interior's top-left; grid <= 1 means integer-pixel positions. Ties are resolved
// by y, then x. Returns false and leaves *out unchanged if no position fits,
// the inputs are invalid, or out is null. Does not mutate occupied.
bool TryResolveDrop(int x, int y, int w, int h, const std::vector<Rect>& occupied,
                    const Rect& interior, int gap, int grid, POINT* out);

// Compatibility helper. Prefer TryResolveDrop when the caller must avoid
// overlap: if no legal point exists this returns the clamped requested point.
POINT ResolveDrop(int x, int y, int w, int h, const std::vector<Rect>& occupied,
                  const Rect& interior, int gap, int grid);

// Board rect after min-size and work-area clamping.
Rect ClampBoard(const Rect& want, const RECT& work, int minWidth, int minHeight);

enum class HitKind { Empty, BoardTitle, Resize, Card, CardTitle, AddSlot };

struct Hit {
    HitKind kind = HitKind::Empty;
    int card = -1;
};

Hit HitTest(int x, int y, int boardWidth, int boardHeight,
            const std::vector<Rect>& cards, const std::vector<Rect>& addSlots);

} // namespace board
} // namespace wg
