// Standalone probe: how expensive is a lazily built CJK glyph cache?
// Rasterizes 150 common Han characters with GDI GGO_BITMAP and reports
// working set / private bytes deltas plus glyph and blit timings.
#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Glyph {
    int width = 0;
    int height = 0;
    int advance = 0;
    std::vector<unsigned char> bits;
};

long long PrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc,
                              sizeof(pmc))) {
        return -1;
    }
    return (long long)pmc.PrivateUsage;
}

long long WorkingSet() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc,
                              sizeof(pmc))) {
        return -1;
    }
    return (long long)pmc.WorkingSetSize;
}

const wchar_t* kText =
    L"的一是了我不人在他有这个上们来到时大地为子中你说生国年着就那和要她出也得里后自以会家可下而过天去能对小多然于心学么之都好看起发当没成只如事把还用第样道想作种开美总从无情己面最女但现前些所同日手又行意动方期它头经长儿回位分爱老因很给名法间斯知世什两次使身者被高已亲其进此话常与活正感";

} // namespace

int wmain() {
    const long long privateBefore = PrivateBytes();
    const long long wsBefore = WorkingSet();

    const ULONGLONG start = GetTickCount64();

    HDC dc = CreateCompatibleDC(nullptr);
    LOGFONTW lf{};
    lf.lfHeight = -14;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = NONANTIALIASED_QUALITY;
    lstrcpynW(lf.lfFaceName, L"Microsoft YaHei UI", LF_FACESIZE);
    HFONT font = CreateFontIndirectW(&lf);
    HGDIOBJ old = SelectObject(dc, font);

    std::unordered_map<unsigned, Glyph> cache;
    int totalPixels = 0;
    int totalBytes = 0;

    const ULONGLONG glyphStart = GetTickCount64();
    for (const wchar_t* p = kText; *p; ++p) {
        const unsigned cp = (unsigned)*p;
        if (cache.count(cp)) continue;

        MAT2 mat{};
        mat.eM11.value = 1;
        mat.eM22.value = 1;

        GLYPHMETRICS gm{};
        const DWORD size = GetGlyphOutlineW(dc, cp, GGO_BITMAP, &gm, 0, nullptr, &mat);
        if (size == GDI_ERROR || gm.gmBlackBoxX == 0) continue;

        Glyph glyph;
        glyph.width = (int)gm.gmBlackBoxX;
        glyph.height = (int)gm.gmBlackBoxY;
        glyph.advance = (int)gm.gmCellIncX;
        glyph.bits.resize(size);
        if (GetGlyphOutlineW(dc, cp, GGO_BITMAP, &gm, size, glyph.bits.data(), &mat) ==
            GDI_ERROR) {
            continue;
        }
        totalPixels += glyph.width * glyph.height;
        totalBytes += (int)glyph.bits.size();
        cache.emplace(cp, std::move(glyph));
    }
    const ULONGLONG glyphEnd = GetTickCount64();

    // Blit all cached glyphs into a scratch DIB a thousand times, the way a
    // refresh would paint a full answer.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 320;
    bi.bmiHeader.biHeight = -160;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBitmap = SelectObject(dc, dib);

    const ULONGLONG blitStart = GetTickCount64();
    unsigned* pixels = (unsigned*)bits;
    for (int pass = 0; pass < 1000; ++pass) {
        int x = 4, y = 20;
        for (const auto& kv : cache) {
            const Glyph& g = kv.second;
            for (int row = 0; row < g.height; ++row) {
                const unsigned char* line = g.bits.data() + row * ((g.width + 31) / 32) * 4;
                for (int col = 0; col < g.width; ++col) {
                    if (line[col >> 3] & (0x80 >> (col & 7))) {
                        const int px = x + col, py = y + row;
                        if (px < 320 && py < 160) pixels[py * 320 + px] = 0xFFF2F2F6;
                    }
                }
            }
            x += g.advance;
            if (x > 280) { x = 4; y += 16; if (y > 140) { y = 20; } }
        }
    }
    const ULONGLONG blitEnd = GetTickCount64();

    SelectObject(dc, oldBitmap);
    DeleteObject(dib);
    SelectObject(dc, old);
    DeleteObject(font);
    DeleteDC(dc);

    const long long privateAfter = PrivateBytes();
    const long long wsAfter = WorkingSet();

    std::printf("glyphs: %zu, pixels: %d, raw bytes: %d\n", cache.size(), totalPixels,
                totalBytes);
    std::printf("font+cache: %llu ms\n", (unsigned long long)(glyphEnd - glyphStart));
    std::printf("1000 full blits: %llu ms\n", (unsigned long long)(blitEnd - blitStart));
    std::printf("private: %lld KB -> %lld KB (delta %lld KB)\n", privateBefore / 1024,
                privateAfter / 1024, (privateAfter - privateBefore) / 1024);
    std::printf("working set: %lld KB -> %lld KB (delta %lld KB)\n", wsBefore / 1024,
                wsAfter / 1024, (wsAfter - wsBefore) / 1024);
    std::printf("total probe: %llu ms\n", (unsigned long long)(blitEnd - start));
    return 0;
}
