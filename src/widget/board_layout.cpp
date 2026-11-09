#include "widget/board_layout.h"

#include "widget/skin.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace wg {
namespace board {
namespace {

int Clamp(int v, int lo, int hi) {
    if (hi < lo) return lo;
    return std::max(lo, std::min(v, hi));
}

using Coord = std::int64_t;

Coord FloorGrid(Coord value, Coord step) {
    Coord quotient = value / step;
    if (value % step < 0) --quotient;
    return quotient * step;
}

Coord CeilGrid(Coord value, Coord step) {
    const Coord floor = FloorGrid(value, step);
    return floor == value ? floor : floor + step;
}

void AddIfInRange(std::vector<Coord>& values, Coord value, Coord lo, Coord hi) {
    if (value >= lo && value <= hi) values.push_back(value);
}

bool ClearAt(Coord x, Coord y, int w, int h, const std::vector<Rect>& occupied,
             int gap) {
    for (const Rect& other : occupied) {
        if (other.empty()) continue;
        const Coord ox = other.x, oy = other.y;
        if (!(x + w + gap <= ox || ox + other.w + gap <= x ||
              y + h + gap <= oy || oy + other.h + gap <= y)) return false;
    }
    return true;
}

} // namespace

int Snap(int value, int grid) {
    if (grid <= 1) return value;
    const int half = grid / 2;
    return value >= 0 ? ((value + half) / grid) * grid
                      : -(((-value + half) / grid) * grid);
}

Rect CardRect(int x, int y, int width, int bodyHeight) {
    Rect r;
    r.x = x;
    r.y = y;
    r.w = std::max(1, width);
    r.h = skin::CardHeight(std::max(0, bodyHeight));
    return r;
}

Rect ClampCard(int x, int y, int w, int h, const Rect& interior) {
    Rect r;
    r.w = std::max(1, w);
    r.h = std::max(1, h);
    r.x = r.w >= interior.w ? interior.x : Clamp(x, interior.x, interior.right() - r.w);
    r.y = r.h >= interior.h ? interior.y : Clamp(y, interior.y, interior.bottom() - r.h);
    return r;
}

bool TryResolveDrop(int x, int y, int w, int h, const std::vector<Rect>& occupied,
                    const Rect& interior, int gap, int grid, POINT* out) {
    if (!out || w <= 0 || h <= 0 || interior.empty() ||
        w > interior.w || h > interior.h) return false;

    const Coord step = std::max(grid, 1);
    gap = std::max(gap, 0);
    const Coord originX = interior.x, originY = interior.y;
    const auto floorX = [&](Coord value) { return originX + FloorGrid(value - originX, step); };
    const auto ceilX = [&](Coord value) { return originX + CeilGrid(value - originX, step); };
    const auto floorY = [&](Coord value) { return originY + FloorGrid(value - originY, step); };
    const auto ceilY = [&](Coord value) { return originY + CeilGrid(value - originY, step); };
    const Coord minX = originX;
    const Coord minY = originY;
    const Coord maxX = floorX(std::min<Coord>(originX + interior.w - w,
                                              std::numeric_limits<LONG>::max()));
    const Coord maxY = floorY(std::min<Coord>(originY + interior.h - h,
                                              std::numeric_limits<LONG>::max()));
    if (minX > maxX || minY > maxY) return false;

    // Any nearest feasible point is either the projection of the requested
    // point or lies next to a boundary of an obstacle/board. Enumerating those
    // candidate coordinates avoids scanning every pixel on a large monitor.
    std::vector<Coord> xs{minX, maxX}, ys{minY, maxY};
    auto addX = [&](Coord value) { AddIfInRange(xs, value, minX, maxX); };
    auto addY = [&](Coord value) { AddIfInRange(ys, value, minY, maxY); };
    addX(floorX(x));
    addX(ceilX(x));
    addY(floorY(y));
    addY(ceilY(y));
    for (const Rect& other : occupied) {
        if (other.empty()) continue;
        addX(floorX((Coord)other.x - w - gap));
        addX(ceilX((Coord)other.x + other.w + gap));
        addY(floorY((Coord)other.y - h - gap));
        addY(ceilY((Coord)other.y + other.h + gap));
    }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

    bool found = false;
    Coord bestX = 0, bestY = 0;
    long double bestDistance = 0;
    for (const Coord cy : ys) {
        for (const Coord cx : xs) {
            if (!ClearAt(cx, cy, w, h, occupied, gap)) continue;
            const long double dx = (long double)cx - x;
            const long double dy = (long double)cy - y;
            const long double distance = dx * dx + dy * dy;
            if (!found || distance < bestDistance ||
                (distance == bestDistance && (cy < bestY || (cy == bestY && cx < bestX)))) {
                found = true;
                bestX = cx;
                bestY = cy;
                bestDistance = distance;
            }
        }
    }
    if (!found) return false;
    out->x = (LONG)bestX;
    out->y = (LONG)bestY;
    return true;
}

POINT ResolveDrop(int x, int y, int w, int h, const std::vector<Rect>& occupied,
                  const Rect& interior, int gap, int grid) {
    POINT result{};
    if (TryResolveDrop(x, y, w, h, occupied, interior, gap, grid, &result)) return result;
    const Rect fallback = ClampCard(Snap(x, grid), Snap(y, grid), w, h, interior);
    return POINT{fallback.x, fallback.y};
}

Rect ClampBoard(const Rect& want, const RECT& work, int minWidth, int minHeight) {
    const int workW = std::max(1, (int)(work.right - work.left));
    const int workH = std::max(1, (int)(work.bottom - work.top));

    Rect r;
    r.w = std::min(std::max(want.w, minWidth), workW);
    r.h = std::min(std::max(want.h, minHeight), workH);
    r.x = Clamp(want.x, work.left, work.right - r.w);
    r.y = Clamp(want.y, work.top, work.bottom - r.h);
    return r;
}

Hit HitTest(int x, int y, int boardWidth, int boardHeight,
            const std::vector<Rect>& cards, const std::vector<Rect>& addSlots) {
    if (x >= boardWidth - skin::kBoardResize && y >= boardHeight - skin::kBoardResize) {
        return Hit{HitKind::Resize, -1};
    }
    if (y < skin::kBoardTitleH) {
        return Hit{HitKind::BoardTitle, -1};
    }
    for (std::size_t i = 0; i < cards.size(); ++i) {
        if (!cards[i].contains(x, y)) continue;
        return Hit{y < cards[i].y + skin::kCardHeaderH ? HitKind::CardTitle : HitKind::Card,
                   (int)i};
    }
    for (const Rect& slot : addSlots) {
        if (slot.contains(x, y)) return Hit{HitKind::AddSlot, -1};
    }
    return Hit{};
}

} // namespace board
} // namespace wg
