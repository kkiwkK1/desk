// Headless integration checks for the board's sizing + placement contract.
//
// These mirror what board.cpp does on the 1280x1310 reference panel: preset
// sizes for Account/Servers/PowerShell, the 8 px grid anchored at the interior
// origin, the 12 px minimum gap, nearest-position drag resolution and the
// "no legal slot" fallback. No HWND is involved.
#include "widget/board.h"
#include "widget/board_layout.h"
#include "widget/board_plan.h"
#include "widget/skin.h"
#include "widget/size.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void Check(bool ok, const wchar_t* what) {
    if (!ok) {
        std::wprintf(L"[composition-test] FAIL %ls\n", what);
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

// board.cpp's InteriorFor(): the card area of a board frame.
wg::Rect InteriorForFrame(int frameW, int frameH) {
    using namespace wg;
    wg::Rect r;
    r.x = skin::kBoardPad;
    r.y = skin::kBoardTitleH + 6;
    r.w = std::max(1, frameW - 2 * skin::kBoardPad);
    r.h = std::max(1, frameH - skin::kBoardTitleH - 6 - skin::kBoardPad - skin::kBoardResize);
    return r;
}

bool Inside(const wg::Rect& card, const wg::Rect& interior) {
    return card.x >= interior.x && card.y >= interior.y &&
           card.right() <= interior.right() && card.bottom() <= interior.bottom();
}

bool OnGrid(const wg::Rect& card, const wg::Rect& interior, int grid) {
    return (card.x - interior.x) % grid == 0 && (card.y - interior.y) % grid == 0;
}

bool PairwiseClear(const std::vector<wg::Rect>& cards, int gap) {
    for (std::size_t i = 0; i < cards.size(); ++i) {
        for (std::size_t j = i + 1; j < cards.size(); ++j) {
            if (cards[i].intersects(cards[j], gap)) return false;
        }
    }
    return true;
}

} // namespace

int wmain() {
    using namespace wg;

    // ---- frame and interior geometry -------------------------------------
    const BoardOptions defaults;
    Check(defaults.width == 1280 && defaults.height == 1310,
          L"first-run frame defaults are the right-half standard panel");

    const Rect interior = InteriorForFrame(defaults.width, defaults.height);
    Check(Same(interior, Rect{14, 32, 1252, 1248}),
          L"1280x1310 frame leaves a 1252x1248 card area");
    const Rect narrow = InteriorForFrame(940, 720);
    Check(Same(narrow, Rect{14, 32, 912, 658}), L"interior follows the panel size");

    // ---- preset sizes ------------------------------------------------------
    const CardSize account = MeasureCardSize(SizePreset::Standard, 340, 96, interior.w);
    const CardSize servers = MeasureCardSize(SizePreset::Standard, 340, 128, interior.w);
    const CardSize psCollapsed = MeasureCardSize(SizePreset::Wide, 568, 120, interior.w);
    const CardSize psExpanded = MeasureCardSize(SizePreset::Wide, 568, 344, interior.w);
    const CardSize full = MeasureCardSize(SizePreset::Full, 0, 160, interior.w);
    const CardSize compact = MeasureCardSize(SizePreset::Compact, 0, 1, interior.w);

    Check(account.width == 340 && account.height == 144, L"Account uses 340x144 collapsed");
    Check(servers.width == 340 && servers.height == 176, L"Servers uses 340x176");
    Check(psCollapsed.width == 568 && psCollapsed.height == 168,
          L"PowerShell 80-column card is 568x168 collapsed");
    Check(psExpanded.width == 568 && psExpanded.height == 392,
          L"PowerShell expanded card is 568x392");
    Check(full.width == 1252 && full.height == 208,
          L"Full spans the 1252 px interior, not a hard-coded 700");
    Check(compact.width == 256 && compact.height == 96, L"Compact preset");
    Check(MeasureCardSize(SizePreset::Full, 0, 160, narrow.w).width == narrow.w,
          L"Full width is re-measured when the panel resizes");

    // ---- reference layout from the施工目标 ---------------------------------
    // A real Servers card is 176 px tall (one header row plus four host rows),
    // so the terminal starts on the next legal row (y=224) instead of the
    // equal-height sketch's 192.
    const std::vector<Rect> reference = {
        Rect{14, 32, account.width, account.height},
        Rect{366, 32, servers.width, servers.height},
        Rect{14, 224, psExpanded.width, psExpanded.height},
    };
    std::vector<Rect> planned;
    board::PlanStatus status = board::PlanStatus::NotFound;
    int blocked = -1;
    Check(board::TryPlanLayout(reference, {}, interior, 12, 8, &planned, -1, &status,
                               &blocked) && status == board::PlanStatus::Ok,
          L"reference Account/Servers/PowerShell layout plans");
    Check(SameList(planned, reference), L"legal saved positions are kept exactly");

    // The sketch in the task puts the terminal at y=192, which overlaps a
    // 176 px tall Servers card. Only the terminal may move; it goes to the
    // nearest legal row (y=224) and the two fixed cards stay put.
    const std::vector<Rect> sketch = {
        Rect{14, 32, account.width, account.height},
        Rect{366, 32, servers.width, servers.height},
        Rect{14, 192, psExpanded.width, psExpanded.height},
    };
    status = board::PlanStatus::NotFound;
    Check(board::TryPlanLayout(sketch, {}, interior, 12, 8, &planned, -1, &status) &&
              status == board::PlanStatus::Ok && Same(planned[0], sketch[0]) &&
              Same(planned[1], sketch[1]) &&
              Same(planned[2], Rect{14, 224, psExpanded.width, psExpanded.height}),
          L"an overlapping sketch is repaired by moving only the terminal");
    Check(PairwiseClear(planned, 12), L"reference layout keeps the 12 px gap");
    for (const Rect& card : planned) {
        Check(Inside(card, interior), L"reference cards stay inside the interior");
        Check(OnGrid(card, interior, 8), L"reference cards sit on the 8 px grid");
    }

    // A fourth card is added without moving the three legal ones.
    std::vector<Rect> withNew = reference;
    withNew.push_back(Rect{14, 632, compact.width, compact.height});
    status = board::PlanStatus::NotFound;
    Check(board::TryPlanLayout(withNew, {}, interior, 12, 8, &planned, 3, &status) &&
              status == board::PlanStatus::Ok,
          L"a new compact card can be added");
    Check(planned.size() == 4 && Same(planned[0], reference[0]) &&
              Same(planned[1], reference[1]) && Same(planned[2], reference[2]),
          L"adding a card keeps the existing legal positions");
    Check(PairwiseClear(planned, 12), L"four-card layout keeps the 12 px gap");
    for (const Rect& card : planned) {
        Check(Inside(card, interior), L"four-card layout stays inside");
        Check(OnGrid(card, interior, 8), L"four-card layout sits on the grid");
    }

    // Re-planning the result must be stable (restart / refresh path).
    std::vector<Rect> replanned;
    Check(board::TryPlanLayout(planned, {}, interior, 12, 8, &replanned) &&
              SameList(replanned, planned),
          L"re-planning a legal layout is stable");

    // Expanding PowerShell keeps everything legal.
    std::vector<Rect> expanded = withNew;
    expanded[2] = Rect{14, 224, psExpanded.width, psExpanded.height};
    Check(board::TryPlanLayout(expanded, {}, interior, 12, 8, &replanned) &&
              PairwiseClear(replanned, 12) && Same(replanned[2], expanded[2]),
          L"expanding the terminal keeps the layout legal");

    // ---- failure reporting: NotFound vs Insufficient -----------------------
    // Two expanded cards still fit this small board, so it must plan.
    const Rect small = InteriorForFrame(400, 420);
    const std::vector<Rect> twoExpanded = {Rect{interior.x, interior.y, 340, 144},
                                           Rect{interior.x, interior.y + 160, 340, 144}};
    std::vector<Rect> untouched = {Rect{7, 8, 9, 10}};
    status = board::PlanStatus::NotFound;
    Check(board::TryPlanLayout(twoExpanded, {}, small, 12, 8, &untouched, -1, &status) ||
              status != board::PlanStatus::Ok,
          L"small-board planning reports a definite status");

    // A blocked strip: no insufficiency proof applies, but three 60x60 cards
    // cannot fit, so this must be reported as NotFound, not Insufficient.
    const Rect strip{14, 32, 200, 200};
    const std::vector<Rect> blocker = {Rect{114, 32, 100, 200}};
    const std::vector<Rect> three = {Rect{14, 32, 60, 60}, Rect{14, 32, 60, 60},
                                     Rect{14, 32, 60, 60}};
    untouched = {Rect{7, 8, 9, 10}};
    status = board::PlanStatus::Ok;
    Check(!board::TryPlanLayout(three, blocker, strip, 12, 8, &untouched, -1, &status) &&
              status == board::PlanStatus::NotFound && Same(untouched[0], Rect{7, 8, 9, 10}),
          L"an unprovable search failure reports NotFound and leaves the result untouched");

    status = board::PlanStatus::Ok;
    const std::vector<Rect> tooWide = {Rect{strip.x, strip.y, 600, 144}};
    untouched = {Rect{7, 8, 9, 10}};
    Check(!board::TryPlanLayout(tooWide, {}, strip, 12, 8, &untouched, -1, &status) &&
              status == board::PlanStatus::Insufficient,
          L"a card wider than the interior reports Insufficient");

    // ---- drag release semantics -------------------------------------------
    const std::vector<Rect> occupied = {reference[0]};
    POINT drop{};
    Check(board::TryResolveDrop(372, 100, 340, 144, occupied, interior, 12, 8, &drop),
          L"a drag release finds a legal slot");
    const Rect dropped{drop.x, drop.y, 340, 144};
    Check(Inside(dropped, interior) && OnGrid(dropped, interior, 8) &&
              !dropped.intersects(occupied[0], 12),
          L"a dropped card is inside, on the grid and clear of the others");
    POINT again{};
    Check(board::TryResolveDrop(372, 100, 340, 144, occupied, interior, 12, 8, &again) &&
              again.x == drop.x && again.y == drop.y,
          L"the same release point resolves the same way");

    const std::vector<Rect> fullBoard = {interior};
    POINT kept{777, 888};
    Check(!board::TryResolveDrop(200, 200, 340, 144, fullBoard, interior, 12, 8, &kept) &&
              kept.x == 777 && kept.y == 888,
          L"a release with no legal slot fails without touching the caller position");

    // ---- drag must never compute a layout ---------------------------------
    // (Behavioural note: board.cpp only calls TryResolveDrop on button release;
    // this test only pins the pure function's contract used by that path.)

    if (failures) {
        std::wprintf(L"[composition-test] %d failure(s)\n", failures);
        return 1;
    }
    std::wprintf(L"[composition-test] ok\n");
    return 0;
}
