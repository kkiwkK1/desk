#include "widget/cjk.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wg {
namespace cjk {
namespace {

constexpr int kFontPx = 14;
constexpr int kWideAdvance = 14;
constexpr std::size_t kMaxGlyphs = 4096;

struct Glyph {
    int width = 0;
    int height = 0;
    int advance = 0;
    int originX = 0;  // black-box offset from the glyph origin
    int originY = 0;
    std::vector<unsigned char> bits;
};

struct FontState {
    HDC dc = nullptr;
    HFONT font = nullptr;
    HGDIOBJ old = nullptr;
    std::unordered_map<unsigned, Glyph> cache;

    ~FontState() {
        if (dc && old) SelectObject(dc, old);
        if (font) DeleteObject(font);
        if (dc) DeleteDC(dc);
    }

    bool hasGlyph(unsigned cp) {
        MAT2 mat{};
        mat.eM11.value = 1;
        mat.eM22.value = 1;
        GLYPHMETRICS gm{};
        const DWORD size = GetGlyphOutlineW(dc, cp, GGO_BITMAP, &gm, 0, nullptr, &mat);
        return size != GDI_ERROR && gm.gmBlackBoxX > 0;
    }

    bool create(const wchar_t* face) {
        LOGFONTW lf{};
        lf.lfHeight = -kFontPx;
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = NONANTIALIASED_QUALITY;
        lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);

        HFONT candidate = CreateFontIndirectW(&lf);
        if (!candidate) return false;
        HGDIOBJ previous = SelectObject(dc, candidate);
        if (!hasGlyph(0x4E2D)) {  // "中": does this face actually cover Han?
            if (previous) SelectObject(dc, previous);
            DeleteObject(candidate);
            return false;
        }
        font = candidate;
        old = previous;
        return true;
    }

    bool ready() {
        if (dc) return true;
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;

        if (create(L"Microsoft YaHei UI")) return true;
        if (create(L"Microsoft YaHei")) return true;
        if (create(L"SimSun")) return true;

        DeleteDC(dc);
        dc = nullptr;
        return false;
    }

    const Glyph* get(unsigned cp) {
        const auto found = cache.find(cp);
        if (found != cache.end()) return found->second.width ? &found->second : nullptr;
        if (!ready()) return nullptr;

        if (cache.size() >= kMaxGlyphs) cache.clear();

        MAT2 mat{};
        mat.eM11.value = 1;
        mat.eM22.value = 1;

        GLYPHMETRICS gm{};
        const DWORD size = GetGlyphOutlineW(dc, cp, GGO_BITMAP, &gm, 0, nullptr, &mat);
        if (size == GDI_ERROR || size == 0 || gm.gmBlackBoxX == 0) {
            cache.emplace(cp, Glyph{});
            return nullptr;
        }

        Glyph glyph;
        glyph.width = (int)gm.gmBlackBoxX;
        glyph.height = (int)gm.gmBlackBoxY;
        glyph.advance = (int)gm.gmCellIncX;
        glyph.originX = (int)gm.gmptGlyphOrigin.x;
        glyph.originY = (int)gm.gmptGlyphOrigin.y;
        glyph.bits.resize(size);
        if (GetGlyphOutlineW(dc, cp, GGO_BITMAP, &gm, size, glyph.bits.data(), &mat) ==
            GDI_ERROR) {
            cache.emplace(cp, Glyph{});
            return nullptr;
        }

        const auto inserted = cache.emplace(cp, std::move(glyph));
        return &inserted.first->second;
    }
};

FontState& Font() {
    static FontState state;
    return state;
}

bool IsAsciiSpace(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; }

// Like TextWidth but without dropping the trailing gap: wrapping compares
// concatenated token widths, where every gap is real padding.
float SumWidth(std::wstring_view text, float scale) {
    float width = 0.0f;
    for (const wchar_t c : text) width += Advance(c, scale);
    return width;
}

std::vector<std::wstring> Tokenize(std::wstring_view text) {
    std::vector<std::wstring> tokens;
    bool pendingSpace = false;

    std::size_t i = 0;
    while (i < text.size()) {
        const wchar_t c = text[i];
        if (IsAsciiSpace(c)) {
            pendingSpace = !tokens.empty();
            ++i;
            continue;
        }

        std::wstring token;
        if (pendingSpace) token.push_back(L' ');
        pendingSpace = false;

        if (IsWide(c)) {
            token.push_back(c);
            ++i;
        } else {
            const std::size_t start = i;
            while (i < text.size() && !IsAsciiSpace(text[i]) && !IsWide(text[i])) ++i;
            token.append(text.substr(start, i - start));
        }
        tokens.push_back(std::move(token));
    }
    return tokens;
}

} // namespace

bool IsWide(wchar_t c) {
    const unsigned cp = (unsigned)c;
    return (cp >= 0x1100 && cp <= 0x115F) ||    // Hangul Jamo
           (cp >= 0x2E80 && cp <= 0x303E) ||    // CJK radicals, punctuation
           (cp >= 0x3041 && cp <= 0x33FF) ||    // kana, CJK compatibility
           (cp >= 0x3400 && cp <= 0x4DBF) ||    // CJK ext A
           (cp >= 0x4E00 && cp <= 0x9FFF) ||    // CJK unified
           (cp >= 0xA000 && cp <= 0xA4CF) ||    // Yi
           (cp >= 0xAC00 && cp <= 0xD7A3) ||    // Hangul syllables
           (cp >= 0xF900 && cp <= 0xFAFF) ||    // compatibility ideographs
           (cp >= 0xFE30 && cp <= 0xFE4F) ||    // CJK compatibility forms
           (cp >= 0xFF00 && cp <= 0xFF60) ||    // fullwidth forms
           (cp >= 0xFFE0 && cp <= 0xFFE6);
}

float Advance(wchar_t c, float scale) {
    return IsWide(c) ? (float)kWideAdvance : 6.0f * scale;
}

float TextWidth(std::wstring_view text, float scale) {
    float width = 0.0f;
    for (const wchar_t c : text) width += Advance(c, scale);
    if (width > 0.0f) width -= 1.0f;
    return width;
}

int LineHeight(std::wstring_view text, float scale) {
    for (const wchar_t c : text) {
        if (IsWide(c)) return 15;
    }
    return (int)(7.0f * scale + 2.5f);
}

void Text(Canvas& canvas, std::wstring_view text, float x, float y, Color color, float scale) {
    float cx = x;
    std::wstring ascii;

    auto flushAscii = [&]() {
        if (ascii.empty()) return;
        canvas.Text(ascii, cx, y, color, scale);
        cx += (float)ascii.size() * 6.0f * scale;
        ascii.clear();
    };

    for (const wchar_t c : text) {
        if (!IsWide(c)) {
            ascii.push_back(c);
            continue;
        }

        flushAscii();
        const Glyph* glyph = Font().get((unsigned)c);
        if (!glyph) {
            canvas.Text(L"?", cx, y, color, scale);
            cx += 6.0f * scale;
            continue;
        }

        // Align with the 5x7 ASCII baseline and honor the glyph's own origin:
        // punctuation such as ，。 has a black box near the baseline and would
        // otherwise be painted at the top of the line.
        const int baseline = (int)(y + 7.0f * scale + 0.5f);
        const int left = (int)(cx + 0.5f) + glyph->originX;
        const int top = baseline - glyph->originY;
        const int stride = ((glyph->width + 31) / 32) * 4;
        for (int row = 0; row < glyph->height; ++row) {
            const unsigned char* line = glyph->bits.data() + row * stride;
            int runStart = -1;
            for (int col = 0; col <= glyph->width; ++col) {
                const bool on = col < glyph->width && (line[col >> 3] & (0x80 >> (col & 7)));
                if (on) {
                    if (runStart < 0) runStart = col;
                } else if (runStart >= 0) {
                    canvas.Fill(left + runStart, top + row, col - runStart, 1, color);
                    runStart = -1;
                }
            }
        }
        cx += (float)(glyph->advance > 0 ? glyph->advance : kWideAdvance);
    }
    flushAscii();
}

void TextTop(Canvas& canvas, std::wstring_view text, float x, float top, Color color,
             float scale) {
    float cx = x;
    std::wstring ascii;

    auto flushAscii = [&]() {
        if (ascii.empty()) return;
        canvas.Text(ascii, cx, top, color, scale);
        cx += (float)ascii.size() * 6.0f * scale;
        ascii.clear();
    };

    for (const wchar_t c : text) {
        if (!IsWide(c)) {
            ascii.push_back(c);
            continue;
        }

        flushAscii();
        const Glyph* glyph = Font().get((unsigned)c);
        if (!glyph) {
            canvas.Text(L"?", cx, top, color, scale);
            cx += 6.0f * scale;
            continue;
        }

        // Black-box top is exactly `top`; originX still positions the glyph
        // horizontally (punctuation has side bearings).
        const int glyphTop = (int)(top + 0.5f);
        const int left = (int)(cx + 0.5f) + glyph->originX;
        const int stride = ((glyph->width + 31) / 32) * 4;
        for (int row = 0; row < glyph->height; ++row) {
            const unsigned char* line = glyph->bits.data() + row * stride;
            int runStart = -1;
            for (int col = 0; col <= glyph->width; ++col) {
                const bool on = col < glyph->width && (line[col >> 3] & (0x80 >> (col & 7)));
                if (on) {
                    if (runStart < 0) runStart = col;
                } else if (runStart >= 0) {
                    canvas.Fill(left + runStart, glyphTop + row, col - runStart, 1, color);
                    runStart = -1;
                }
            }
        }
        cx += (float)(glyph->advance > 0 ? glyph->advance : kWideAdvance);
    }
    flushAscii();
}

std::vector<std::wstring> Wrap(std::wstring_view text, int maxWidth, std::size_t maxLines,
                               float scale) {
    if (maxLines == 0) return {};
    std::vector<std::wstring> lines;
    const std::vector<std::wstring> tokens = Tokenize(text);
    if (tokens.empty()) return {std::wstring()};
    if (maxWidth < 1) maxWidth = 1;

    std::wstring line;
    float lineWidth = 0.0f;

    auto flush = [&]() {
        if (!line.empty()) {
            lines.push_back(line);
            line.clear();
            lineWidth = 0.0f;
        }
    };

    for (std::wstring token : tokens) {
        float tokenWidth = SumWidth(token, scale);
        if (tokenWidth > (float)maxWidth) {
            // A single long token (URL, long English word): hard split it.
            if (!line.empty()) flush();
            while (SumWidth(token, scale) > (float)maxWidth) {
                std::size_t cut = 1;
                while (cut < token.size() &&
                       SumWidth(token.substr(0, cut + 1), scale) <= (float)maxWidth) {
                    ++cut;
                }
                lines.push_back(token.substr(0, cut));
                token.erase(0, cut);
            }
            line = token;
            lineWidth = SumWidth(line, scale);
            continue;
        }

        if (line.empty()) {
            if (!token.empty() && token.front() == L' ') token.erase(0, 1);
            line = std::move(token);
            lineWidth = SumWidth(line, scale);
        } else if (lineWidth + tokenWidth <= (float)maxWidth) {
            line += token;
            lineWidth += tokenWidth;
        } else {
            flush();
            if (!token.empty() && token.front() == L' ') token.erase(0, 1);
            line = std::move(token);
            lineWidth = SumWidth(line, scale);
        }
    }
    flush();

    if (lines.empty()) lines.push_back(std::wstring());
    if (lines.size() > maxLines) {
        lines.resize(maxLines);
        std::wstring& last = lines.back();
        while (!last.empty() && SumWidth(last + L"...", scale) > (float)maxWidth) {
            last.pop_back();
        }
        last += L"...";
    }
    return lines;
}

} // namespace cjk
} // namespace wg
