#include "widget/board_grow.h"

#include <algorithm>
#include <cstdint>

namespace wg::board {
namespace {

using Coord = std::int64_t;

// Right-aligned candidate: x is pinned to the work area's right edge and y is
// clamped so the rectangle stays inside the work area vertically.
Rect MakeCandidate(Coord workRight, Coord workTop, Coord workBottom, int width, int height,
                   int targetY) {
    Rect r;
    r.w = width;
    r.h = height;
    r.x = static_cast<int>(workRight - width);
    Coord y = targetY;
    const Coord maxY = workBottom - height;
    if (y < workTop) y = workTop;
    if (y > maxY) y = maxY;
    r.y = static_cast<int>(y);
    return r;
}

} // namespace

bool FindMinimalGrow(const Rect& target, const RECT& work, const GrowLimits& limits,
                     const std::function<bool(const Rect&)>& fits, Rect* out) {
    if (!out || !fits || limits.step < 1 || target.empty()) return false;

    const Coord workW = static_cast<Coord>(work.right) - work.left;
    const Coord workH = static_cast<Coord>(work.bottom) - work.top;
    if (workW <= 0 || workH <= 0) return false;

    // Width grid: anchored at max(target.w, minWidth); maxWidth only caps, a
    // wider target is kept as the single candidate width.
    const Coord startW = std::max<Coord>(target.w, limits.minWidth);
    const Coord endW = std::min<Coord>(std::max<Coord>(limits.maxWidth, target.w), workW);
    // Height interval: never shrink the target, respect minHeight and both caps.
    const Coord lowH = std::max<Coord>(target.h, limits.minHeight);
    const Coord highH = std::min<Coord>(limits.maxHeight, workH);
    if (startW < 1 || startW > endW || lowH < 1 || lowH > highH) return false;

    const Coord workRight = work.right;
    const Coord step = limits.step;
    Rect best;
    Coord bestArea = 0;
    bool found = false;

    const auto candidateAt = [&](Coord width, Coord height) {
        return MakeCandidate(workRight, work.top, work.bottom, static_cast<int>(width),
                             static_cast<int>(height), target.y);
    };

    for (Coord w = startW; w <= endW; w += step) {
        // Area pruning: widths only grow, so once even the smallest height
        // cannot beat (or tie, since ties prefer the narrower width) the best,
        // no later width can win.
        if (found && w * lowH >= bestArea) break;

        // Scan heights upward in step increments. fits() is NOT assumed to be
        // monotone in height: the caller may pass the bounded greedy planner,
        // which can fail at some heights and succeed at others.
        Coord previousFailed = lowH - 1;
        Coord success = -1;
        for (Coord h = lowH;; h += step) {
            const Coord sample = h > highH ? highH : h;
            if (found && w * sample >= bestArea) break;
            if (fits(candidateAt(w, sample))) {
                success = sample;
                break;
            }
            previousFailed = sample;
            if (sample == highH) break;
        }
        if (success < 0) continue;

        // The first successful sample is in the lowest feasible window at this
        // width, but the window can start a few pixels below the sample. Refine
        // it upwards pixel by pixel for the smallest feasible height.
        Coord bestH = success;
        for (Coord h = previousFailed + 1; h < success; ++h) {
            if (fits(candidateAt(w, h))) {
                bestH = h;
                break;
            }
        }

        const Coord area = w * bestH;
        if (!found || area < bestArea ||
            (area == bestArea && (w < best.w || (w == best.w && bestH < best.h)))) {
            best = candidateAt(w, bestH);
            bestArea = area;
            found = true;
        }
    }

    if (!found) return false;
    *out = best;
    return true;
}

} // namespace wg::board
