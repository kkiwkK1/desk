#include "counter.h"

#include "widget/module.h"
#include "widget/size.h"
#include "widget/skin.h"

#include <cwchar>
#include <memory>
#include <string>
#include <vector>

namespace {

// Sizing contract (docs/framework.md, "组件尺寸与位置"):
//  - sizePreset picks the width. Standard is 340px wide with a 112..640px
//    outer-height range - the largest fixed preset that is not a terminal.
//  - BodyHeightOf() declares the *content* height only. The framework adds the
//    48px card chrome (30px title bar + 8px top + 10px bottom padding), rounds
//    up to the 8px card step and floors the result at the preset minimum.
//    One 32px row therefore becomes a 112px card, and 16 rows would be
//    48 + 512 = 560px, still inside the range.
//  - The range is a validation boundary, not a clipping instruction: an
//    oversized body would be clipped by the board (and the layout refused), so
//    a component that can grow must either cap its rows or pick a taller preset.
static_assert(wg::kCardChromeHeight + wg::skin::kRowPitch <= 640,
              "the Standard preset must hold this card's content height");

struct ClockData {
    std::vector<wg::Row> rows;
};

class ClockPanel final : public wg::ModuleT<ClockData> {
public:
    wg::ModuleSpec Describe() const override {
        wg::ModuleSpec spec;
        spec.id = L"clock";
        spec.title = L"CLOCK";
        // Explicit preset: 340px wide, height derived from BodyHeightOf().
        spec.sizePreset = wg::SizePreset::Standard;
        spec.refreshSeconds = 60;
        return spec;
    }

protected:
    void LoadData(ClockData& data) override {
        data.rows = {wg::Row{L"LOCAL TIME", L"--:--", L"", -1.0f, 0, L"", 0}};
    }

    void RefreshData(ClockData& data) override {
        // Refresh runs on a worker thread. Replace the whole snapshot instead
        // of assuming the staging buffer contains the last displayed value.
        SYSTEMTIME time{};
        GetLocalTime(&time);
        wchar_t text[16];
        swprintf(text, 16, L"%02u:%02u", (unsigned)time.wHour, (unsigned)time.wMinute);
        data.rows = {wg::Row{L"LOCAL TIME", text, L"", -1.0f, 0, L"", 0}};
    }

    // Declare the content height here and nowhere else: PaintBodyOf() draws
    // into exactly this many pixels, so deriving both from data.rows.size()
    // keeps the drawing and the frame in sync as rows are added.
    int BodyHeightOf(const ClockData& data) const override {
        return wg::skin::RowsHeight((int)data.rows.size());
    }

    void PaintBodyOf(const ClockData& data, wg::Canvas& canvas,
                     const wg::Rect& body) override {
        wg::skin::Rows(canvas, body, data.rows, theme_);
    }

private:
    wg::Theme theme_;
};

} // namespace

std::unique_ptr<wg::Module> CreateClockPanel() {
    return std::make_unique<ClockPanel>();
}
