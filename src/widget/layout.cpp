#include "widget/layout.h"

#include <algorithm>
#include <cstring>
#include <cwchar>

namespace wg {
namespace {

constexpr int kMargin = 8;

bool Fits(int x, int y, int w, int h, const RECT& work) {
    return x >= work.left + kMargin && y >= work.top + kMargin &&
           x + w <= work.right - kMargin && y + h <= work.bottom - kMargin;
}

// At least `gap` pixels between the edges of the candidate and an occupied box.
bool Clear(int x, int y, int w, int h, const std::vector<Rect>& occupied, int gap) {
    for (const Rect& o : occupied) {
        const bool separated = x + w + gap <= o.x || o.x + o.w + gap <= x ||
                               y + h + gap <= o.y || o.y + o.h + gap <= y;
        if (!separated) return false;
    }
    return true;
}

int Clamp(int value, int lo, int hi) {
    if (hi < lo) return lo;
    return std::max(lo, std::min(value, hi));
}

struct Candidate {
    int x;
    int y;
    int rank;
    long long score;
};

struct EnumContext {
    HWND self;
    std::vector<Rect>* out;
};

BOOL CALLBACK EnumWidgets(HWND hwnd, LPARAM param) {
    EnumContext* ctx = reinterpret_cast<EnumContext*>(param);
    if (!ctx || hwnd == ctx->self) return TRUE;

    wchar_t name[128]{};
    if (GetClassNameW(hwnd, name, 128) <= 0) return TRUE;
    if (wcsncmp(name, L"wg.", 3) != 0) return TRUE;

    const wchar_t* id = name + 3;
    if (*id == L'\0' || wcschr(id, L'.') != nullptr) return TRUE;  // details panel
    if (_wcsicmp(id, L"Prompt") == 0) return TRUE;                 // input dialog

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return TRUE;
    if (rect.right <= rect.left || rect.bottom <= rect.top) return TRUE;

    ctx->out->push_back({rect.left, rect.top, rect.right - rect.left,
                         rect.bottom - rect.top});
    return TRUE;
}

} // namespace

POINT FindFreeSpot(int x, int y, int w, int h, const std::vector<Rect>& occupied,
                   const RECT& work, int gap) {
    gap = std::max(gap, 0);
    w = std::max(w, 1);
    h = std::max(h, 1);

    const int minX = work.left + kMargin;
    const int minY = work.top + kMargin;
    const int maxX = work.right - kMargin - w;
    const int maxY = work.bottom - kMargin - h;

    auto clampX = [&](int value) { return Clamp(value, minX, maxX); };
    auto clampY = [&](int value) { return Clamp(value, minY, maxY); };

    x = clampX(x);
    y = clampY(y);
    if (Fits(x, y, w, h, work) && Clear(x, y, w, h, occupied, gap)) return {x, y};

    std::vector<Candidate> candidates;
    auto add = [&](int cx, int cy, int rank) {
        cx = clampX(cx);
        cy = clampY(cy);
        // "Same row first" is a preference, not a rule: a slot several widget
        // widths away is not the nearest free spot, so let the vertical
        // candidate win instead of jumping across the desktop.
        if (rank == 0 && (cx > x + w + gap || cx < x - w - gap)) rank = 1;
        if (!Fits(cx, cy, w, h, work)) return;
        if (!Clear(cx, cy, w, h, occupied, gap)) return;

        const long long dx = (long long)cx - x;
        const long long dy = (long long)cy - y;
        candidates.push_back({cx, cy, rank, dx * dx + dy * dy});
    };

    for (const Rect& o : occupied) {
        // Same top row first: a row of widgets reads better than a vertical pile.
        add(o.x - gap - w, y, 0);
        add(o.x + o.w + gap, y, 0);

        // Otherwise align with the obstacle that is in the way.
        add(o.x - gap - w, o.y, 1);
        add(o.x + o.w + gap, o.y, 1);
        add(x, o.y - gap - h, 1);
        add(x, o.y + o.h + gap, 1);
        add(o.x - gap - w, o.y + o.h - h, 1);
        add(o.x + o.w + gap, o.y + o.h - h, 1);
    }

    // Last resort: scan the work area from the bottom-right corner.
    const int stepX = std::max(1, w + gap);
    const int stepY = std::max(1, h + gap);
    for (int gy = maxY; gy >= minY; gy -= stepY) {
        for (int gx = maxX; gx >= minX; gx -= stepX) {
            add(gx, gy, 2);
        }
    }

    if (candidates.empty()) return {x, y};

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  if (a.rank != b.rank) return a.rank < b.rank;
                  if (a.score != b.score) return a.score < b.score;
                  if (a.y != b.y) return a.y < b.y;
                  return a.x < b.x;
              });
    return {candidates.front().x, candidates.front().y};
}

LayoutGuard::LayoutGuard() {
    mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\wgWidgetLayout");
    if (!mutex_) return;

    const DWORD result = WaitForSingleObject(mutex_, 2000);
    owned_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
}

LayoutGuard::~LayoutGuard() {
    if (!mutex_) return;
    if (owned_) ReleaseMutex(mutex_);
    CloseHandle(mutex_);
    mutex_ = nullptr;
    owned_ = false;
}

std::vector<Rect> OtherWidgetRects(HWND self) {
    std::vector<Rect> rects;
    EnumContext ctx{self, &rects};
    EnumWindows(&EnumWidgets, reinterpret_cast<LPARAM>(&ctx));
    return rects;
}

} // namespace wg
