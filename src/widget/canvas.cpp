#include "widget/canvas.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace wg {
namespace {

struct Glyph {
    wchar_t c;
    unsigned char r[7];
};

// 5x7 bitmap font. Add a row to teach the framework a new character; lowercase
// input is folded to uppercase, unknown characters render as a space.
const Glyph kGlyphs[] = {
    {L' ', {0,0,0,0,0,0,0}},
    {L'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {L'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {L'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {L'3', {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {L'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {L'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {L'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {L'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {L'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {L'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {L'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {L'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {L'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {L'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {L'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {L'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {L'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
    {L'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {L'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {L'J', {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {L'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {L'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {L'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {L'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {L'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {L'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {L'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {L'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {L'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {L'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {L'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {L'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {L'W', {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
    {L'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {L'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {L'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    {L'%', {0x19,0x1A,0x02,0x04,0x08,0x0B,0x13}},
    {L'.', {0,0,0,0,0,0x04,0x04}},
    {L'-', {0,0,0,0x0E,0,0,0}},
    {L':', {0,0x04,0x04,0,0x04,0x04,0}},
    {L'/', {0x01,0x02,0x02,0x04,0x08,0x08,0x10}},
    {L'+', {0,0x04,0x04,0x1F,0x04,0x04,0}},
    {L'*', {0,0x04,0x15,0x0E,0x15,0x04,0}},
    {L'_', {0,0,0,0,0,0,0x1F}},
};

const Glyph& FindGlyph(wchar_t c) {
    if (c >= L'a' && c <= L'z') c = (wchar_t)(c - 32);
    for (const auto& g : kGlyphs) {
        if (g.c == c) return g;
    }
    return kGlyphs[0];
}

inline int Rnd(float v) { return (int)(v + 0.5f); }

} // namespace

Canvas::~Canvas() { Release(); }

void Canvas::Release() {
    if (dc_ && prev_) {
        SelectObject(dc_, prev_);
        prev_ = nullptr;
    }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    if (dc_) { DeleteDC(dc_); dc_ = nullptr; }
    px_ = nullptr;
    w_ = h_ = 0;
    clipped_ = false;
    clipX_ = clipY_ = clipW_ = clipH_ = 0;
}

bool Canvas::Resize(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (px_ && w == w_ && h == h_) return true;

    Release();

    HDC screen = GetDC(nullptr);
    dc_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc_) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    dib_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib_ || !bits) { Release(); return false; }

    prev_ = (HBITMAP)SelectObject(dc_, dib_);
    px_ = (Color*)bits;
    w_ = w;
    h_ = h;
    return true;
}

void Canvas::Clear(Color c) {
    if (!px_) return;
    std::fill(px_, px_ + (size_t)w_ * h_, c);
}

void Canvas::SetClip(int x, int y, int w, int h) {
    if (clipped_) {
        const int x0 = std::max(x, clipX_);
        const int y0 = std::max(y, clipY_);
        const int x1 = std::min(x + std::max(w, 0), clipX_ + clipW_);
        const int y1 = std::min(y + std::max(h, 0), clipY_ + clipH_);
        clipX_ = x0;
        clipY_ = y0;
        clipW_ = std::max(0, x1 - x0);
        clipH_ = std::max(0, y1 - y0);
    } else {
        clipX_ = x;
        clipY_ = y;
        clipW_ = std::max(0, w);
        clipH_ = std::max(0, h);
        clipped_ = true;
    }
}

void Canvas::ResetClip() {
    clipped_ = false;
    clipX_ = clipY_ = clipW_ = clipH_ = 0;
}

void Canvas::Fill(int x, int y, int w, int h, Color c) {
    if (!px_ || w <= 0 || h <= 0) return;
    int x0 = std::max(x, 0);
    int y0 = std::max(y, 0);
    int x1 = std::min(x + w, w_);
    int y1 = std::min(y + h, h_);
    if (clipped_) {
        x0 = std::max(x0, clipX_);
        y0 = std::max(y0, clipY_);
        x1 = std::min(x1, clipX_ + clipW_);
        y1 = std::min(y1, clipY_ + clipH_);
    }
    if (x0 >= x1 || y0 >= y1) return;
    for (int yy = y0; yy < y1; ++yy) {
        Color* row = px_ + (size_t)yy * w_;
        std::fill(row + x0, row + x1, c);
    }
}

void Canvas::HLine(int x0, int x1, int y, Color c) { Fill(x0, y, x1 - x0, 1, c); }

void Canvas::Rect(int x, int y, int w, int h, Color c) {
    HLine(x, x + w, y, c);
    HLine(x, x + w, y + h - 1, c);
    Fill(x, y + 1, 1, h - 2, c);
    Fill(x + w - 1, y + 1, 1, h - 2, c);
}

float Canvas::TextWidth(std::wstring_view t, float scale) const {
    if (t.empty()) return 0.0f;
    return (float)t.size() * 6.0f * scale - scale;
}

void Canvas::Text(std::wstring_view t, float x, float y, Color c, float scale) {
    if (!px_) return;
    float cx = x;
    for (wchar_t ch : t) {
        const Glyph& g = FindGlyph(ch);
        for (int r = 0; r < 7; ++r) {
            unsigned bits = g.r[r];
            if (!bits) continue;
            int y0 = Rnd(y + r * scale);
            int y1 = Rnd(y + (r + 1) * scale);
            for (int col = 0; col < 5; ++col) {
                if (bits & (0x10u >> col)) {
                    int x0 = Rnd(cx + col * scale);
                    int x1 = Rnd(cx + (col + 1) * scale);
                    Fill(x0, y0, x1 - x0, y1 - y0, c);
                }
            }
        }
        cx += 6.0f * scale;
    }
}

void Canvas::TextRight(std::wstring_view t, int right, float y, Color c, float scale) {
    Text(t, (float)right - TextWidth(t, scale), y, c, scale);
}

void Canvas::TextCentered(std::wstring_view t, int centerX, float y, Color c, float scale) {
    Text(t, (float)centerX - TextWidth(t, scale) / 2.0f, y, c, scale);
}

void Canvas::Bar(int x, int y, int w, int h, float frac, Color fill, Color track) {
    Fill(x, y, w, h, track);
    if (frac < 0.0f) return;
    if (frac > 1.0f) frac = 1.0f;
    int filled = (int)(w * frac);
    if (filled > 0) Fill(x, y, filled, h, fill);
}

void Canvas::Panel(const Theme& t, bool active, bool solid) {
    if (!px_) return;
    const Color bg = solid ? t.panelSolid : t.panel;

    Fill(2, 0, w_ - 4, 1, bg);
    Fill(2, h_ - 1, w_ - 4, 1, bg);
    Fill(0, 2, w_, h_ - 4, bg);
    Fill(1, 1, w_ - 2, 1, bg);
    Fill(1, h_ - 2, w_ - 2, 1, bg);

    const Color border = active ? t.borderEdit : t.border;
    HLine(2, w_ - 2, 0, border);
    HLine(2, w_ - 2, h_ - 1, border);
    Fill(0, 2, 1, h_ - 4, border);
    Fill(w_ - 1, 2, 1, h_ - 4, border);
}

void Canvas::Present(HWND hwnd, int x, int y) const {
    if (!dc_ || !hwnd) return;
    POINT src{0, 0};
    SIZE size{w_, h_};
    POINT dst{x, y};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, nullptr, &dst, &size, dc_, &src, 0, &bf, ULW_ALPHA);
}

HICON Canvas::ToIcon() const {
    if (!px_) return nullptr;

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w_;
    bi.bV5Height = -h_;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return nullptr;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        DeleteDC(dc);
        return nullptr;
    }
    std::memcpy(bits, px_, (size_t)w_ * h_ * sizeof(Color));

    const int maskStride = ((w_ + 15) / 16) * 2;
    std::vector<BYTE> zero((size_t)maskStride * h_, 0);
    HBITMAP mask = CreateBitmap(w_, h_, 1, 1, zero.data());

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);

    if (mask) DeleteObject(mask);
    DeleteObject(color);
    DeleteDC(dc);
    return icon;
}

} // namespace wg
