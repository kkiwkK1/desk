#include "widget/board_grow.h"

#include "widget/board_plan.h"
#include "widget/size.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wprintf(L"[board-grow-test] FAIL %ls\n", message);
        ++failures;
    }
}

bool Same(const wg::Rect& a, const wg::Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool InsideWork(const wg::Rect& r, const RECT& work) {
    return r.x >= work.left && r.y >= work.top && r.x + r.w <= work.right &&
           r.y + r.h <= work.bottom;
}

struct Space {
    int startW = 0;
    int endW = 0;
    int step = 0;
    int lowH = 0;
    int highH = 0;
    bool valid = false;
};

Space CandidateSpace(const wg::Rect& target, const RECT& work, const wg::board::GrowLimits& limits) {
    Space s;
    const long long workW = (long long)work.right - work.left;
    const long long workH = (long long)work.bottom - work.top;
    if (workW <= 0 || workH <= 0 || limits.step < 1 || target.empty()) return s;
    s.startW = (int)std::max<long long>(target.w, limits.minWidth);
    s.endW = (int)std::min<long long>(std::max<long long>(limits.maxWidth, target.w), workW);
    s.lowH = (int)std::max<long long>(target.h, limits.minHeight);
    s.highH = (int)std::min<long long>(limits.maxHeight, workH);
    s.step = limits.step;
    s.valid = s.startW >= 1 && s.startW <= s.endW && s.lowH >= 1 && s.lowH <= s.highH;
    return s;
}

wg::Rect MakeRect(const wg::Rect& target, const RECT& work, int w, int h) {
    wg::Rect r;
    r.w = w;
    r.h = h;
    r.x = work.right - w;
    int y = target.y;
    if (y < work.top) y = work.top;
    const int maxY = work.bottom - h;
    if (y > maxY) y = maxY;
    r.y = y;
    return r;
}

// Reference implementation: enumerate every width on the step grid and every
// integer height, keep the best by (area, narrower width, shorter height).
bool BruteForce(const wg::Rect& target, const RECT& work, const wg::board::GrowLimits& limits,
                const std::function<bool(const wg::Rect&)>& fits, wg::Rect* out) {
    const Space s = CandidateSpace(target, work, limits);
    if (!s.valid) return false;
    bool found = false;
    wg::Rect best;
    long long bestArea = 0;
    for (int w = s.startW; w <= s.endW; w += s.step) {
        for (int h = s.lowH; h <= s.highH; ++h) {
            const wg::Rect candidate = MakeRect(target, work, w, h);
            if (!InsideWork(candidate, work)) continue;
            if (!fits(candidate)) continue;
            const long long area = (long long)w * h;
            if (!found || area < bestArea ||
                (area == bestArea &&
                 (w < best.w || (w == best.w && h < best.h)))) {
                best = candidate;
                bestArea = area;
                found = true;
            }
        }
    }
    if (!found) return false;
    *out = best;
    return true;
}

} // namespace

int wmain() {
    using wg::CardSize;
    using wg::MeasureCardSize;
    using wg::Rect;
    using wg::SizePreset;
    using wg::board::FindMinimalGrow;
    using wg::board::GrowLimits;
    using namespace wg::board;

    // ---- exhaustive cross-check on small randomized ranges -----------------
    {
        std::uint32_t state = 0x1234ABCDu;
        const auto next = [&]() {
            state = state * 1664525u + 1013904223u;
            return state;
        };
        bool agreed = true;
        for (int trial = 0; trial < 300 && agreed; ++trial) {
            RECT work;
            work.left = (int)(next() % 41) - 20;
            work.top = (int)(next() % 41) - 20;
            work.right = work.left + 80 + (int)(next() % 200);
            work.bottom = work.top + 80 + (int)(next() % 180);
            Rect target;
            target.w = 40 + (int)(next() % 300);
            target.h = 40 + (int)(next() % 300);
            target.x = work.left + (int)(next() % 200) - 50;
            target.y = work.top + (int)(next() % 240) - 60;
            GrowLimits limits;
            limits.minWidth = (int)(next() % 200);
            limits.minHeight = (int)(next() % 200);
            limits.maxWidth = (int)(next() % 400);
            limits.maxHeight = 100 + (int)(next() % 300);
            limits.step = (next() % 2) ? 8 : 16;
            const int salt = (int)(next() % 97);
            // Monotone in height for a fixed width, arbitrary in width: the
            // step scan must still recover the exact minimum.
            const std::function<bool(const Rect&)> fits = [salt](const Rect& r) {
                return r.h >= 100 + ((r.w * 37 + salt) % 180);
            };
            Rect actual;
            Rect expected;
            const bool okActual = FindMinimalGrow(target, work, limits, fits, &actual);
            const bool okExpected = BruteForce(target, work, limits, fits, &expected);
            agreed = okActual == okExpected && (!okActual || Same(actual, expected));
            if (okActual) {
                agreed = agreed && InsideWork(actual, work) && actual.right() == work.right;
            }
            if (!agreed) {
                std::wprintf(L"[board-grow-test] random trial %d disagreed "
                             L"(work %d,%d,%d,%d target %d,%d,%d,%d step %d)\n",
                             trial, (int)work.left, (int)work.top, (int)work.right,
                             (int)work.bottom, target.x, target.y, target.w, target.h,
                             limits.step);
            }
        }
        Check(agreed, L"minimal grow agrees with the exhaustive candidate search");
    }

    // ---- non-monotone oracles: scattered feasible height windows -----------
    {
        std::uint32_t state = 0x00C0FFEEu;
        const auto next = [&]() {
            state = state * 1664525u + 1013904223u;
            return state;
        };
        bool agreed = true;
        int trials = 0;
        for (int trial = 0; trial < 200 && agreed; ++trial) {
            RECT work;
            work.left = (int)(next() % 31) - 15;
            work.top = (int)(next() % 31) - 15;
            work.right = work.left + 120 + (int)(next() % 220);
            work.bottom = work.top + 120 + (int)(next() % 220);
            Rect target;
            target.w = 40 + (int)(next() % 200);
            target.h = 40 + (int)(next() % 200);
            target.x = work.left + (int)(next() % 100) - 40;
            target.y = work.top + (int)(next() % 200) - 50;
            GrowLimits limits;
            limits.minWidth = (int)(next() % 150);
            limits.minHeight = (int)(next() % 150);
            limits.maxWidth = (int)(next() % 300);
            limits.maxHeight = 120 + (int)(next() % 260);
            limits.step = 1 + (int)(next() % 16);
            const Space space = CandidateSpace(target, work, limits);
            if (!space.valid) continue;
            const int range = space.highH - space.lowH + 1;
            if (range < limits.step) continue;  // windows could not be observed
            // Feasible height windows keyed by width candidate; every window is
            // at least `step` tall, so one sample lands in each window and the
            // documented scan+refine must recover its exact bottom.
            struct Window {
                int low;
                int high;
            };
            std::vector<std::vector<Window>> windows(7);
            for (std::size_t key = 0; key < windows.size(); ++key) {
                const int count = (int)(next() % 4);
                for (int i = 0; i < count; ++i) {
                    const int length = limits.step + (int)(next() % 40);
                    if (length > range) continue;
                    const int low = space.lowH + (int)(next() % (range - length + 1));
                    windows[key].push_back(Window{low, low + length});
                }
            }
            const int startW = space.startW;
            const std::function<bool(const Rect&)> fits = [&](const Rect& r) {
                const std::size_t key =
                    (std::size_t)((r.w - startW) / limits.step) % windows.size();
                for (const Window& win : windows[key]) {
                    if (r.h >= win.low && r.h <= win.high) return true;
                }
                return false;
            };
            Rect actual;
            Rect expected;
            const bool okActual = FindMinimalGrow(target, work, limits, fits, &actual);
            const bool okExpected = BruteForce(target, work, limits, fits, &expected);
            ++trials;
            agreed = okActual == okExpected && (!okActual || Same(actual, expected));
            if (okActual) {
                agreed = agreed && InsideWork(actual, work) && actual.right() == work.right &&
                         fits(actual);
            }
            if (!agreed) {
                std::wprintf(L"[board-grow-test] non-monotone trial %d disagreed\n", trial);
            }
        }
        Check(trials > 100, L"non-monotone oracle trials were not skipped entirely");
        Check(agreed, L"non-monotone scattered windows agree with the exhaustive search");
    }

    // ---- width ladder bug from review: feasible only from 600 ---------------
    {
        const RECT work{0, 0, 2560, 1392};
        const Rect target{100, 100, 500, 300};
        const GrowLimits limits{400, 420, 1280, 800, 8};
        const std::function<bool(const Rect&)> fits = [](const Rect& r) {
            return r.w >= 600;
        };
        Rect out;
        const bool ok = FindMinimalGrow(target, work, limits, fits, &out);
        Check(ok && out.w == 604 && out.h == 420,
              L"step scan finds a feasible width the old +8..+64 ladder missed");
        Check(ok && out.x + out.w == work.right, L"grown board is flush with work.right");
        // The old ladder stopped at target.w + 64 = 564, far below 600.
        Check(!fits(MakeRect(target, work, 564, 420)),
              L"the old target.w+64 ladder really was infeasible");
        Rect expected;
        Check(BruteForce(target, work, limits, fits, &expected) && Same(out, expected),
              L"ladder case agrees with brute force");
    }

    // ---- tie-break: equal area prefers the narrower width -------------------
    {
        const RECT work{0, 0, 400, 600};
        const Rect target{0, 0, 100, 100};
        const GrowLimits limits{100, 100, 200, 500, 50};
        const std::function<bool(const Rect&)> fits = [](const Rect& r) {
            return (r.w >= 200 && r.h >= 100) || (r.w >= 100 && r.h >= 200);
        };
        Rect out;
        Check(FindMinimalGrow(target, work, limits, fits, &out) && out.w == 100 && out.h == 200,
              L"equal areas prefer the narrower width");
        // Smaller area beats the tie-break: 200x100 (20000) beats 100x300 (30000).
        const std::function<bool(const Rect&)> areaFits = [](const Rect& r) {
            return (r.w >= 100 && r.h >= 300) || (r.w >= 200 && r.h >= 100);
        };
        Check(FindMinimalGrow(target, work, limits, areaFits, &out) && out.w == 200 &&
                  out.h == 100,
              L"smaller area wins over a narrower but taller layout");
    }

    // ---- right-alignment and y clamping -------------------------------------
    {
        const RECT work{100, 50, 1500, 900};
        const Rect target{0, 100, 400, 200};
        const GrowLimits limits{300, 200, 1000, 600, 8};
        const std::function<bool(const Rect&)> fits = [](const Rect&) { return true; };
        Rect out;
        Check(FindMinimalGrow(target, work, limits, fits, &out) && out.w == 400 &&
                  out.h == 200 && out.x + out.w == work.right && out.y == 100 &&
                  InsideWork(out, work),
              L"right-aligned candidate keeps work.right and stays inside");

        const Rect below{0, 1300, 500, 300};
        const GrowLimits tall{400, 420, 1280, 800, 8};
        Check(FindMinimalGrow(below, work, tall, fits, &out) && out.y + out.h == work.bottom &&
                  InsideWork(out, work),
              L"y below the work area is clamped to the bottom edge");

        const Rect above{0, -20, 500, 300};
        Check(FindMinimalGrow(above, work, tall, fits, &out) && out.y == work.top &&
                  InsideWork(out, work),
              L"y above the work area is clamped to the top edge");
    }

    // ---- height: exact minimum, minHeight and target height are honoured ----
    {
        const RECT work{0, 0, 2560, 1392};
        const GrowLimits limits{400, 100, 1000, 800, 8};
        const Rect target{0, 0, 400, 100};
        const std::function<bool(const Rect&)> fits = [](const Rect& r) {
            return r.h >= 333;
        };
        Rect out;
        Check(FindMinimalGrow(target, work, limits, fits, &out) && out.h == 333,
              L"height scan plus refine returns the exact smallest fitting height");
        const std::function<bool(const Rect&)> always = [](const Rect&) { return true; };
        const Rect taller{0, 0, 400, 500};
        Check(FindMinimalGrow(taller, work, limits, always, &out) && out.h == 500,
              L"target height is never shrunk");
        const GrowLimits raised{400, 420, 1000, 800, 8};
        const Rect shortTarget{0, 0, 400, 100};
        Check(FindMinimalGrow(shortTarget, work, raised, always, &out) && out.h == 420,
              L"minHeight raises the candidate height");
    }

    // ---- width: maxWidth only caps, never shrinks a wider target -----------
    {
        const RECT work{0, 0, 2560, 1392};
        const GrowLimits limits{400, 200, 600, 800, 8};
        const std::function<bool(const Rect&)> always = [](const Rect&) { return true; };
        const Rect wideTarget{0, 0, 900, 300};
        Rect out;
        Check(FindMinimalGrow(wideTarget, work, limits, always, &out) && out.w == 900,
              L"a target wider than maxWidth is not shrunk");
        const GrowLimits wideCap{600, 200, 1280, 800, 8};
        const Rect smallTarget{0, 0, 500, 300};
        Check(FindMinimalGrow(smallTarget, work, wideCap, always, &out) && out.w == 600,
              L"minWidth raises the candidate width");
    }

    // ---- width is scanned, not binary-searched (non-monotone fits) ---------
    {
        const RECT work{0, 0, 2560, 1392};
        const Rect target{0, 0, 500, 300};
        const GrowLimits limits{400, 420, 1280, 800, 8};
        // Feasible only in two disjoint width bands: a wider board can fit less
        // (a Full-preset card grows with the board).
        const std::function<bool(const Rect&)> fits = [](const Rect& r) {
            return (r.w >= 600 && r.h >= 500) || (r.w >= 900 && r.h >= 450);
        };
        Rect out;
        Check(FindMinimalGrow(target, work, limits, fits, &out) && out.w == 604 &&
                  out.h == 500,
              L"non-monotone width feasibility is handled by scanning every width");
        Rect expected;
        Check(BruteForce(target, work, limits, fits, &expected) && Same(out, expected),
              L"non-monotone width case agrees with brute force");
    }

    // ---- determinism and the documented work cap ---------------------------
    {
        const RECT work{0, 0, 2560, 1392};
        const Rect target{0, 0, 500, 300};
        const GrowLimits limits{400, 420, 1280, 800, 8};
        int calls = 0;
        bool allInside = true;
        const std::function<bool(const Rect&)> fits = [&](const Rect& r) {
            ++calls;
            allInside = allInside && InsideWork(r, work) && r.right() == work.right;
            return r.w >= 700 && r.h >= 600;
        };
        Rect first;
        const bool ok = FindMinimalGrow(target, work, limits, fits, &first);
        const int firstCalls = calls;
        calls = 0;
        Rect second;
        const bool again = FindMinimalGrow(target, work, limits, fits, &second);
        Check(ok && again && Same(first, second) && allInside,
              L"growth search is deterministic and only probes inside rects");
        const int startW = std::max(target.w, limits.minWidth);
        const int endW = std::min(std::max(limits.maxWidth, target.w),
                                  (int)(work.right - work.left));
        const int lowH = std::max(target.h, limits.minHeight);
        const int highH = std::min(limits.maxHeight, (int)(work.bottom - work.top));
        const int widths = (endW - startW) / limits.step + 1;
        // Per width: the step samples plus at most step - 1 pixel refinement probes.
        const int perWidth = (highH - lowH) / limits.step + limits.step + 1;
        const int cap = widths * perWidth;
        Check(firstCalls <= cap && firstCalls == calls,
              L"fits calls stay within the documented cap");
    }

    // ---- production numbers: right-half cap on a 2560x1392 work area ------
    {
        const RECT work{0, 0, 2560, 1392};
        const Rect target{1280, 0, 1280, 1310};  // the 1280x1310 reference board
        const GrowLimits limits{400, 420, 1280, 1392, 8};
        // The reference board already owns the full right-half width, so growth
        // can only add height; the right edge must stay on work.right.
        const std::function<bool(const Rect&)> needsTaller = [](const Rect& r) {
            return r.h >= 1380;
        };
        Rect out;
        Check(FindMinimalGrow(target, work, limits, needsTaller, &out) && out.w == 1280 &&
                  out.h == 1380 && out.x == 1280 && out.x + out.w == work.right &&
                  out.y == 0 && InsideWork(out, work),
              L"right-half board grows in height only and stays flush right");
        // Given as just the right half (left = 1280), the same rectangle falls
        // out: the search is right-aligned and origin-independent.
        const RECT rightHalf{1280, 0, 2560, 1392};
        Check(FindMinimalGrow(target, rightHalf, limits, needsTaller, &out) &&
                  out.w == 1280 && out.h == 1380 && out.x == 1280 &&
                  out.x + out.w == 2560,
              L"right-aligned growth is independent of the work area origin");
        // A narrower saved board grows to the largest step-grid width below the
        // right-half cap (900 + 47 * 8 = 1276; the cap itself is only a bound).
        const Rect narrowTarget{900, 0, 900, 1200};
        const std::function<bool(const Rect&)> needsWide = [](const Rect& r) {
            return r.w >= 1270 && r.h >= 1200;
        };
        Check(FindMinimalGrow(narrowTarget, work, limits, needsWide, &out) &&
                  out.w == 1276 && out.h == 1200 && out.x == work.right - 1276,
              L"narrow saved board grows width on the step grid below the cap");
    }

    // ---- adversarial: the REAL bounded-greedy planner is not monotone ------
    // Reviewer counterexample geometry: work 2032x972, panel width 1016, board
    // currently 1016x850. With these four shipped-preset cards the planner only
    // accepts heights 934..941, and the old binary search probes 911, 942, 957,
    // 965, 969, 971 and 972 - it cannot find 934. (Pinned to the current
    // planner; revisit these numbers if TryPlanLayout heuristics change.)
    {
        const CardSize full = MeasureCardSize(SizePreset::Full, 0, 344, 988);
        const CardSize account = MeasureCardSize(SizePreset::Standard, 340, 224, 988);
        const CardSize servers = MeasureCardSize(SizePreset::Standard, 340, 176, 988);
        const CardSize standard = MeasureCardSize(SizePreset::Standard, 340, 80, 988);
        Check(full.width == 988 && full.height == 392 && account.height == 272 &&
                  servers.height == 224 && standard.height == 128,
              L"adversarial cards are measured from the shipped presets");
        const std::vector<Rect> desired = {
            {596, 408, full.width, full.height},
            {2, 808, account.width, account.height},
            {14, 688, servers.width, servers.height},
            {366, 368, standard.width, standard.height},
        };
        const std::function<bool(const Rect&)> fits = [&](const Rect& board) {
            const Rect interior{14, 32, board.w - 28, board.h - 62};
            if (interior.w <= 0 || interior.h <= 0) return false;
            std::vector<Rect> planned;
            wg::board::PlanStatus status = wg::board::PlanStatus::Ok;
            return wg::board::TryPlanLayout(desired, {}, interior, 12, 8, &planned, -1,
                                            &status);
        };
        int minFeasible = -1;
        for (int h = 850; h <= 972; ++h) {
            if (fits(Rect{1016, 0, 1016, h})) {
                minFeasible = h;
                break;
            }
        }
        Check(minFeasible == 934, L"real planner admits only a narrow height pocket");
        Check(fits(Rect{1016, 0, 1016, 934}) && !fits(Rect{1016, 0, 1016, 942}),
              L"real planner really is non-monotone at the reviewer geometry");
        const auto oldBinarySearch = [&]() {
            int low = 850;
            int high = 972;
            while (low < high) {
                const int mid = low + (high - low) / 2;
                if (fits(Rect{1016, 0, 1016, mid})) {
                    high = mid;
                } else {
                    low = mid + 1;
                }
            }
            return fits(Rect{1016, 0, 1016, low});
        };
        Check(!oldBinarySearch(), L"binary-search height strategy misses the real pocket");
        const RECT work{0, 0, 2032, 972};
        const GrowLimits limits{400, 420, 1016, 972, 8};
        const Rect target{1016, 0, 1016, 850};
        Rect grown;
        Check(FindMinimalGrow(target, work, limits, fits, &grown) && grown.w == 1016 &&
                  grown.h == 934 && grown.x == 1016 &&
                  grown.x + grown.w == work.right && grown.y == 0 &&
                  InsideWork(grown, work) && fits(grown),
              L"minimal grow finds the real planner pocket at height 934");
        Rect expected;
        Check(BruteForce(target, work, limits, fits, &expected) && Same(grown, expected),
              L"real planner pocket matches the exhaustive height scan");
    }

    // ---- no solution and invalid input leave out untouched -----------------
    {
        const RECT work{0, 0, 2560, 1392};
        const Rect target{0, 0, 500, 300};
        const GrowLimits limits{400, 420, 1280, 800, 8};
        const Rect sentinel{7, 8, 9, 10};
        const std::function<bool(const Rect&)> never = [](const Rect&) { return false; };
        const std::function<bool(const Rect&)> always = [](const Rect&) { return true; };

        Rect out = sentinel;
        Check(!FindMinimalGrow(target, work, limits, never, &out) && Same(out, sentinel),
              L"no fitting candidate reports false and leaves out untouched");
        out = sentinel;
        Check(!FindMinimalGrow(target, work, limits, std::function<bool(const Rect&)>(), &out) &&
                  Same(out, sentinel),
              L"empty fits predicate is invalid and leaves out untouched");
        out = sentinel;
        Check(!FindMinimalGrow(target, work, limits, always, nullptr),
              L"null out is rejected");
        out = sentinel;
        const RECT tiny{0, 0, 300, 300};
        Check(!FindMinimalGrow(target, tiny, limits, always, &out) && Same(out, sentinel),
              L"work narrower than the smallest candidate reports false untouched");
        out = sentinel;
        const GrowLimits shortCap{400, 420, 1280, 100, 8};
        Check(!FindMinimalGrow(target, work, shortCap, always, &out) && Same(out, sentinel),
              L"maxHeight below minHeight reports false untouched");
        out = sentinel;
        const GrowLimits badStep{400, 420, 1280, 800, 0};
        Check(!FindMinimalGrow(target, work, badStep, always, &out) && Same(out, sentinel),
              L"step < 1 is invalid and leaves out untouched");
        out = sentinel;
        const RECT emptyWork{10, 10, 10, 200};
        Check(!FindMinimalGrow(target, emptyWork, limits, always, &out) &&
                  Same(out, sentinel),
              L"empty work area is invalid and leaves out untouched");
        out = sentinel;
        const Rect emptyTarget{0, 0, 0, 300};
        Check(!FindMinimalGrow(emptyTarget, work, limits, always, &out) && Same(out, sentinel),
              L"empty target is invalid and leaves out untouched");
    }

    if (failures) {
        std::wprintf(L"[board-grow-test] %d failure(s)\n", failures);
        return 1;
    }
    std::wprintf(L"[board-grow-test] ok\n");
    return 0;
}
