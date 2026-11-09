// Atomic layout planning for an ordered collection of cards.
#pragma once

#include "widget/geom.h"

#include <vector>

namespace wg::board {

// Classification of a planning result.
//   Ok           every requested card received a legal position; *out holds it.
//   NotFound     the bounded search found no arrangement, but none of the
//                insufficiency proofs below fired. A solution may still exist;
//                NotFound only means "not found within the search cap".
//   Insufficient a proof shows that no arrangement can exist at all.
enum class PlanStatus { Ok, NotFound, Insufficient };

// Places every desired card inside `interior` at least `gap` pixels from every
// other planned card and from every fixed `occupied` rectangle. EVERY returned
// rectangle is grid-aligned: (x - interior.x) % grid == 0 and
// (y - interior.y) % grid == 0 whenever grid > 1, including the least-movement
// pass. A requested rectangle that is legal but off-grid is therefore snapped
// to the nearest legal grid point; one that is legal and already on-grid is
// kept exactly. `desired`, `occupied` and `interior` share one coordinate
// space; the grid origin is interior's top-left corner, and grid <= 1 means
// integer-pixel positions (no alignment constraint). *out keeps the requested
// order.
//
// Search (bounded, deterministic, no randomness, timers or backtracking):
//   Four candidate orderings are tried. Each candidate places every card at
//   most once, so the whole search performs at most 4 * N TryResolveDrop calls
//   for N = desired.size(); there is no exhaustive scan of the grid and no
//   recursion:
//     1. requested order, with the explicit priorityIndex card first;
//     2. largest area first (the priority card stays first), stable by request
//        index;
//     3. tallest first (the priority card stays first), ties by area, then
//        request index;
//     4. least movement: a card whose requested rectangle is already legal and
//        grid-aligned against `occupied` plus the cards kept before it stays
//        exactly where it was asked to be; the remaining cards (missing,
//        colliding or off-grid) are re-dropped.
//   Every non-kept card is positioned by TryResolveDrop, so it lands on the
//   nearest legal grid point to the request (ties: smaller y, then smaller x)
//   and can never overlap. Among successful candidates the one with the smallest total
//   squared displacement from the requested rectangles wins; ties keep the
//   earlier candidate, so the result depends only on the arguments.
//
// Insufficiency proofs (Insufficient is reported only when one is provable):
//   P1. A card is wider than interior.w or taller than interior.h.
//   P2. Area bound: let e = gap / 2. Cards keep at least `gap` distance from
//       each other, so growing every card by e on all four sides makes the
//       grown rectangles pairwise disjoint; their total area must fit in the
//       interior grown by e:
//           sum((w_i + 2e) * (h_i + 2e)) <= (interior.w + 2e) * (interior.h + 2e)
//       Otherwise no arrangement can exist.
//   P3. Row/column bound: if no pair of cards can be stacked vertically
//       (h_i + h_j + gap > interior.h for every pair), then every pair must be
//       separated horizontally, which requires
//           sum(w_i) + (n - 1) * gap <= interior.w
//       and symmetrically: if no pair can sit side by side
//       (w_i + w_j + gap > interior.w for every pair), then
//           sum(h_i) + (n - 1) * gap <= interior.h
//       must hold. A violated necessary condition proves insufficiency.
//   `occupied` rectangles are deliberately ignored by the proofs, so they can
//   only ever under-report infeasibility, never over-report it.
//
// Return value and outputs:
//   true   success: *out holds exactly desired.size() rectangles in request
//          order; *status = Ok and *blockedIndex = -1 when the pointers are
//          non-null.
//   false  failure: *out, `desired` and `occupied` are left untouched (also
//          when *out aliases one of the inputs). *status receives NotFound or
//          Insufficient; *blockedIndex receives the desired index of the card
//          that caused P1, or for NotFound the card that failed to place in
//          the candidate that placed the most cards (earliest such candidate),
//          and -1 for P2/P3 where no single card is at fault.
//   Invalid input (null `out`, empty interior, negative gap or grid,
//   priorityIndex outside [-1, size), a desired rectangle with a non-positive
//   width or height) returns false without modifying *status or *blockedIndex.
bool TryPlanLayout(const std::vector<Rect>& desired, const std::vector<Rect>& occupied,
                   const Rect& interior, int gap, int grid, std::vector<Rect>* out,
                   int priorityIndex = -1, PlanStatus* status = nullptr,
                   int* blockedIndex = nullptr);

} // namespace wg::board
