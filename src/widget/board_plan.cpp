#include "widget/board_plan.h"

#include "widget/board_layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace wg::board {
namespace {

using Coord = std::int64_t;
constexpr Coord kCoordMax = std::numeric_limits<Coord>::max();

Coord SaturatingMul(Coord a, Coord b) {
    if (a == 0 || b == 0) return 0;
    if (b < 0 || a > kCoordMax / b) return kCoordMax;
    return a * b;
}

Coord SaturatingAdd(Coord a, Coord b) {
    if (b > kCoordMax - a) return kCoordMax;
    return a + b;
}

Coord SquareClamped(Coord v) {
    if (v > 3037000499 || v < -3037000499) return kCoordMax;
    return v * v;
}

bool InsideInterior(const Rect& card, const Rect& interior) {
    return card.x >= interior.x && card.y >= interior.y &&
           static_cast<Coord>(card.x) + card.w <= interior.right() &&
           static_cast<Coord>(card.y) + card.h <= interior.bottom();
}

// A rect is on the board grid when its offset from the interior origin is a
// multiple of the snap step. grid <= 1 means integer-pixel positions, which
// every integer rect already satisfies.
bool OnGrid(const Rect& card, const Rect& interior, int grid) {
    if (grid <= 1) return true;
    return (static_cast<Coord>(card.x) - interior.x) % grid == 0 &&
           (static_cast<Coord>(card.y) - interior.y) % grid == 0;
}

bool ClearOf(const Rect& candidate, const std::vector<Rect>& taken, int gap) {
    for (const Rect& other : taken) {
        if (other.empty()) continue;
        if (!(static_cast<Coord>(candidate.x) + candidate.w + gap <= other.x ||
              static_cast<Coord>(other.x) + other.w + gap <= candidate.x ||
              static_cast<Coord>(candidate.y) + candidate.h + gap <= other.y ||
              static_cast<Coord>(other.y) + other.h + gap <= candidate.y)) {
            return false;
        }
    }
    return true;
}

// One candidate ordering. `order` lists desired indices in placement order;
// when a priority index was requested it is always order.front().
// `keepFitting` selects the least-movement pass: after the priority card (if
// any) is resolved, every card whose requested rectangle is legal, grid-aligned
// against occupied plus the cards already kept stays untouched, and only the
// remaining (missing, colliding or off-grid) cards are dropped.
struct Ordering {
    std::vector<std::size_t> order;
    bool keepFitting = false;
};

// Places every card of `ordering`. Returns false on the first card that has no
// legal position, reporting how many cards were placed and which one failed.
bool ResolveOrder(const std::vector<Rect>& desired, const std::vector<Rect>& occupied,
                  const Rect& interior, int gap, int grid, const Ordering& ordering,
                  int priorityIndex, std::vector<Rect>* cards, int* placed,
                  int* failedIndex) {
    std::vector<Rect> taken = occupied;
    taken.reserve(occupied.size() + desired.size());
    std::vector<Rect> planned(desired.size());
    std::vector<char> kept(desired.size(), 0);
    int count = 0;

    const auto drop = [&](std::size_t index) {
        const Rect& requested = desired[index];
        POINT point{};
        if (!TryResolveDrop(requested.x, requested.y, requested.w, requested.h, taken,
                            interior, gap, grid, &point)) {
            if (placed) *placed = count;
            if (failedIndex) *failedIndex = static_cast<int>(index);
            return false;
        }
        const Rect result{static_cast<int>(point.x), static_cast<int>(point.y), requested.w,
                          requested.h};
        planned[index] = result;
        taken.push_back(result);
        kept[index] = 1;
        ++count;
        return true;
    };

    // The priority card keeps its precedence in every candidate: it is resolved
    // before any other card, so its final position never depends on the
    // ordering. TryResolveDrop returns the requested point when it is legal,
    // hence this is also the least-movement behaviour for that card.
    std::size_t first = 0;
    if (ordering.keepFitting && priorityIndex >= 0 && !ordering.order.empty()) {
        if (!drop(ordering.order.front())) return false;
        first = 1;
    }

    if (ordering.keepFitting) {
        for (std::size_t k = first; k < ordering.order.size(); ++k) {
            const std::size_t index = ordering.order[k];
            const Rect& requested = desired[index];
            if (!InsideInterior(requested, interior) || !OnGrid(requested, interior, grid) ||
                !ClearOf(requested, taken, gap)) {
                continue;
            }
            // Keep the requested rectangle exactly: this pass exists to avoid
            // needless movement, not to re-snap cards that are already legal.
            // An off-grid request is never kept; it is dropped to the nearest
            // legal grid point, so every planned rectangle honours the grid.
            planned[index] = requested;
            taken.push_back(requested);
            kept[index] = 1;
            ++count;
        }
    }

    for (std::size_t k = first; k < ordering.order.size(); ++k) {
        const std::size_t index = ordering.order[k];
        if (kept[index]) continue;
        if (!drop(index)) return false;
    }

    *cards = std::move(planned);
    return true;
}

Coord Displacement(const std::vector<Rect>& desired, const std::vector<Rect>& planned) {
    Coord total = 0;
    for (std::size_t i = 0; i < desired.size(); ++i) {
        const Coord dx = static_cast<Coord>(planned[i].x) - desired[i].x;
        const Coord dy = static_cast<Coord>(planned[i].y) - desired[i].y;
        total = SaturatingAdd(total, SaturatingAdd(SquareClamped(dx), SquareClamped(dy)));
    }
    return total;
}

// The proofs below are necessary conditions for feasibility, so a violated
// condition proves that no arrangement exists. They never use `occupied`,
// which can only make a layout harder, so they cannot over-report.
bool ProveInsufficient(const std::vector<Rect>& desired, const Rect& interior, int gap,
                       int* proofIndex) {
    if (proofIndex) *proofIndex = -1;

    // P1: a card that does not fit in the interior at all.
    for (std::size_t i = 0; i < desired.size(); ++i) {
        if (desired[i].w > interior.w || desired[i].h > interior.h) {
            if (proofIndex) *proofIndex = static_cast<int>(i);
            return true;
        }
    }

    const Coord n = static_cast<Coord>(desired.size());
    if (n == 0) return false;

    // P2: grown-by-gap/2 area bound.
    const Coord e = gap / 2;
    const Coord grownW = static_cast<Coord>(interior.w) + 2 * e;
    const Coord grownH = static_cast<Coord>(interior.h) + 2 * e;
    const Coord capacity = SaturatingMul(grownW, grownH);
    Coord used = 0;
    for (const Rect& card : desired) {
        const Coord w = static_cast<Coord>(card.w) + 2 * e;
        const Coord h = static_cast<Coord>(card.h) + 2 * e;
        used = SaturatingAdd(used, SaturatingMul(w, h));
    }
    if (used > capacity) return true;

    // P3: row/column packing bound (needs at least two cards).
    if (n >= 2) {
        const Coord span = (n - 1) * static_cast<Coord>(gap);
        bool anyPairStacks = false;
        bool anyPairSideBySide = false;
        for (std::size_t i = 0; i < desired.size(); ++i) {
            for (std::size_t j = i + 1; j < desired.size(); ++j) {
                if (static_cast<Coord>(desired[i].h) + desired[j].h + gap <= interior.h) {
                    anyPairStacks = true;
                }
                if (static_cast<Coord>(desired[i].w) + desired[j].w + gap <= interior.w) {
                    anyPairSideBySide = true;
                }
            }
            if (anyPairStacks && anyPairSideBySide) break;
        }
        if (!anyPairStacks) {
            Coord sumW = 0;
            for (const Rect& card : desired) sumW = SaturatingAdd(sumW, card.w);
            if (SaturatingAdd(sumW, span) > interior.w) return true;
        }
        if (!anyPairSideBySide) {
            Coord sumH = 0;
            for (const Rect& card : desired) sumH = SaturatingAdd(sumH, card.h);
            if (SaturatingAdd(sumH, span) > interior.h) return true;
        }
    }

    return false;
}

} // namespace

bool TryPlanLayout(const std::vector<Rect>& desired, const std::vector<Rect>& occupied,
                   const Rect& interior, int gap, int grid, std::vector<Rect>* out,
                   int priorityIndex, PlanStatus* status, int* blockedIndex) {
    if (!out || interior.empty() || gap < 0 || grid < 0 || priorityIndex < -1) return false;
    if (priorityIndex >= 0 && static_cast<std::size_t>(priorityIndex) >= desired.size()) {
        return false;
    }
    for (const Rect& requested : desired) {
        if (requested.empty()) return false;
    }

    // The proofs run before the search: when a necessary condition is violated
    // the search cannot succeed, so this only saves work.
    int offending = -1;
    if (ProveInsufficient(desired, interior, gap, &offending)) {
        if (status) *status = PlanStatus::Insufficient;
        if (blockedIndex) *blockedIndex = offending;
        return false;
    }

    // Candidate 1: requested order, explicit priority card first.
    Ordering base;
    base.order.reserve(desired.size());
    if (priorityIndex >= 0) base.order.push_back(static_cast<std::size_t>(priorityIndex));
    for (std::size_t i = 0; i < desired.size(); ++i) {
        if (priorityIndex >= 0 && i == static_cast<std::size_t>(priorityIndex)) continue;
        base.order.push_back(i);
    }

    std::vector<Ordering> orderings;
    orderings.push_back(base);

    const std::size_t sortFrom = priorityIndex >= 0 ? 1u : 0u;
    const auto area = [&](std::size_t i) {
        return static_cast<Coord>(desired[i].w) * desired[i].h;
    };

    Ordering largest = base;
    std::stable_sort(largest.order.begin() + static_cast<std::ptrdiff_t>(sortFrom),
                     largest.order.end(),
                     [&](std::size_t a, std::size_t b) { return area(a) > area(b); });
    orderings.push_back(std::move(largest));

    Ordering tallest = base;
    std::stable_sort(tallest.order.begin() + static_cast<std::ptrdiff_t>(sortFrom),
                     tallest.order.end(), [&](std::size_t a, std::size_t b) {
                         if (desired[a].h != desired[b].h) return desired[a].h > desired[b].h;
                         return area(a) > area(b);
                     });
    orderings.push_back(std::move(tallest));

    Ordering leastMovement = base;
    leastMovement.keepFitting = true;
    orderings.push_back(std::move(leastMovement));

    std::vector<Rect> best;
    Coord bestCost = 0;
    bool found = false;
    int bestPlaced = -1;
    int fallbackBlocked = -1;

    for (const Ordering& ordering : orderings) {
        std::vector<Rect> planned;
        int placed = 0;
        int failed = -1;
        if (!ResolveOrder(desired, occupied, interior, gap, grid, ordering, priorityIndex,
                          &planned, &placed, &failed)) {
            // Keep the diagnostic from the candidate that got furthest; ties
            // keep the earlier candidate.
            if (placed > bestPlaced) {
                bestPlaced = placed;
                fallbackBlocked = failed;
            }
            continue;
        }
        const Coord cost = Displacement(desired, planned);
        if (!found || cost < bestCost) {
            best = std::move(planned);
            bestCost = cost;
            found = true;
        }
        // Zero displacement is the global minimum for this ordering set, so no
        // later candidate can win; stopping keeps ties trivially deterministic.
        if (bestCost == 0) break;
    }

    if (!found) {
        if (status) *status = PlanStatus::NotFound;
        if (blockedIndex) *blockedIndex = fallbackBlocked;
        return false;
    }

    *out = std::move(best);
    if (status) *status = PlanStatus::Ok;
    if (blockedIndex) *blockedIndex = -1;
    return true;
}

} // namespace wg::board
