// Headless checks for the pure board geometry and clipping.
#include "widget/board_layout.h"
#include "widget/canvas.h"
#include "widget/module.h"
#include "widget/skin.h"

#include <cstdio>
#include <cstdint>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const wchar_t* what) {
    if (!ok) {
        wprintf(L"[board-test] FAIL %ls\n", what);
        ++g_failures;
    }
}

bool Same(const wg::Rect& a, const wg::Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

struct IncrementalData { int value = 0; };
class IncrementalModule : public wg::ModuleT<IncrementalData> {
public:
    bool fail = false;
    int value() const { return shown().value; }
    wg::ModuleSpec Describe() const override { return {}; }
protected:
    void RefreshData(IncrementalData& data) override {
        ++data.value;
        if (fail) throw 1;
    }
    int BodyHeightOf(const IncrementalData&) const override { return 0; }
    void PaintBodyOf(const IncrementalData&, wg::Canvas&, const wg::Rect&) override {}
};

void TestModuleRefresh() {
    IncrementalModule module;
    module.Load();
    module.Refresh();
    Check(wg::detail::CommitRefresh(module, true) && module.value() == 1,
          L"first refresh published");
    module.Refresh();
    Check(wg::detail::CommitRefresh(module, true) && module.value() == 2,
          L"incremental refresh uses latest state");
    module.fail = true;
    bool refreshed = true;
    try { module.Refresh(); } catch (...) { refreshed = false; }
    Check(!wg::detail::CommitRefresh(module, refreshed) && module.value() == 2,
          L"failed refresh retains last published state");
    module.fail = false;
    module.Refresh();
    Check(wg::detail::CommitRefresh(module, true) && module.value() == 3,
          L"retry starts from last successful state");
}

void TestNearestDrop() {
    using wg::Rect;
    const Rect interior{3, 5, 32, 32};
    POINT point{999, 999};
    Check(wg::board::TryResolveDrop(7, 9, 8, 8, {}, interior, 0, 8, &point) &&
              point.x == 3 && point.y == 5,
          L"ties choose top then left relative to interior grid");
    Check(wg::board::TryResolveDrop(100, 100, 8, 8, {}, interior, 0, 8, &point) &&
              point.x == 27 && point.y == 29,
          L"drop beyond board clamps to last legal grid position");
    point = POINT{111, 222};
    Check(!wg::board::TryResolveDrop(3, 5, 33, 8, {}, interior, 0, 8, &point) &&
              point.x == 111 && point.y == 222,
          L"oversized card reports failure without modifying output");
    Check(!wg::board::TryResolveDrop(3, 5, 8, 8, {interior}, interior, 0, 8, &point) &&
              point.x == 111 && point.y == 222,
          L"full board reports failure without modifying output");
    Check(!wg::board::TryResolveDrop(3, 5, 8, 8, {}, interior, 0, 8, nullptr),
          L"null output rejected");

    // Compare with an independently enumerated grid on small, varied boards.
    // This catches narrow slots, mixed obstacles, negative origins and ties.
    std::uint32_t state = 0x12345678u;
    const auto random = [&]() { state = state * 1664525u + 1013904223u; return state; };
    for (int trial = 0; trial < 600; ++trial) {
        const int ox = (int)(random() % 41) - 20;
        const int oy = (int)(random() % 41) - 20;
        const Rect area{ox, oy, 16 + (int)(random() % 30), 16 + (int)(random() % 30)};
        const int w = 3 + (int)(random() % 16), h = 3 + (int)(random() % 16);
        const int x = ox - 12 + (int)(random() % 70);
        const int y = oy - 12 + (int)(random() % 70);
        const int grid = 1 + (int)(random() % 8), gap = (int)(random() % 4);
        std::vector<Rect> occupied;
        const int count = (int)(random() % 5);
        for (int i = 0; i < count; ++i) {
            occupied.push_back(Rect{ox - 5 + (int)(random() % 55),
                                    oy - 5 + (int)(random() % 55),
                                    1 + (int)(random() % 18), 1 + (int)(random() % 18)});
        }
        bool expectedFound = false;
        long long best = 0;
        POINT expected{};
        for (int cy = area.y; cy + h <= area.bottom(); cy += grid) {
            for (int cx = area.x; cx + w <= area.right(); cx += grid) {
                const Rect candidate{cx, cy, w, h};
                bool clear = true;
                for (const Rect& other : occupied) {
                    if (candidate.intersects(other, gap)) { clear = false; break; }
                }
                if (!clear) continue;
                const long long dx = (long long)cx - x, dy = (long long)cy - y;
                const long long distance = dx * dx + dy * dy;
                if (!expectedFound || distance < best ||
                    (distance == best && (cy < expected.y || (cy == expected.y && cx < expected.x)))) {
                    expectedFound = true;
                    best = distance;
                    expected = POINT{cx, cy};
                }
            }
        }
        POINT actual{777, 888};
        const bool found = wg::board::TryResolveDrop(x, y, w, h, occupied, area, gap, grid, &actual);
        if (found != expectedFound || (found && (actual.x != expected.x || actual.y != expected.y)) ||
            (!found && (actual.x != 777 || actual.y != 888))) {
            Check(false, L"nearest grid search agrees with exhaustive search");
            break;
        }
    }
}

} // namespace

int wmain() {
    using namespace wg;

    TestModuleRefresh();
    TestNearestDrop();

    Check(board::Snap(37, 8) == 40, L"snap up");
    Check(board::Snap(3, 8) == 0, L"snap down");
    Check(board::Snap(-9, 8) == -8, L"snap negative");

    const Rect card = board::CardRect(20, 32, 340, 96);
    Check(Same(card, Rect{20, 32, 340, 144}), L"card rect includes chrome");
    Check(skin::CardHeight(96) == 144, L"card height");
    Check(Same(skin::CardBody(card), Rect{20, 70, 340, 96}), L"card body rect");

    const Rect interior{14, 32, 732, 738};
    const Rect clamped = board::ClampCard(700, 700, 340, 144, interior);
    Check(clamped.x == 406 && clamped.y == 626, L"clamp card inside board");
    const Rect pinned = board::ClampCard(-50, -50, 340, 144, interior);
    Check(pinned.x == 14 && pinned.y == 32, L"clamp card top-left");

    const RECT work{0, 0, 1568, 849};
    const Rect boardRect =
        board::ClampBoard(Rect{784, 24, 760, 800}, work, 400, 420);
    Check(Same(boardRect, Rect{784, 24, 760, 800}), L"clamp board in work area");
    const Rect big = board::ClampBoard(Rect{-100, -50, 900, 900}, work, 400, 420);
    Check(Same(big, Rect{0, 0, 900, 849}), L"clamp oversized board");
    const Rect tiny = board::ClampBoard(Rect{10, 10, 20, 20}, work, 400, 420);
    Check(tiny.w == 400 && tiny.h == 420, L"board min size");

    const std::vector<Rect> occupied = {Rect{20, 32, 340, 144}};
    const POINT drop = board::ResolveDrop(20, 32, 340, 144, occupied, interior, 12, 8);
    const Rect dropped{drop.x, drop.y, 340, 144};
    Check(!dropped.intersects(occupied[0], 12), L"drop avoids occupied card");
    Check(dropped.x >= interior.x && dropped.right() <= interior.right(), L"drop inside x");
    Check((drop.x - interior.x) % 8 == 0 && (drop.y - interior.y) % 8 == 0, L"drop snapped");

    const Rect full{0, 0, 340, 144};
    const POINT noRoom =
        board::ResolveDrop(0, 0, 340, 144, std::vector<Rect>{full}, full, 12, 8);
    Check(noRoom.x == 0 && noRoom.y == 0, L"no room keeps the drop");

    const std::vector<Rect> cards = {Rect{20, 32, 340, 144}};
    const std::vector<Rect> slots = {Rect{20, 200, 340, 118}};
    Check(board::HitTest(30, 40, 760, 800, cards, slots).kind ==
              board::HitKind::CardTitle,
          L"hit card title");
    Check(board::HitTest(30, 100, 760, 800, cards, slots).kind == board::HitKind::Card,
          L"hit card body");
    Check(board::HitTest(30, 10, 760, 800, cards, slots).kind ==
              board::HitKind::BoardTitle,
          L"hit board title");
    Check(board::HitTest(750, 790, 760, 800, cards, slots).kind ==
              board::HitKind::Resize,
          L"hit resize");
    Check(board::HitTest(30, 210, 760, 800, cards, slots).kind ==
              board::HitKind::AddSlot,
          L"hit add slot");
    Check(board::HitTest(700, 700, 760, 800, cards, slots).kind ==
              board::HitKind::Empty,
          L"hit empty");

    Canvas canvas;
    Check(canvas.Resize(20, 20), L"canvas resize");
    canvas.Clear(0);
    canvas.SetClip(5, 5, 5, 5);
    canvas.Fill(0, 0, 20, 20, Rgba(0xFF0000));
    canvas.ResetClip();
    Check(canvas.data()[4 * 20 + 4] == 0, L"clip blocks outside");
    Check(canvas.data()[5 * 20 + 5] == Rgba(0xFF0000), L"clip allows inside");
    Check(canvas.data()[9 * 20 + 9] == Rgba(0xFF0000), L"clip allows corner");
    Check(canvas.data()[10 * 20 + 10] == 0, L"clip ends");

    Canvas textCanvas;
    textCanvas.Resize(340, 100);
    const std::wstring fitted =
        skin::FitText(textCanvas, L"COMMANDCODE-WITH-A-VERY-LONG-NAME", 100, 1.0f);
    Check(textCanvas.TextWidth(fitted, 1.0f) <= 100.0f, L"fit text width");
    Check(fitted.size() >= 3 && fitted.substr(fitted.size() - 3) == L"...",
          L"fit text ellipsis");

    if (g_failures) {
        wprintf(L"[board-test] %d failure(s)\n", g_failures);
        return 1;
    }
    wprintf(L"[board-test] ok\n");
    return 0;
}
