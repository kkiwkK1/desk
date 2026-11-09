// AiMeter - pixel desktop widget for AI usage.
// No GPU, no D2D, no font files: one DIB + a hand-written 5x7 bitmap font.
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <wincred.h>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iterator>

namespace fs = std::filesystem;

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

// ---------------------------------------------------------------- 5x7 font
struct Glyph { wchar_t c; unsigned char r[7]; };
static const Glyph kGlyphs[] = {
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

static const Glyph* FindGlyph(wchar_t c) {
    if (c >= L'a' && c <= L'z') c = (wchar_t)(c - 32);
    for (const auto& g : kGlyphs) if (g.c == c) return &g;
    return &kGlyphs[0];
}

// ---------------------------------------------------------------- layout
static const int   kWidth   = 280;
static const float kScale   = 1.5f;
static const float kAdvance = 6.0f * kScale;
static const float kGlyphW  = 5.0f * kScale;
static const float kGlyphH  = 7.0f * kScale;
static const int   kPad     = 8;
static const int   kRowH    = 33;   // 1.5x the original 22px row pitch
static const int   kBarH    = 4;
static const int   kBarGap  = 8;    // vertical gap between a row's text and its bar

static const int   kDlgW    = 360;
static const int   kDlgH    = 92;
static const int   kFieldX  = 10;
static const int   kFieldY  = 34;
static const int   kFieldW  = kDlgW - 20;
static const int   kFieldH  = 24;

// ---------------------------------------------------------------- surface
struct Surface { HDC dc = nullptr; HBITMAP dib = nullptr; DWORD* px = nullptr; int w = 0, h = 0; };
static Surface g_main;

static HWND g_hwnd = nullptr;
static int  g_x = 0, g_y = 0;
static bool g_edit = false;

struct Row {
    std::wstring name;
    std::wstring value;
    std::wstring sub;          // small caption on the bar line (e.g. today's spend)
    float frac = -1.0f;
    DWORD accent = 0x4ADE80;
};
static std::vector<Row> g_rows;

static std::wstring g_key;      // DeepSeek key, mirrored from Credential Manager
static std::wstring g_input;    // live edit buffer

static std::wstring g_day;        // "YYYY-MM-DD" of the snapshot below
static long long    g_dayStart = 0;  // balance at the start of that day, in cents
static long long    g_lastCents = 0; // most recent balance seen, in cents

static inline DWORD Px(DWORD rgb, int a) {
    DWORD r = ((rgb >> 16) & 0xFF) * a / 255;
    DWORD g = ((rgb >> 8) & 0xFF) * a / 255;
    DWORD b = (rgb & 0xFF) * a / 255;
    return ((DWORD)a << 24) | (r << 16) | (g << 8) | b;
}

static void HLine(Surface& s, int x0, int x1, int y, DWORD c) {
    if (y < 0 || y >= s.h) return;
    if (x0 < 0) x0 = 0;
    if (x1 > s.w) x1 = s.w;
    DWORD* p = s.px + (size_t)y * s.w;
    for (int x = x0; x < x1; ++x) p[x] = c;
}

static void Fill(Surface& s, int x, int y, int w, int h, DWORD c) {
    for (int i = 0; i < h; ++i) HLine(s, x, x + w, y + i, c);
}

static float TextWidth(const std::wstring& t, float scale) {
    if (t.empty()) return 0.0f;
    return (float)t.size() * 6.0f * scale - 1.0f * scale;
}

static inline int Rnd(float v) { return (int)(v + 0.5f); }

static void DrawText(Surface& s, const std::wstring& t, float x, float y, float scale, DWORD c) {
    float cx = x;
    for (wchar_t ch : t) {
        const Glyph* g = FindGlyph(ch);
        for (int r = 0; r < 7; ++r) {
            unsigned bits = g->r[r];
            if (!bits) continue;
            int y0 = Rnd(y + r * scale);
            int y1 = Rnd(y + (r + 1) * scale);
            for (int col = 0; col < 5; ++col) {
                if (bits & (0x10u >> col)) {
                    int x0 = Rnd(cx + col * scale);
                    int x1 = Rnd(cx + (col + 1) * scale);
                    Fill(s, x0, y0, x1 - x0, y1 - y0, c);
                }
            }
        }
        cx += 6.0f * scale;
    }
}

static void DrawPanel(Surface& s, int w, int h, bool active, int alpha = 200) {
    const DWORD bg = Px(0x0E0E12, alpha);
    Fill(s, 2, 0, w - 4, 1, bg);
    Fill(s, 2, h - 1, w - 4, 1, bg);
    Fill(s, 0, 2, w, h - 4, bg);
    Fill(s, 1, 1, w - 2, 1, bg);
    Fill(s, 1, h - 2, w - 2, 1, bg);

    const DWORD border = active ? Px(0x60A5FA, 255) : Px(0x2A2A32, 255);
    HLine(s, 2, w - 2, 0, border);
    HLine(s, 2, w - 2, h - 1, border);
    Fill(s, 0, 2, 1, h - 4, border);
    Fill(s, w - 1, 2, 1, h - 4, border);
}

static void ReleaseSurface(Surface& s) {
    if (s.dib) { DeleteObject(s.dib); s.dib = nullptr; }
    if (s.dc) { DeleteDC(s.dc); s.dc = nullptr; }
    s.px = nullptr;
    s.w = s.h = 0;
}

static bool BuildSurface(Surface& s, int w, int h) {
    ReleaseSurface(s);
    if (h < 1) h = 1;

    HDC screen = GetDC(nullptr);
    s.dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    s.dib = CreateDIBSection(s.dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!s.dib || !bits) return false;
    SelectObject(s.dc, s.dib);
    s.px = (DWORD*)bits;
    s.w = w;
    s.h = h;
    return true;
}

static void Present(Surface& s, HWND hwnd, int x, int y) {
    POINT src{0, 0};
    SIZE  size{s.w, s.h};
    POINT dst{x, y};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, nullptr, &dst, &size, s.dc, &src, 0, &bf, ULW_ALPHA);
}

// ---------------------------------------------------------------- credentials
static const wchar_t* kCredTarget = L"AiMeter/DeepSeek";
static const wchar_t* kCredUser   = L"deepseek";

static bool SaveSecret(const std::wstring& secret) {
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = const_cast<LPWSTR>(kCredTarget);
    c.UserName = const_cast<LPWSTR>(kCredUser);
    c.CredentialBlobSize = (DWORD)(secret.size() * sizeof(wchar_t));
    c.CredentialBlob = (LPBYTE)secret.c_str();
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&c, 0) != FALSE;
}

static std::wstring LoadSecret() {
    PCREDENTIALW c = nullptr;
    if (!CredReadW(kCredTarget, CRED_TYPE_GENERIC, 0, &c)) return L"";
    std::wstring out((const wchar_t*)c->CredentialBlob, c->CredentialBlobSize / sizeof(wchar_t));
    CredFree(c);
    return out;
}

// ---------------------------------------------------------------- http
static bool HttpGet(const wchar_t* host, const wchar_t* path, const std::wstring& token,
                    std::string& body, DWORD& status) {
    body.clear();
    status = 0;

    // Direct connection on purpose: WPAD auto-discovery can stall for ~30s on
    // networks without a proxy, and the APIs we call are reachable directly.
    HINTERNET s = WinHttpOpen(L"AiMeter/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 3000, 3000, 5000, 8000);
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!c) { WinHttpCloseHandle(s); return false; }
    HINTERNET r = WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!r) { WinHttpCloseHandle(c); WinHttpCloseHandle(s); return false; }

    std::wstring hdr = L"Authorization: Bearer " + token + L"\r\nAccept: application/json";
    BOOL ok = WinHttpSendRequest(r, hdr.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(r, nullptr);

    if (ok) {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        status = code;

        DWORD avail = 0;
        do {
            avail = 0;
            if (!WinHttpQueryDataAvailable(r, &avail)) break;
            if (!avail) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(r, chunk.data(), avail, &read)) break;
            chunk.resize(read);
            body += chunk;
        } while (avail > 0);
    }

    WinHttpCloseHandle(r);
    WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

// ---------------------------------------------------------------- codex source
static double NumAfter(const std::string& s, const char* key, size_t from) {
    size_t p = s.find(key, from);
    if (p == std::string::npos) return -1.0;
    p += std::strlen(key);
    return std::strtod(s.c_str() + p, nullptr);
}

static bool ReadCodex(float& p5, float& p7) {
    wchar_t buf[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH)) return false;
    fs::path base = fs::path(buf) / L".codex" / L"sessions";
    std::error_code ec;
    if (!fs::exists(base, ec)) return false;

    fs::path newest;
    fs::file_time_type best{};
    for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != L".jsonl") continue;
        auto t = it->last_write_time(ec);
        if (ec) continue;
        if (newest.empty() || t > best) { best = t; newest = it->path(); }
    }
    if (newest.empty()) return false;

    std::ifstream in(newest, std::ios::binary);
    if (!in) return false;
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    size_t p = s.rfind("\"rate_limits\"");
    if (p == std::string::npos) return false;
    double a = NumAfter(s, "\"primary\":{\"used_percent\":", p);
    double b = NumAfter(s, "\"secondary\":{\"used_percent\":", p);
    if (a < 0 || b < 0) return false;
    p5 = (float)a;
    p7 = (float)b;
    return true;
}

// ---------------------------------------------------------------- deepseek
struct DeepSeek {
    bool ok = false;
    long long cents = 0;
    std::wstring text;
};

static long long ParseCents(const std::string& s) {
    long long whole = 0, frac = 0;
    int digits = 0;
    size_t i = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) whole = whole * 10 + (s[i] - '0');
    if (i < s.size() && s[i] == '.') {
        for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 2; ++i, ++digits)
            frac = frac * 10 + (s[i] - '0');
    }
    while (digits < 2) { frac *= 10; ++digits; }
    return whole * 100 + frac;
}

static std::wstring Money(long long cents) {
    wchar_t buf[32];
    long long a = cents < 0 ? -cents : cents;
    swprintf(buf, 32, L"%s%lld.%02lld", cents < 0 ? L"-" : L"", a / 100, a % 100);
    return buf;
}

static DeepSeek FetchDeepSeek() {
    DeepSeek r;
    if (g_key.empty()) { r.text = L"NO KEY"; return r; }

    std::string body;
    DWORD status = 0;
    if (!HttpGet(L"api.deepseek.com", L"/user/balance", g_key, body, status)) {
        r.text = L"NET ERR";
        return r;
    }
    if (status != 200) {
        r.text = status == 401 ? L"BAD KEY" : L"ERR " + std::to_wstring(status);
        return r;
    }

    size_t p = body.find("\"total_balance\":\"");
    if (p == std::string::npos) { r.text = L"NO DATA"; return r; }
    p += 17;
    size_t e = body.find('"', p);
    if (e == std::string::npos) { r.text = L"NO DATA"; return r; }

    std::string num = body.substr(p, e - p);
    num.erase(std::remove(num.begin(), num.end(), ','), num.end());

    r.ok = true;
    r.cents = ParseCents(num);
    r.text = L"CNY " + std::wstring(num.begin(), num.end());
    return r;
}

// Today's spend = opening balance of the day minus the current balance.
// A day rollover uses the last balance we saw as the new opening value.
static void UpdateDay(long long cents) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t today[16];
    swprintf(today, 16, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);

    if (g_day != today) {
        g_day = today;
        g_dayStart = (g_lastCents > 0) ? g_lastCents : cents;
    }
    g_lastCents = cents;
}

static void Refresh();
static void SaveConfig();

static void RenderMain() {
    if (!g_main.px) return;
    std::memset(g_main.px, 0, (size_t)g_main.w * g_main.h * 4);
    DrawPanel(g_main, kWidth, g_main.h, g_edit);

    int y = kPad;
    for (const auto& row : g_rows) {
        DrawText(g_main, row.name, (float)kPad, (float)y, kScale, Px(0xB9B9C4, 255));
        float vw = TextWidth(row.value, kScale);
        DrawText(g_main, row.value, (float)kWidth - kPad - vw, (float)y, kScale, Px(0xF2F2F6, 255));

        int barY = y + (int)kGlyphH + kBarGap;
        int barW = kWidth - kPad * 2;
        if (!row.sub.empty()) {
            float sw = TextWidth(row.sub, 1.0f);
            DrawText(g_main, row.sub, (float)kWidth - kPad - sw, (float)barY, 1.0f, Px(0x8A8A96, 255));
            y += kRowH;
            continue;
        }
        Fill(g_main, kPad, barY, barW, kBarH, Px(0x2A2A34, 255));
        if (row.frac >= 0.0f) {
            float f = row.frac > 1.0f ? 1.0f : row.frac;
            int w = (int)(barW * f);
            if (w > 0) Fill(g_main, kPad, barY, w, kBarH, Px(row.accent, 255));
        }
        y += kRowH;
    }
    Present(g_main, g_hwnd, g_x, g_y);
}

static std::wstring g_dsCache = L"...";

static void Refresh() {
    g_rows.clear();

    Row ds{L"DEEPSEEK", g_key.empty() ? L"NO KEY" : g_dsCache, L"", -1.0f, 0x4ADE80};
    g_rows.push_back(ds);

    Row cx{L"CODEX", L"", L"", -1.0f, 0x4ADE80};
    float p5 = 0, p7 = 0;
    if (ReadCodex(p5, p7)) {
        wchar_t v[64];
        swprintf(v, 64, L"%d%% / %d%%", (int)(p5 + 0.5f), (int)(p7 + 0.5f));
        cx.value = v;
        cx.frac = p5 / 100.0f;
        cx.accent = p5 >= 90 ? 0xF87171 : (p5 >= 70 ? 0xFBBF24 : 0x4ADE80);
    } else {
        cx.value = L"NO DATA";
    }
    g_rows.push_back(cx);

    static constexpr bool kShowZai = false;
    if (kShowZai) g_rows.push_back(Row{L"Z.AI", L"--", L"", -1.0f, 0x7A7A86});

    const int contentH = (int)kGlyphH + kBarGap + kBarH;   // text + gap + bar
    int want = kPad * 2 + (int)(g_rows.size() - 1) * kRowH + contentH;
    if (want != g_main.h) {
        int sh = GetSystemMetrics(SM_CYSCREEN);
        int ny = g_y;
        if (ny + want > sh) ny = sh - want - 16;
        if (ny < 0) ny = 0;
        BuildSurface(g_main, kWidth, want);
        SetWindowPos(g_hwnd, nullptr, g_x, ny, kWidth, want,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        g_y = ny;
    }
    RenderMain();   // paint cached values first, never block on the network

    if (!g_key.empty()) {
        DeepSeek ds = FetchDeepSeek();
        g_dsCache = ds.text;
        g_rows[0].value = ds.text;
        g_rows[0].accent = ds.ok ? 0x4ADE80 : 0xF87171;
        if (ds.ok) {
            UpdateDay(ds.cents);
            long long spent = g_dayStart - ds.cents;
            std::wstring sign = spent > 0 ? L"-" : (spent < 0 ? L"+" : L"");
            g_rows[0].sub = sign + Money(spent) + L" TODAY";
            SaveConfig();
        }
        RenderMain();
    }
}

// ---------------------------------------------------------------- settings dialog
static HWND g_dlg = nullptr;

static std::wstring Masked() {
    if (g_input.empty()) return L"";
    size_t n = g_input.size();
    if (n <= 8) return std::wstring(n, L'*');
    return g_input.substr(0, 4) + L"***" + g_input.substr(n - 4);
}

static void RenderDlg() {
    static Surface s;
    if (!s.px || s.w != kDlgW || s.h != kDlgH) {
        if (!BuildSurface(s, kDlgW, kDlgH)) return;
    }
    std::memset(s.px, 0, (size_t)s.w * s.h * 4);
    DrawPanel(s, kDlgW, kDlgH, true, 244);

    DrawText(s, L"DEEPSEEK API KEY", (float)kPad, 12.0f, kScale, Px(0xF2F2F6, 255));
    if (!g_key.empty()) {
        float w = TextWidth(L"SAVED", 1.0f);
        DrawText(s, L"SAVED", (float)kDlgW - kPad - w, 15.0f, 1.0f, Px(0x4ADE80, 255));
    }

    Fill(s, kFieldX, kFieldY, kFieldW, kFieldH, Px(0x15151C, 255));
    HLine(s, kFieldX, kFieldX + kFieldW, kFieldY, Px(0x33333F, 255));
    HLine(s, kFieldX, kFieldX + kFieldW, kFieldY + kFieldH - 1, Px(0x33333F, 255));
    Fill(s, kFieldX, kFieldY, 1, kFieldH, Px(0x33333F, 255));
    Fill(s, kFieldX + kFieldW - 1, kFieldY, 1, kFieldH, Px(0x33333F, 255));

    std::wstring shown = Masked();
    DrawText(s, shown, (float)(kFieldX + 6), (float)(kFieldY + 7), kScale, Px(0xE8E8EE, 255));

    float caretX = (float)(kFieldX + 6) + TextWidth(shown, kScale) + 2.0f;
    Fill(s, (int)caretX, kFieldY + 6, 1, (int)(kGlyphH + 2), Px(0x60A5FA, 255));

    DrawText(s, L"ENTER SAVE   ESC CANCEL", (float)kPad, 66.0f, 1.0f, Px(0x7A7A86, 255));

    RECT rc; GetWindowRect(g_dlg, &rc);
    Present(s, g_dlg, rc.left, rc.top);
}

static void CloseDlg() {
    if (g_dlg) { DestroyWindow(g_dlg); g_dlg = nullptr; }
}

static void SaveAndClose() {
    std::wstring trimmed = g_input;
    while (!trimmed.empty() && (trimmed.back() == L' ' || trimmed.back() == L'\r' || trimmed.back() == L'\n'))
        trimmed.pop_back();
    size_t b = trimmed.find_first_not_of(L" \t");
    if (b != std::wstring::npos) trimmed = trimmed.substr(b);

    if (!trimmed.empty()) {
        SaveSecret(trimmed);
        g_key = trimmed;
    }
    CloseDlg();
    Refresh();
}

static void PasteClipboard() {
    if (!OpenClipboard(g_dlg)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            for (const wchar_t* q = p; *q && g_input.size() < 256; ++q) {
                if (*q >= 32 && *q < 127) g_input.push_back(*q);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

static LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { CloseDlg(); return 0; }
        if (wp == VK_RETURN) { SaveAndClose(); return 0; }
        if (wp == VK_BACK) {
            if (!g_input.empty()) g_input.pop_back();
            RenderDlg();
            return 0;
        }
        if (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            PasteClipboard();
            RenderDlg();
            return 0;
        }
        return 0;
    case WM_CHAR:
        if (wp >= 32 && wp < 127 && g_input.size() < 256) {
            g_input.push_back((wchar_t)wp);
            RenderDlg();
        }
        return 0;
    case WM_CLOSE:
        CloseDlg();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OpenSettings() {
    if (g_dlg) { SetForegroundWindow(g_dlg); return; }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"AiMeterSettingsWnd";
        RegisterClassExW(&wc);
        registered = true;
    }

    // Start empty: the stored key stays in Credential Manager and is only
    // replaced when a new one is typed and confirmed.
    g_input.clear();

    int x = (GetSystemMetrics(SM_CXSCREEN) - kDlgW) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - kDlgH) / 2;

    g_dlg = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST, L"AiMeterSettingsWnd",
                            L"AiMeter Settings", WS_POPUP, x, y, kDlgW, kDlgH,
                            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_dlg) return;

    RECT rc{x, y, x + kDlgW, y + kDlgH};
    (void)rc;
    ShowWindow(g_dlg, SW_SHOW);
    SetForegroundWindow(g_dlg);
    SetFocus(g_dlg);
    RenderDlg();
}

// ---------------------------------------------------------------- config
static std::wstring Env(const wchar_t* name) {
    wchar_t buf[1024]{};
    DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    return n ? std::wstring(buf, n) : std::wstring();
}

// Portable layout: settings live next to the exe. Falls back to %APPDATA% when
// the program folder is read-only (e.g. unzipped under Program Files).
static std::wstring ConfigPath() {
    wchar_t mod[MAX_PATH]{};
    GetModuleFileNameW(nullptr, mod, MAX_PATH);
    std::wstring dir(mod);
    dir = dir.substr(0, dir.find_last_of(L'\\'));

    std::wstring local = dir + L"\\aimeter.ini";
    HANDLE probe = CreateFileW(local.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (probe != INVALID_HANDLE_VALUE) { CloseHandle(probe); return local; }

    std::wstring appdata = Env(L"APPDATA") + L"\\AiMeter";
    CreateDirectoryW(appdata.c_str(), nullptr);
    return appdata + L"\\aimeter.ini";
}

// Plain Win32 file I/O: libstdc++ wide-char streams cannot open a path that
// contains a non-ASCII user name.
static bool LoadConfig(int& x, int& y) {
    std::wstring path = ConfigPath();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    wchar_t buf[2048]{};
    DWORD rd = 0;
    ReadFile(h, buf, sizeof(buf) - sizeof(wchar_t), &rd, nullptr);
    CloseHandle(h);
    buf[rd / sizeof(wchar_t)] = 0;

    for (wchar_t* p = buf; *p; ) {
        if (wcsncmp(p, L"x=", 2) == 0) x = (int)wcstol(p + 2, nullptr, 10);
        else if (wcsncmp(p, L"y=", 2) == 0) y = (int)wcstol(p + 2, nullptr, 10);
        else if (wcsncmp(p, L"day=", 4) == 0) {
            wchar_t* nl = wcschr(p, L'\n');
            g_day.assign(p + 4, nl ? (size_t)(nl - (p + 4)) : wcslen(p + 4));
        }
        else if (wcsncmp(p, L"start=", 6) == 0) g_dayStart = wcstoll(p + 6, nullptr, 10);
        else if (wcsncmp(p, L"last=", 5) == 0) g_lastCents = wcstoll(p + 5, nullptr, 10);
        wchar_t* nl = wcschr(p, L'\n');
        if (!nl) break;
        p = nl + 1;
    }
    return rd > 0;
}

static void SaveConfig() {
    std::wstring path = ConfigPath();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    wchar_t buf[256]{};
    int n = swprintf(buf, 256, L"x=%d\ny=%d\nday=%ls\nstart=%lld\nlast=%lld\n",
                     g_x, g_y, g_day.c_str(), g_dayStart, g_lastCents);
    DWORD wr = 0;
    if (n > 0) WriteFile(h, buf, (DWORD)n * sizeof(wchar_t), &wr, nullptr);
    CloseHandle(h);
}

static bool AutostartEnabled() {
    HKEY k{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[1024]{};
    DWORD sz = sizeof(buf);
    bool on = RegQueryValueExW(k, L"AiMeter", nullptr, nullptr, (LPBYTE)buf, &sz) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

static void SetAutostart(bool on) {
    HKEY k{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring val = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(k, L"AiMeter", 0, REG_SZ, (const BYTE*)val.c_str(),
                       (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"AiMeter");
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------- tray
#define WM_TRAY (WM_APP + 1)
static NOTIFYICONDATAW g_nid{};
static bool g_trayAdded = false;
static HICON g_icon = nullptr;
static UINT  g_taskbarCreated = 0;

static HICON MakeTrayIcon() {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = 16;
    bi.bV5Height = -16;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC dc = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) { DeleteDC(dc); return nullptr; }

    DWORD* px = (DWORD*)bits;
    for (int i = 0; i < 256; ++i) px[i] = 0;

    const DWORD bars[3] = { 0xFF4ADE80, 0xFFFBBF24, 0xFFF87171 };
    const int heights[3] = { 5, 9, 13 };
    for (int b = 0; b < 3; ++b) {
        int x = 3 + b * 4;
        for (int y = 14 - heights[b]; y <= 14; ++y)
            for (int w = 0; w < 2; ++w) px[y * 16 + x + w] = bars[b];
    }

    BYTE zero[16 * 2]{};
    HBITMAP mask = CreateBitmap(16, 16, 1, 1, zero);
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON ic = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    DeleteDC(dc);
    return ic;
}

static void TrayAdd(HWND h) {
    if (!g_icon) g_icon = MakeTrayIcon();
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = h;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icon ? g_icon : LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy(g_nid.szTip, L"AiMeter", 127);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

static void TrayRemove(HWND h) {
    (void)h;
    if (g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
}

static void TrayMenu(HWND h) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 10, L"Refresh");
    AppendMenuW(m, MF_STRING, 11, L"Settings...");
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), 12, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 13, L"Exit");

    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(h);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
    DestroyMenu(m);
    PostMessageW(h, WM_NULL, 0, 0);

    if (cmd == 10) Refresh();
    if (cmd == 11) OpenSettings();
    if (cmd == 12) SetAutostart(!AutostartEnabled());
    if (cmd == 13) PostMessageW(h, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------- z-order
// Keep the widget pinned directly above the desktop: never above any app window.
// Re-applied whenever another window takes the foreground.
static void PinToDesktop() {
    if (!g_hwnd) return;
    SetWindowPos(g_hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static HHOOK g_hookUnused = nullptr;
static HWINEVENTHOOK g_fgHook = nullptr;

static void CALLBACK ForegroundProc(HWINEVENTHOOK, DWORD ev, HWND hwnd,
                                    LONG, LONG, DWORD, DWORD) {
    (void)g_hookUnused;
    if (ev == EVENT_SYSTEM_FOREGROUND && g_hwnd && hwnd != g_hwnd) PinToDesktop();
}

// ---------------------------------------------------------------- main window
static void ToggleEdit() {
    g_edit = !g_edit;
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    if (g_edit) ex &= ~WS_EX_TRANSPARENT;
    else        ex |= WS_EX_TRANSPARENT;
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RenderMain();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbarCreated && msg == g_taskbarCreated) { TrayAdd(hwnd); return 0; }

    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP) TrayMenu(hwnd);
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK) ToggleEdit();
        return 0;
    case WM_EXITSIZEMOVE: {
        RECT r; GetWindowRect(hwnd, &r);
        g_x = r.left; g_y = r.top;
        SaveConfig();
        Present(g_main, g_hwnd, g_x, g_y);
        PinToDesktop();
        return 0;
    }
    case WM_CLOSE:
        TrayRemove(hwnd);
        SaveConfig();
        DestroyWindow(hwnd);
        return 0;
    case WM_HOTKEY:
        if (wp == 1) ToggleEdit();
        if (wp == 2) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_LBUTTONDOWN:
        if (g_edit) {
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            RECT r; GetWindowRect(hwnd, &r);
            g_x = r.left; g_y = r.top;
        }
        return 0;
    case WM_RBUTTONUP:
        if (g_edit) {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"Refresh");
            AppendMenuW(m, MF_STRING, 3, L"Settings...");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 2, L"Exit");
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(m);
            if (cmd == 1) Refresh();
            if (cmd == 3) OpenSettings();
            if (cmd == 2) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_TIMER:
        if (wp == 1) Refresh();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    SetLastError(0);
    HANDLE once = CreateMutexW(nullptr, TRUE, L"AiMeterSingleInstance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(once); return 0; }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_key = LoadSecret();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AiMeterWnd";
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    g_x = sw - kWidth - 40;
    g_y = sh - 60 - 80;
    if (!LoadConfig(g_x, g_y)) {
        SetAutostart(true);   // first run: start with Windows by default
    }
    if (g_x < 0 || g_x > sw - 60 || g_y < 0 || g_y > sh - 40) {
        g_x = sw - kWidth - 40;
        g_y = sh - 60 - 80;
    }

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        L"AiMeterWnd", L"AiMeter", WS_POPUP,
        g_x, g_y, kWidth, 60, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    TrayAdd(g_hwnd);

    PinToDesktop();
    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                               nullptr, ForegroundProc, 0, 0, WINEVENT_OUTOFCONTEXT);

    RegisterHotKey(g_hwnd, 1, MOD_CONTROL | MOD_ALT, 'W');
    RegisterHotKey(g_hwnd, 2, MOD_CONTROL | MOD_ALT, 'Q');
    SetTimer(g_hwnd, 1, 5 * 60 * 1000, nullptr);

    Refresh();

    if (wcsstr(GetCommandLineW(), L"--settings")) OpenSettings();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_fgHook) UnhookWinEvent(g_fgHook);
    return 0;
}
