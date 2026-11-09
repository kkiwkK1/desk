// Small geometry helpers shared by the board layout and the skin.
#pragma once

namespace wg {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }

    bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }

    bool intersects(const Rect& other, int gap = 0) const {
        return !(x + w + gap <= other.x || other.x + other.w + gap <= x ||
                 y + h + gap <= other.y || other.y + other.h + gap <= y);
    }
};

} // namespace wg
