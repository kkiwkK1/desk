// Framework card sizing policy. All sizes are logical board pixels.
//
// Width comes from a small set of presets. Compact/Standard/Wide have fixed
// widths; Full is measured from the board's interior width, so a full-width
// card really spans the usable card area (1252 px on a 1280 px board) instead
// of a hard-coded number that no longer matches the panel.
//
// Heights are quantized upward to kCardSizeStep so content is never shortened
// by normalization. The preset height range is a validation boundary, never a
// clipping instruction: an oversized component is reported and the caller
// decides how to present or handle it.
#pragma once

#include "widget/skin.h"

#include <algorithm>
#include <climits>
#include <cstdint>

namespace wg {

// Auto maps a legacy ModuleSpec::width upward to a preset so existing component
// contents are not narrowed. Existing aggregate initializers remain compatible.
enum class SizePreset { Auto, Compact, Standard, Wide, Full };

struct SizeRule {
    SizePreset preset;
    int width;
    int minHeight;
    int maxHeight;
};

struct CardSize {
    SizePreset preset = SizePreset::Standard;
    int width = 340;
    int height = 0;
    // false when the width does not match the preset exactly or the measured
    // height leaves [minHeight, maxHeight]. The measured height is still
    // returned without truncation when representable.
    bool withinPreset = false;
    // Height is inside [minHeight, maxHeight] regardless of the width match.
    // The board uses this to flag an oversized card (TOO TALL) without ever
    // clipping its content.
    bool heightWithinRange = false;
    // false only for a negative body, an unknown preset, no panel context for
    // Full, a non-positive width, or integer overflow.
    bool representable = false;
};

constexpr int kCardSizeStep = 8;
constexpr int kCardChromeHeight =
    skin::kCardHeaderH + skin::kCardPadTop + skin::kCardPadBottom;

// 'availableWidth' is the board interior width (the space cards get). Full uses
// it directly; width 0 means "no panel context", so Full cannot be measured and
// no silent fallback width is invented. Other presets ignore the argument.
inline SizeRule SizePresetRule(SizePreset preset, int availableWidth = 0) {
    switch (preset) {
    case SizePreset::Compact:  return {preset, 256, 96, 512};
    case SizePreset::Standard: return {preset, 340, 112, 640};
    case SizePreset::Wide:     return {preset, 568, 168, 720};
    case SizePreset::Full:     return {preset, std::max(availableWidth, 0), 208, 720};
    case SizePreset::Auto:     return {SizePreset::Standard, 340, 112, 640};
    }
    return {SizePreset::Standard, 340, 112, 640};
}

inline bool IsSizePreset(SizePreset preset) {
    return preset == SizePreset::Auto || preset == SizePreset::Compact ||
           preset == SizePreset::Standard || preset == SizePreset::Wide ||
           preset == SizePreset::Full;
}

// Preset a request maps to. Explicit presets are returned unchanged; Auto picks
// the narrowest fixed preset that can hold the legacy width without narrowing,
// and Full when the legacy card is wider than Wide (the board then keeps the
// legacy width, see MeasureCardSize). availableWidth is only needed so an Auto
// card wider than the interior still reports a deterministic preset.
inline SizePreset ResolveSizePreset(SizePreset requested, int legacyWidth,
                                    int availableWidth = 0) {
    (void)availableWidth;
    if (requested != SizePreset::Auto && IsSizePreset(requested)) return requested;
    if (legacyWidth <= 0) return SizePreset::Standard;
    const SizePreset presets[] = {SizePreset::Compact, SizePreset::Standard,
                                  SizePreset::Wide};
    for (SizePreset preset : presets) {
        if (legacyWidth <= SizePresetRule(preset).width) return preset;
    }
    return SizePreset::Full;
}

// Height includes card chrome (title bar and padding). maxHeight is a
// validation boundary, never a clipping instruction.
//
// Width policy:
//  - explicit Compact/Standard/Wide: the preset width.
//  - explicit Full: the board interior width, so it follows panel resizes.
//  - Auto (legacy): the component's declared width is kept exactly, because a
//    compatibility mapping must never silently narrow or re-layout old content;
//    the resolved preset only supplies height bounds and classification.
inline CardSize MeasureCardSize(SizePreset requested, int legacyWidth, int bodyHeight,
                                int availableWidth = 0) {
    CardSize result;
    if (!IsSizePreset(requested)) return result;

    const SizePreset preset = ResolveSizePreset(requested, legacyWidth, availableWidth);
    const SizeRule rule = SizePresetRule(preset, availableWidth);
    result.preset = preset;

    if (requested == SizePreset::Auto) {
        result.width = legacyWidth > 0 ? legacyWidth : rule.width;
    } else {
        result.width = rule.width;
    }
    if (result.width <= 0) return result;
    if (bodyHeight < 0) return result;

    const std::int64_t desired = std::max<std::int64_t>(
        rule.minHeight, (std::int64_t)bodyHeight + kCardChromeHeight);
    const std::int64_t height =
        ((desired + kCardSizeStep - 1) / kCardSizeStep) * kCardSizeStep;
    if (height > INT_MAX) return result;

    result.height = (int)height;
    result.representable = true;
    result.heightWithinRange = height >= rule.minHeight && height <= rule.maxHeight;
    result.withinPreset =
        result.width == rule.width && result.heightWithinRange;
    return result;
}

} // namespace wg
