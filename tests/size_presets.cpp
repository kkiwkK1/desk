#include "widget/size.h"
#include "widget/module.h"

#include <climits>
#include <cstdio>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[size-test] FAIL %s\n", message);
        ++failures;
    }
}

} // namespace

int main() {
    using namespace wg;

    const ModuleSpec legacy{L"legacy", L"LEGACY", L"", 340, 60, false, false, {}};
    Check(legacy.sizePreset == SizePreset::Auto,
          "existing aggregate initializer remains valid");

    Check(ResolveSizePreset(SizePreset::Auto, 340) == SizePreset::Standard,
          "legacy standard width");
    Check(ResolveSizePreset(SizePreset::Auto, 568) == SizePreset::Wide,
          "legacy terminal width");
    Check(ResolveSizePreset(SizePreset::Auto, 0) == SizePreset::Standard,
          "unset width uses standard");
    Check(ResolveSizePreset(SizePreset::Auto, 298) == SizePreset::Standard,
          "legacy width rounds upward without narrowing");
    Check(ResolveSizePreset(SizePreset::Auto, 400) == SizePreset::Wide,
          "legacy intermediate width is not narrowed");
    Check(ResolveSizePreset(SizePreset::Full, 340) == SizePreset::Full,
          "explicit preset overrides legacy width");

    // Preset widths are unchanged; Full follows the board interior instead of
    // the 700 px draft that no longer matched a 1280 px board.
    Check(SizePresetRule(SizePreset::Compact).width == 256 &&
              SizePresetRule(SizePreset::Standard).width == 340 &&
              SizePresetRule(SizePreset::Wide).width == 568,
          "fixed preset widths");
    Check(SizePresetRule(SizePreset::Full, 1252).width == 1252,
          "full width is measured from the board interior");
    Check(SizePresetRule(SizePreset::Full).width == 0,
          "full width without a panel context is unknown, not 700");
    Check(kCardChromeHeight == 48, "card chrome height");

    const CardSize account = MeasureCardSize(SizePreset::Auto, 340, 96);
    Check(account.representable && account.withinPreset && account.width == 340 &&
              account.height == 144,
          "account collapsed dimensions are preserved");
    const CardSize detail = MeasureCardSize(SizePreset::Standard, 340, 221);
    Check(detail.representable && detail.withinPreset && detail.height == 272 &&
              detail.height >= 221 + kCardChromeHeight,
          "expanded height rounds upward without losing content");
    const CardSize terminalSmall = MeasureCardSize(SizePreset::Wide, 0, 120);
    const CardSize terminalLarge = MeasureCardSize(SizePreset::Wide, 0, 344);
    Check(terminalSmall.width == 568 && terminalSmall.height == 168 &&
              terminalLarge.width == 568 && terminalLarge.height == 392,
          "80-column terminal geometry is preserved");
    const CardSize compact = MeasureCardSize(SizePreset::Compact, 0, 1);
    Check(compact.width == 256 && compact.height == 96 && compact.withinPreset,
          "compact preset enforces minimum height");
    Check(MeasureCardSize(SizePreset::Standard, 0, 65).height == 120,
          "heights round up to the 8 px step");

    const CardSize full = MeasureCardSize(SizePreset::Full, 0, 160, 1252);
    Check(full.representable && full.withinPreset && full.width == 1252 &&
              full.height == 208,
          "full-width preset spans the 1280 board interior");
    Check(MeasureCardSize(SizePreset::Full, 0, 160, 1000).width == 1000,
          "full width follows panel resizes");
    Check(!MeasureCardSize(SizePreset::Full, 0, 160).representable,
          "full width without a panel context cannot be measured");
    const CardSize fullTall = MeasureCardSize(SizePreset::Full, 0, 900, 1252);
    Check(fullTall.representable && !fullTall.withinPreset &&
              fullTall.height >= 900 + kCardChromeHeight,
          "full-width overflow is reported without clipping");

    const CardSize overflow = MeasureCardSize(SizePreset::Standard, 340, 900);
    Check(overflow.representable && !overflow.withinPreset &&
              overflow.height >= 900 + kCardChromeHeight,
          "oversized content is reported without clipping");
    const CardSize tooWide = MeasureCardSize(SizePreset::Auto, 800, 100, 1252);
    Check(tooWide.representable && !tooWide.withinPreset && tooWide.width == 800,
          "oversized legacy width is reported and not narrowed");
    const CardSize oddLegacy = MeasureCardSize(SizePreset::Auto, 298, 96);
    Check(oddLegacy.representable && oddLegacy.width == 298 && !oddLegacy.withinPreset,
          "legacy Auto keeps its declared width instead of re-laying out content");
    Check(!MeasureCardSize(SizePreset::Standard, 340, -1).representable,
          "negative height is rejected");
    Check(!MeasureCardSize(SizePreset::Standard, 340, INT_MAX).representable,
          "integer overflow is rejected");
    Check(!MeasureCardSize(static_cast<SizePreset>(42), 340, 96).representable,
          "unknown preset is rejected");

    std::printf("[size-test] %s\n", failures ? "failed" : "ok");
    return failures == 0 ? 0 : 1;
}
