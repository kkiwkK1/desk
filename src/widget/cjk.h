// Lazy CJK glyphs for the pixel canvas.
//
// The 5x7 bitmap font stays the ASCII path: ASCII text is drawn exactly as
// before and never touches GDI. A wide character (Han, kana, Hangul, fullwidth
// punctuation) is rasterized on first use from the system UI font at 14 px,
// cached as a 1-bpp mask, and blitted with the same premultiplied color. A
// widget that never shows CJK pays nothing: no font is created, no extra pages
// are touched.
//
// Font creation and rasterization happen on the UI thread. Width/line-height
// helpers are font-free so wrapping can run on a worker thread.
#pragma once

#include "widget/canvas.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wg {
namespace cjk {

// Han, kana, Hangul, CJK punctuation and fullwidth forms.
bool IsWide(wchar_t c);

// Font-free metrics: ASCII advances 6*scale px, wide glyphs 14 px.
float Advance(wchar_t c, float scale = kTextScale);
float TextWidth(std::wstring_view text, float scale = kTextScale);
int   LineHeight(std::wstring_view text, float scale = kTextScale);

// Draws ASCII runs with Canvas::Text and wide characters from the lazy cache.
// UI thread only.
void Text(Canvas& canvas, std::wstring_view text, float x, float y, Color color,
          float scale = kTextScale);

// Top-aligned variant: ASCII starts at `top` and a wide glyph's black box also
// starts at `top`. cjk::Text instead aligns wide glyphs to the 7px ASCII
// baseline, which is right for mixed prose but clips a 14px glyph at the top of
// a fixed-height cell (for example the first terminal row).
void TextTop(Canvas& canvas, std::wstring_view text, float x, float top, Color color,
             float scale = kTextScale);

// Pixel-width word wrap. ASCII words stay together; each wide character is a
// break opportunity. Truncates to maxLines and appends "..." when it does not
// fit. Font-free, so it is safe to call from RefreshData.
std::vector<std::wstring> Wrap(std::wstring_view text, int maxWidth,
                               std::size_t maxLines, float scale = kTextScale);

} // namespace cjk
} // namespace wg
