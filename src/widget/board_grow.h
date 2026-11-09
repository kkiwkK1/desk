// Smallest board rectangle that still fits the cards: a bounded, deterministic
// growth search with an injected feasibility predicate.
//
// Pure geometry (no HWND, no timers, no randomness) so board.cpp can pass
// TryPlanLayout as the oracle and headless tests can pass synthetic oracles.
#pragma once

#include "widget/geom.h"

#include <windows.h>

#include <functional>

namespace wg::board {

// Bounds for FindMinimalGrow. maxWidth is the caller's cap for the panel
// (for desk: the right-half width), maxHeight the caller's own cap; the work
// area height caps the height as well. step is the width growth step and the
// height sampling step.
struct GrowLimits {
    int minWidth = 0;
    int minHeight = 0;
    int maxWidth = 0;
    int maxHeight = 0;
    int step = 8;
};

// Finds the smallest-area board rectangle that is inside `work` and accepted
// by `fits`.
//
// Candidate shape (deterministic, right-aligned):
//   width  starts at max(target.w, minWidth) and grows by limits.step up to
//          min(max(limits.maxWidth, target.w), work width). limits.maxWidth is
//          a cap only: a target that is already wider is never shrunk, it is
//          scanned as the single width candidate.
//   x      = work.right - width, so the right edge stays flush with the work
//          area's right edge and the grown panel keeps living on that side.
//   height the admissible interval is [max(target.h, minHeight),
//          min(maxHeight, work height)]. For every width it is scanned upward
//          in limits.step increments from the bottom; the first accepted sample
//          opens a window whose skipped pixels are then scanned upward one by
//          one for the smallest accepted height. lowH and highH are always
//          sampled.
//   y      = clamp(target.y, work.top, work.bottom - height).
// Every candidate is guaranteed to lie inside `work`. Among fitting
// candidates the smallest area (width * height) wins; ties go to the narrower
// width, then to the shorter height. The result depends only on the arguments.
//
// No monotonicity assumption: `fits` may be non-monotone in BOTH directions
// (the caller passes the bounded greedy planner, which can fail at a taller
// height after fitting a shorter one). Width is always scanned exhaustively
// (a wider board can also fit less: a Full-preset card widens with the
// interior), and height is sampled at step granularity with a pixel refinement
// of the first successful window. Residual bounded-search risk: a feasible
// island of heights shorter than `step` lying entirely inside a window whose
// sampled heights all fail is not discovered; only the first successful window
// per width is refined. With step = 1 the height scan is exhaustive.
//
// Work cap: at most W * (floor((highH - lowH) / step) + step + 1) calls to
// `fits`, where W = floor((endW - startW) / step) + 1 is the width candidate
// count and the bracket is the per-width sample count plus at most step - 1
// pixel refinement probes. W is bounded by the work area width, so the work is
// O(workWidth / step * (workHeight / step + step)). Early area pruning stops
// widths that can no longer beat the best area, so the typical count is much
// lower. There is no recursion, allocation or randomness.
//
// Returns false and leaves *out untouched when no candidate fits. Invalid input
// (null `out`, empty `fits`, step < 1, empty target, empty work area) also
// returns false and leaves *out untouched. On success *out is the smallest-area
// candidate described above.
bool FindMinimalGrow(const Rect& target, const RECT& work, const GrowLimits& limits,
                     const std::function<bool(const Rect&)>& fits, Rect* out);

} // namespace wg::board
