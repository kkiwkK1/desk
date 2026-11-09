#include "widget/board_plan.h"

#include <cstdint>
#include <cwchar>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wprintf(L"[board-plan-test] FAIL %ls\n", message);
        ++failures;
    }
}

bool Same(const wg::Rect& a, const wg::Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool SameList(const std::vector<wg::Rect>& a, const std::vector<wg::Rect>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!Same(a[i], b[i])) return false;
    }
    return true;
}

bool Inside(const wg::Rect& card, const wg::Rect& interior) {
    return card.x >= interior.x && card.y >= interior.y &&
           card.x + card.w <= interior.right() && card.y + card.h <= interior.bottom();
}

bool OnGrid(const wg::Rect& card, const wg::Rect& interior, int grid) {
    return grid <= 1 || ((card.x - interior.x) % grid == 0 &&
                         (card.y - interior.y) % grid == 0);
}

bool ClearOf(const wg::Rect& card, const std::vector<wg::Rect>& others, int gap) {
    for (const wg::Rect& other : others) {
        if (card.intersects(other, gap)) return false;
    }
    return true;
}

void CheckPlanLegal(const std::vector<wg::Rect>& requested, const std::vector<wg::Rect>& occupied,
                    const std::vector<wg::Rect>& planned, const wg::Rect& interior, int gap,
                    int grid, const wchar_t* what) {
    bool ok = planned.size() == requested.size();
    for (std::size_t i = 0; ok && i < planned.size(); ++i) {
        ok = planned[i].w == requested[i].w && planned[i].h == requested[i].h &&
             Inside(planned[i], interior) && OnGrid(planned[i], interior, grid) &&
             ClearOf(planned[i], occupied, gap);
        for (std::size_t j = 0; ok && j < i; ++j) {
            ok = !planned[i].intersects(planned[j], gap);
        }
    }
    Check(ok, what);
}

} // namespace

int wmain() {
    using namespace wg;
    const Rect interior{14, 32, 200, 100};
    const std::vector<Rect> desired = {{14, 32, 70, 40}, {14, 32, 70, 40}};
    std::vector<Rect> out;

    Check(board::TryPlanLayout(desired, {}, interior, 10, 10, &out), L"ordered plan fits");
    Check(out.size() == 2 && Same(out[0], desired[0]) &&
              Same(out[1], Rect{14, 82, 70, 40}),
          L"normal ordering preserves the first requested position");

    Check(board::TryPlanLayout(desired, {}, interior, 10, 10, &out, 1),
          L"priority plan fits");
    Check(out.size() == 2 && Same(out[1], desired[1]) &&
              Same(out[0], Rect{14, 82, 70, 40}),
          L"priority placed first while output remains in original order");

    const std::vector<Rect> fixed = {{14, 32, 70, 40}};
    Check(board::TryPlanLayout({desired[0]}, fixed, interior, 10, 10, &out),
          L"plan respects fixed occupied cards");
    Check(out.size() == 1 && Same(out[0], Rect{14, 82, 70, 40}) &&
              !out[0].intersects(fixed[0], 10),
          L"planned card does not overlap fixed card");

    const std::vector<Rect> sentinel = {{7, 8, 9, 10}};
    out = sentinel;
    const std::vector<Rect> tooMany = {{0, 0, 60, 40}, {0, 0, 60, 40}};
    Check(!board::TryPlanLayout(tooMany, {}, Rect{0, 0, 100, 40}, 0, 1, &out),
          L"no room returns false");
    Check(SameList(out, sentinel) && Same(tooMany[0], Rect{0, 0, 60, 40}) &&
              Same(tooMany[1], Rect{0, 0, 60, 40}),
          L"failed plan leaves output and desired positions unchanged");
    Check(!board::TryPlanLayout(desired, {}, interior, 0, 8, &out, 2) &&
              SameList(out, sentinel),
          L"invalid priority leaves output unchanged");
    Check(!board::TryPlanLayout(desired, {}, interior, 0, 8, nullptr),
          L"null output is rejected");

    const std::vector<Rect> reorder = {{0, 30, 40, 40}, {0, 0, 100, 60}};
    Check(board::TryPlanLayout(reorder, {}, Rect{0, 0, 100, 100}, 0, 10, &out) &&
              out.size() == 2 && Same(out[0], Rect{0, 60, 40, 40}) &&
              Same(out[1], Rect{0, 0, 100, 60}),
          L"fallback places larger card first when original order blocks it");
    out = sentinel;
    Check(!board::TryPlanLayout(reorder, {}, Rect{0, 0, 100, 100}, 0, 10, &out, 0) &&
              SameList(out, sentinel),
          L"fallback does not move the priority card to later in the order");

    std::vector<Rect> alias = tooMany;
    Check(!board::TryPlanLayout(alias, {}, Rect{0, 0, 100, 40}, 0, 1, &alias) &&
              SameList(alias, tooMany),
          L"failed plan is atomic when output aliases requested input");
    alias = desired;
    Check(board::TryPlanLayout(alias, {}, interior, 10, 10, &alias) &&
              alias.size() == 2 && Same(alias[1], Rect{14, 82, 70, 40}),
          L"successful plan supports aliased output");

    Check(board::TryPlanLayout({}, {}, interior, 0, 8, &out) && out.empty(),
          L"empty plan succeeds");

    // ---- Status contract: Ok / NotFound / Insufficient + blockedIndex ----
    {
        board::PlanStatus status = board::PlanStatus::NotFound;
        int blocked = 7;
        Check(board::TryPlanLayout(desired, {}, interior, 10, 10, &out, -1, &status, &blocked) &&
                  status == board::PlanStatus::Ok && blocked == -1,
              L"success reports Ok with blockedIndex -1");
        status = board::PlanStatus::NotFound;
        blocked = 7;
        Check(board::TryPlanLayout({}, {}, interior, 0, 8, &out, -1, &status, &blocked) &&
                  out.empty() && status == board::PlanStatus::Ok && blocked == -1,
              L"empty plan reports Ok with blockedIndex -1");
    }

    // P1: a card larger than the interior proves insufficiency and names it.
    {
        const Rect bigInterior{14, 32, 1252, 1248};
        const std::vector<Rect> oversized = {{14, 32, 340, 144}, {14, 32, 1300, 144}};
        board::PlanStatus status = board::PlanStatus::Ok;
        int blocked = -2;
        out = sentinel;
        Check(!board::TryPlanLayout(oversized, {}, bigInterior, 12, 8, &out, -1, &status,
                                    &blocked) &&
                  status == board::PlanStatus::Insufficient && blocked == 1 &&
                  SameList(out, sentinel) &&
                  Same(oversized[0], Rect{14, 32, 340, 144}),
              L"oversized card proves insufficiency, reports its index and stays atomic");
        const std::vector<Rect> tooTall = {{14, 32, 340, 1300}};
        status = board::PlanStatus::Ok;
        blocked = -2;
        Check(!board::TryPlanLayout(tooTall, {}, bigInterior, 12, 8, &out, -1, &status,
                                    &blocked) &&
                  status == board::PlanStatus::Insufficient && blocked == 0,
              L"card taller than the interior proves insufficiency");
    }

    // P2: expanded-area bound. 5 * 340x144 can never fit 700x300 at gap 12.
    {
        const std::vector<Rect> five(5, Rect{0, 0, 340, 144});
        board::PlanStatus status = board::PlanStatus::Ok;
        int blocked = 3;
        Check(!board::TryPlanLayout(five, {}, Rect{0, 0, 700, 300}, 12, 8, &out, -1, &status,
                                    &blocked) &&
                  status == board::PlanStatus::Insufficient && blocked == -1,
              L"area bound proves insufficiency without a single culprit");
    }

    // P3a: no pair can stack vertically, so all must sit side by side.
    {
        const std::vector<Rect> pair60 = {{0, 0, 60, 60}, {0, 0, 60, 60}};
        board::PlanStatus status = board::PlanStatus::Ok;
        int blocked = 3;
        Check(!board::TryPlanLayout(pair60, {}, Rect{0, 0, 100, 100}, 0, 1, &out, -1, &status,
                                    &blocked) &&
                  status == board::PlanStatus::Insufficient && blocked == -1,
              L"row bound proves insufficiency when no pair can stack");
    }

    // P3b: no pair can sit side by side, so all must stack; heights overflow.
    {
        std::vector<Rect> fourLong;
        for (int i = 0; i < 4; ++i) fourLong.push_back(Rect{0, 0, 60, 30});
        board::PlanStatus status = board::PlanStatus::Ok;
        int blocked = 3;
        Check(!board::TryPlanLayout(fourLong, {}, Rect{0, 0, 100, 100}, 0, 1, &out, -1,
                                    &status, &blocked) &&
                  status == board::PlanStatus::Insufficient && blocked == -1,
              L"column bound proves insufficiency when no pair fits side by side");
        // The same cards with height to spare are feasible and must not be
        // misreported: 4 rows of 30 fit in 100 tall with room between rows.
        board::PlanStatus okStatus = board::PlanStatus::NotFound;
        int okBlocked = 5;
        Check(board::TryPlanLayout(fourLong, {}, Rect{0, 0, 100, 260}, 0, 8, &out, -1, &okStatus,
                                   &okBlocked) &&
                  okStatus == board::PlanStatus::Ok && okBlocked == -1,
              L"column bound does not fire for a feasible stack");
    }

    // NotFound: 5 * 340x144 in 700x400 is impossible (2x2 is the maximum), but
    // the proofs above cannot show it, so the bounded search must report
    // NotFound rather than pretending to prove it.
    {
        std::vector<Rect> five;
        five.push_back(Rect{0, 0, 340, 144});
        five.push_back(Rect{352, 0, 340, 144});
        five.push_back(Rect{0, 160, 340, 144});
        five.push_back(Rect{352, 160, 340, 144});
        five.push_back(Rect{0, 320, 340, 144});
        board::PlanStatus status = board::PlanStatus::Ok;
        int blocked = -2;
        out = sentinel;
        Check(!board::TryPlanLayout(five, {}, Rect{0, 0, 700, 400}, 12, 8, &out, -1, &status,
                                    &blocked) &&
                  status == board::PlanStatus::NotFound && blocked == 4 &&
                  SameList(out, sentinel) && Same(five[4], Rect{0, 320, 340, 144}),
              L"unsolvable-without-proof reports NotFound and the blocked card");
    }

    // ---- 1280x1310 reference layout: every requested rect is preserved ----
    {
        const Rect boardInterior{14, 32, 1252, 1248};  // 1280 - 28, 1310 - 26 - 6 - 14 - 16
        const std::vector<Rect> reference = {
            {14 + 0, 32 + 0, 340, 144},    // Account, Standard, collapsed
            {14 + 352, 32 + 0, 340, 144},  // Servers, Standard, collapsed
            {14 + 0, 32 + 160, 568, 392},  // PowerShell, Wide, expanded (80x24)
        };
        board::PlanStatus status = board::PlanStatus::NotFound;
        int blocked = 7;
        Check(board::TryPlanLayout(reference, {}, boardInterior, 12, 8, &out, -1, &status,
                                   &blocked) &&
                  SameList(out, reference) && status == board::PlanStatus::Ok && blocked == -1,
              L"1280x1310 reference layout keeps every requested rectangle");
        CheckPlanLegal(reference, {}, out, boardInterior, 12, 8,
                       L"reference layout is legal, inside and on grid");
        // The board is flush to the right edge of a 2560x1392 work area, but the
        // plan only sees interior-relative coordinates.
        Check(out[1].x - boardInterior.x == 352 && out[2].y - boardInterior.y == 160,
              L"reference layout coordinates are interior-relative");
    }

    // ---- 340 / 568 / 1252-wide cards in a 1252x1248 interior ----
    {
        const Rect boardInterior{14, 32, 1252, 1248};
        const std::vector<Rect> mixed = {
            {14 + 0, 32 + 0, 340, 144},     // Standard
            {14 + 352, 32 + 0, 340, 176},   // Standard, taller (4 servers)
            {14 + 0, 32 + 192, 568, 392},   // Wide
            {14 + 0, 32 + 624, 1252, 208},  // Full: exactly the interior width
        };
        Check(board::TryPlanLayout(mixed, {}, boardInterior, 12, 8, &out) && SameList(out, mixed),
              L"340/568/1252-wide cards keep legal requested positions");
        CheckPlanLegal(mixed, {}, out, boardInterior, 12, 8,
                       L"mixed preset plan is legal, inside and on grid");
        Check(out[3].w == boardInterior.w && out[3].x == boardInterior.x,
              L"full-width card spans the interior exactly");

        // A full-width card first requested off the left edge and off grid can
        // only land at the interior's left edge.
        const std::vector<Rect> fullOnly = {{14 + 40, 32 + 800, 1252, 208}};
        Check(board::TryPlanLayout(fullOnly, {}, boardInterior, 12, 8, &out) &&
                  out.size() == 1 && out[0].x == boardInterior.x && out[0].w == 1252 &&
                  Inside(out[0], boardInterior) && OnGrid(out[0], boardInterior, 8),
              L"full-width card is pinned to the interior left edge");
    }

    // ---- Ok on a tight layout that fills the interior ----
    {
        const Rect tightInterior{0, 0, 700, 440};
        const std::vector<Rect> tight = {
            {0, 0, 340, 144},
            {352, 0, 340, 144},
            {0, 160, 568, 168},
            {0, 344, 568, 96},
        };
        board::PlanStatus status = board::PlanStatus::NotFound;
        int blocked = 9;
        Check(board::TryPlanLayout(tight, {}, tightInterior, 12, 8, &out, -1, &status, &blocked) &&
                  SameList(out, tight) && status == board::PlanStatus::Ok && blocked == -1,
              L"tight layout that exactly fits reports Ok and stays put");
        CheckPlanLegal(tight, {}, out, tightInterior, 12, 8,
                       L"tight layout is legal, inside and on grid");
    }

    // ---- An order too tight for the naive request, solvable tallest-first ----
    {
        const Rect narrow{0, 0, 200, 100};
        const std::vector<Rect> wants = {{0, 30, 200, 40}, {0, 0, 100, 60}};
        // Requested order places the wide-short card at y=30, which blocks the
        // tall card completely; placing the tall card first works.
        board::PlanStatus status = board::PlanStatus::NotFound;
        int blocked = 4;
        Check(board::TryPlanLayout(wants, {}, narrow, 0, 10, &out, -1, &status, &blocked) &&
                  out.size() == 2 && Same(out[0], Rect{0, 60, 200, 40}) &&
                  Same(out[1], Rect{0, 0, 100, 60}) && status == board::PlanStatus::Ok,
              L"tallest-first order solves a set the requested order cannot place");
    }

    // ---- Movement minimality: a card that already fits must not move ----
    {
        const Rect area{0, 0, 200, 200};
        const std::vector<Rect> taken = {{0, 0, 100, 100}};
        const std::vector<Rect> wants = {{50, 50, 40, 40}, {100, 50, 40, 40}};
        // Card 1 is already at a legal spot. The naive order drops card 0 onto
        // that spot (it is card 0's nearest legal point), forcing card 1 to
        // move; the least-movement pass keeps card 1 and moves card 0 further.
        Check(board::TryPlanLayout(wants, taken, area, 0, 10, &out) && out.size() == 2 &&
                  Same(out[1], wants[1]) && Same(out[0], Rect{50, 100, 40, 40}),
              L"an already-fitting card is not moved for a card that must move");
    }

    // ---- Priority: the named card keeps its nearest legal spot ----
    {
        const Rect area{0, 0, 200, 100};
        const std::vector<Rect> taken = {{0, 0, 100, 100}};
        // Both cards want the same free spot beside the fixed card. The card
        // with the smaller index normally keeps it; an explicit priority moves
        // the priority card there and pushes the other one away.
        const std::vector<Rect> first = {{100, 50, 40, 40}, {50, 50, 40, 40}};
        Check(board::TryPlanLayout(first, taken, area, 0, 10, &out) && out.size() == 2 &&
                  Same(out[0], Rect{100, 50, 40, 40}) && Same(out[1], Rect{100, 10, 40, 40}),
              L"request order decides a contested legal spot without priority");

        board::PlanStatus status = board::PlanStatus::NotFound;
        int blocked = 4;
        Check(board::TryPlanLayout(first, taken, area, 0, 10, &out, 1, &status, &blocked) &&
                  out.size() == 2 && Same(out[1], Rect{100, 50, 40, 40}) &&
                  Same(out[0], Rect{100, 10, 40, 40}) &&
                  status == board::PlanStatus::Ok && blocked == -1,
              L"priority card takes its nearest legal spot and output order is unchanged");

        // The priority card's position must equal the single-card answer.
        std::vector<Rect> single;
        Check(board::TryPlanLayout({first[1]}, taken, area, 0, 10, &single) &&
                  Same(single[0], out[1]),
              L"priority card position equals the independent single-card placement");
    }

    // ---- Every returned rectangle honours the snap grid (review item 5) ----
    {
        const Rect area{0, 0, 200, 100};
        // A legal but off-grid request must be snapped to the nearest legal grid
        // point instead of being preserved verbatim by the least-movement pass.
        const std::vector<Rect> offGrid = {{3, 5, 70, 40}};
        Check(board::TryPlanLayout(offGrid, {}, area, 0, 8, &out) && out.size() == 1 &&
                  Same(out[0], Rect{0, 8, 70, 40}),
              L"legal off-grid request is snapped to the nearest legal grid point");
        // A legal on-grid request stays exactly where it is.
        const std::vector<Rect> onGrid = {{16, 8, 70, 40}};
        Check(board::TryPlanLayout(onGrid, {}, area, 0, 8, &out) && Same(out[0], onGrid[0]),
              L"legal on-grid request is kept exactly");
        // grid <= 1 means integer pixels: no snapping at all.
        const std::vector<Rect> integer = {{3, 5, 70, 40}};
        Check(board::TryPlanLayout(integer, {}, area, 0, 1, &out) && Same(out[0], integer[0]),
              L"grid<=1 keeps integer-pixel positions unchanged");
        // Mixed: only the off-grid card is snapped, the on-grid one stays put
        // (kept far apart so the movement minimiser has no reason to move it).
        const std::vector<Rect> mixedGrid = {{3, 5, 70, 40}, {120, 40, 70, 40}};
        Check(board::TryPlanLayout(mixedGrid, {}, area, 0, 8, &out) && out.size() == 2 &&
                  OnGrid(out[0], area, 8) && OnGrid(out[1], area, 8) &&
                  Same(out[0], Rect{0, 8, 70, 40}) && Same(out[1], mixedGrid[1]),
              L"mixed off/on-grid requests: only the off-grid card is snapped");
        // Review evidence: a saved position one pixel off the grid (PowerShell
        // restored at y=188 with origin 32 -> 156 % 8 != 0) must not survive.
        const Rect tallArea{14, 32, 1252, 1248};
        const std::vector<Rect> restored = {{14, 188, 568, 392}};
        Check(board::TryPlanLayout(restored, {}, tallArea, 12, 8, &out) && out.size() == 1 &&
                  OnGrid(out[0], tallArea, 8) && out[0].x == 14 && out[0].y == 184,
              L"restored off-grid card position is snapped onto the grid");
    }

    // ---- Determinism: repeated calls return identical output ----
    {
        const Rect area{0, 0, 200, 100};
        const std::vector<Rect> mixed = {{0, 30, 200, 40}, {0, 0, 100, 60}};
        std::vector<Rect> a;
        std::vector<Rect> b;
        std::vector<Rect> c;
        const bool okA = board::TryPlanLayout(mixed, {}, area, 0, 10, &a);
        const bool okB = board::TryPlanLayout(mixed, {}, area, 0, 10, &b);
        const bool okC = board::TryPlanLayout(mixed, {}, area, 0, 10, &c, 1);
        Check(okA && okB && SameList(a, b), L"repeated plans are identical");
        // Aliased output must produce the same answer as a fresh vector.
        std::vector<Rect> alias = mixed;
        const bool okAlias = board::TryPlanLayout(alias, {}, area, 0, 10, &alias);
        Check(okAlias && SameList(alias, a), L"aliased plan matches the fresh-vector plan");
        (void)okC;
    }

    // ---- Invalid arguments are rejected without touching status/blocked ----
    {
        const std::vector<Rect> one = {{14, 32, 70, 40}};
        const board::PlanStatus before = board::PlanStatus::Insufficient;
        const int blockedBefore = 42;
        const auto invalid = [&](int gap, int grid, const Rect& area, int priority,
                                 const std::vector<Rect>& cards) {
            board::PlanStatus status = before;
            int blocked = blockedBefore;
            out = sentinel;
            const bool rejected =
                !board::TryPlanLayout(cards, {}, area, gap, grid, &out, priority, &status,
                                      &blocked);
            return rejected && status == before && blocked == blockedBefore &&
                   SameList(out, sentinel);
        };
        Check(invalid(-1, 8, interior, -1, one), L"negative gap is invalid and atomic");
        Check(invalid(0, -1, interior, -1, one), L"negative grid is invalid and atomic");
        Check(invalid(0, 8, Rect{14, 32, 0, 100}, -1, one),
              L"empty interior is invalid and atomic");
        Check(invalid(0, 8, interior, -2, one), L"priority below -1 is invalid and atomic");
        Check(invalid(0, 8, interior, -1, {{14, 32, 0, 40}}),
              L"zero-width card is invalid and atomic");
        Check(invalid(0, 8, interior, -1, {{14, 32, 70, -1}}),
              L"negative-height card is invalid and atomic");
    }

    // ---- Randomized property checks: bounded search stays legal/atomic ----
    {
        std::uint32_t state = 0x9e3779b9u;
        const auto next = [&]() {
            state = state * 1664525u + 1013904223u;
            return state;
        };
        const int widths[] = {256, 340, 568};
        const int heights[] = {96, 112, 168, 208};
        for (int trial = 0; trial < 120; ++trial) {
            const Rect area{-16 + (int)(next() % 33), -8 + (int)(next() % 17), 400, 300};
            const int count = 1 + (int)(next() % 4);
            std::vector<Rect> requested;
            for (int i = 0; i < count; ++i) {
                const int w = widths[next() % 3];
                const int h = heights[next() % 4];
                // Positions are deliberately a few pixels off the 8 px grid,
                // and sometimes outside the interior or on top of each other,
                // so every plan must snap and re-place them.
                const int relX = ((int)(next() % 90) - 40) * 8 + (int)(next() % 8);
                const int relY = ((int)(next() % 70) - 30) * 8 + (int)(next() % 8);
                requested.push_back(Rect{area.x + relX, area.y + relY, w, h});
            }
            const std::vector<Rect> occupied = {};
            board::PlanStatus status = board::PlanStatus::Ok;
            int blocked = -7;
            std::vector<Rect> plan = sentinel;
            const bool ok = board::TryPlanLayout(requested, occupied, area, 12, 8, &plan, -1,
                                                 &status, &blocked);
            if (ok) {
                Check(plan.size() == requested.size() && status == board::PlanStatus::Ok &&
                          blocked == -1,
                      L"random feasible plan reports Ok and keeps the card count");
                CheckPlanLegal(requested, occupied, plan, area, 12, 8,
                               L"random plan is legal, inside and on grid");
                std::vector<Rect> again;
                board::PlanStatus status2 = board::PlanStatus::NotFound;
                Check(board::TryPlanLayout(requested, occupied, area, 12, 8, &again, -1,
                                           &status2) &&
                          SameList(plan, again),
                      L"random plan is deterministic");
            } else {
                const bool classified = status == board::PlanStatus::NotFound ||
                                        status == board::PlanStatus::Insufficient;
                Check(classified && blocked >= -1 && blocked < count &&
                          SameList(plan, sentinel),
                      L"random failure is classified with a bounded blocked index and atomic out");
            }
        }
    }

    if (failures) {
        std::wprintf(L"[board-plan-test] %d failure(s)\n", failures);
        return 1;
    }
    std::wprintf(L"[board-plan-test] ok\n");
    return 0;
}
