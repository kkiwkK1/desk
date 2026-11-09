// Headless contract test for the CARDSPEC card (M2.0 UI lane).
//
// No Engine, no DSH/PI, no network and no real board: one private synthetic
// config directory under the test exe, a wg::Store bound exactly the way the
// board binds one, and a wg::Canvas painted without a window. The card is
// driven through the public factory, so ModuleT's Load/Refresh/Commit staging
// split is exercised for real.
//
// Covers: factory Describe defaults (fixed id/title/preset/hidden/no timer),
// missing -> rows -> bar -> invalid -> fix recovery, oversized file, shown vs
// pending heights, CJK ink, strict value/max bars, eliding and safe clipping,
// and MeasureCardSize accepting every 0..8 row / error height.
#include "bmp.h"

#include "data/cardspec/cardspec.h"
#include "panels/cardspec/cardspec.h"
#include "panels/cardspec/cardspec_view.h"

#include "widget/canvas.h"
#include "widget/module.h"
#include "widget/size.h"
#include "widget/store.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wprintf(L"[cardspec-render-test] FAIL %ls\n", message);
        ++failures;
    }
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0) return std::wstring(L".");
    const std::wstring text(path, n);
    const std::size_t slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : text.substr(0, slash);
}

void RemoveTree(const std::wstring& path) {
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring child = path + L"\\" + name;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                RemoveTree(child);
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    RemoveDirectoryW(path.c_str());
}

bool WriteBytes(const std::wstring& path, const std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = true;
    if (!bytes.empty()) {
        ok = WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, nullptr) &&
             written == bytes.size();
    }
    CloseHandle(file);
    return ok;
}

std::string ReadBytes(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::string();
    std::string bytes;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(file, buf, sizeof(buf), &got, nullptr) && got > 0) {
        bytes.append(buf, got);
    }
    CloseHandle(file);
    return bytes;
}

bool PixelIs(const wg::Canvas& canvas, int x, int y, wg::Color color) {
    if (x < 0 || y < 0 || x >= canvas.width() || y >= canvas.height()) return false;
    return canvas.data()[(std::size_t)y * canvas.width() + x] == color;
}

bool HasInk(const wg::Canvas& canvas, int x, int y, int w, int h, wg::Color background) {
    for (int yy = y; yy < y + h; ++yy) {
        if (yy < 0 || yy >= canvas.height()) continue;
        const wg::Color* row =
            canvas.data() + (std::size_t)yy * (std::size_t)canvas.width();
        for (int xx = x; xx < x + w; ++xx) {
            if (xx < 0 || xx >= canvas.width()) continue;
            if (row[(std::size_t)xx] != background) return true;
        }
    }
    return false;
}

// True when any pixel outside `area` differs from the background: the proof
// that a module painted only inside the body rectangle it was handed.
bool HasInkOutside(const wg::Canvas& canvas, const wg::Rect& area, wg::Color background) {
    for (int yy = 0; yy < canvas.height(); ++yy) {
        const wg::Color* row =
            canvas.data() + (std::size_t)yy * (std::size_t)canvas.width();
        for (int xx = 0; xx < canvas.width(); ++xx) {
            if (xx >= area.x && xx < area.right() && yy >= area.y && yy < area.bottom()) {
                continue;
            }
            if (row[(std::size_t)xx] != background) return true;
        }
    }
    return false;
}

std::vector<wg::Color> Snapshot(const wg::Canvas& canvas) {
    const std::size_t count =
        (std::size_t)canvas.width() * (std::size_t)canvas.height();
    return std::vector<wg::Color>(canvas.data(), canvas.data() + count);
}

// Paints the module's shown state into a body of the card's own width.
int PaintCard(wg::Module& module, wg::Canvas& canvas, int width, wg::Color background) {
    const int height = std::max(1, module.BodyHeight());
    if (!canvas.Resize(width, height)) return 0;
    canvas.Clear(background);
    module.PaintBody(canvas, wg::Rect{0, 0, width, height});
    return height;
}

// Synthetic fixtures. The JSON bytes are encoded at runtime from wide literals,
// so the test does not depend on the compiler's narrow execution charset.
std::string Utf8(std::wstring_view text) {
    if (text.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out((std::size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), need,
                        nullptr, nullptr);
    return out;
}

std::string JsonString(const wchar_t* text) { return "\"" + Utf8(text) + "\""; }

std::string RowsJson() {
    return "{\"version\":1,\"title\":" + JsonString(L"今日目标") +
           ",\"source\":{\"type\":\"inline\"},\"layout\":\"rows\",\"rows\":[" +
           "{\"label\":" + JsonString(L"学习") + ",\"text\":" +
           JsonString(L"阅读三十分钟") + "}," +
           "{\"label\":" + JsonString(L"运动") + ",\"text\":" +
           JsonString(L"跑步五公里") + "}]}";
}

std::string BarJson() {
    return "{\"version\":1,\"title\":" + JsonString(L"目标进度") +
           ",\"source\":{\"type\":\"inline\"},\"layout\":\"bar\",\"rows\":[" +
           "{\"label\":" + JsonString(L"学习") + ",\"value\":3,\"max\":5}," +
           "{\"label\":" + JsonString(L"空闲") + ",\"value\":0,\"max\":5}]}";
}

// Truncated on purpose: rejected by the parser, and the card must not echo it.
std::string BadJson() { return "{\"version\":1,\"title\":" + JsonString(L"坏") + ","; }

cardspec_view::ViewState MakeRowsView(int rows) {
    cardspec_view::ViewState state;
    state.state = cardspec_view::State::Ok;
    state.spec.title = L"布局";
    state.spec.layout = cardspec::Layout::Rows;
    state.spec.rows.resize((std::size_t)rows);
    for (int i = 0; i < rows; ++i) {
        state.spec.rows[(std::size_t)i].label = L"标签";
        state.spec.rows[(std::size_t)i].text = L"内容";
    }
    return state;
}

} // namespace

int wmain() {
    using namespace wg;

    const Theme theme;
    const Color background = theme.panelSolid;
    const int width = cardspec_view::kCardWidth;
    const int left = cardspec_view::kPadX;
    const int right = left + cardspec_view::kTextWidth;

    const std::wstring root = ExeDir() + L"\\cardspec-render-test";
    const std::wstring configPath = root + L"\\cardspec.ini";
    const std::wstring specPath = root + L"\\cardspec.json";

    RemoveTree(root);
    if (!CreateDirectoryW(root.c_str(), nullptr)) {
        std::wprintf(L"[cardspec-render-test] FAIL cannot create %ls\n", root.c_str());
        return 1;
    }

    Store store;
    store.Open(configPath);  // absent file: Open still records the path
    int refreshes = 0;

    std::unique_ptr<Module> module = CreateCardSpecModule();
    Check(module != nullptr, L"the factory returns a module");
    if (!module) {
        RemoveTree(root);
        return 1;
    }
    module->Bind(&store, [] {});
    module->BindRefresh([&refreshes] { ++refreshes; });

    // ---- frozen Describe contract ------------------------------------------
    const ModuleSpec desc = module->Describe();
    Check(desc.id == L"cardspec", L"fixed module id is cardspec");
    Check(desc.title == L"CARDSPEC", L"fixed host title is CARDSPEC");
    Check(desc.configFile == L"cardspec.ini", L"host state lives in cardspec.ini");
    Check(desc.sizePreset == SizePreset::Wide, L"fixed Wide preset (568 px)");
    Check(SizePresetRule(desc.sizePreset).width == width, L"Wide is 568 px wide");
    Check(desc.refreshSeconds == 0, L"manual refresh only (no timer)");
    Check(!desc.hasDetails, L"no details block");
    Check(!desc.visibleByDefault, L"hidden until the user adds the card");
    Check(desc.hotkeys.empty(), L"no hotkeys");

    // The spec is the fixed sibling of the Store file, never a user path.
    Check(cardspec_view::SpecPathFor(configPath) == specPath,
          L"the spec path is the config directory's cardspec.json");
    Check(cardspec_view::SpecPathFor(L"cardspec.ini").empty(),
          L"a bare file name has no fixed directory");
    Check(cardspec_view::SpecPathFor(L"").empty(),
          L"no bound config means no spec path");
    Check(cardspec_view::SpecPathFor(L"C:\\cfg\\") == L"C:\\cfg\\cardspec.json",
          L"a trailing separator is normalized");

    // ---- missing file -----------------------------------------------------
    module->Load();
    Sleep(80);
    Check(refreshes == 0, L"Load never schedules a background refresh");
    Check(module->BodyHeight() > 0 && module->PendingBodyHeight() == module->BodyHeight(),
          L"the missing card reports its own content height");
    Check(module->Status().text == L"NO SPEC", L"missing file reports NO SPEC");

    const cardspec_view::ViewState missing = cardspec_view::LoadSpecFile(specPath);
    Check(missing.state == cardspec_view::State::Missing, L"missing file state");
    Check(missing.message.find(L"未找到") != std::wstring::npos,
          L"missing file has a readable Chinese message");
    Check(missing.message.find(root) == std::wstring::npos,
          L"the missing message hides the absolute directory");

    wg::Canvas missingCanvas;
    const int missingH = PaintCard(*module, missingCanvas, width, background);
    Check(missingH == cardspec_view::BodyHeight(missing), L"missing body height matches");
    Check(HasInk(missingCanvas, left, 0, 300, cardspec_view::kLineH, background),
          L"the missing card paints its heading");
    Check(HasInk(missingCanvas, left, 20, 500, cardspec_view::kLineH, background),
          L"the missing card paints its message");
    Check(GetFileAttributesW(specPath.c_str()) == INVALID_FILE_ATTRIBUTES,
          L"a missing spec is reported, never generated");

    // ---- valid rows: staging vs shown -------------------------------------
    Check(WriteBytes(specPath, RowsJson()), L"write the rows fixture");
    const std::vector<wg::Color> missingPixels = Snapshot(missingCanvas);
    module->Refresh();
    Check(module->BodyHeight() == missingH,
          L"shown height is unchanged before Commit");
    Check(module->PendingBodyHeight() ==
              cardspec_view::kTitleBlock + 2 * cardspec_view::kRowPitch,
          L"PendingBodyHeight follows the staged rows layout");

    wg::Canvas pendingCanvas;
    const int pendingH = PaintCard(*module, pendingCanvas, width, background);
    Check(pendingH == missingH, L"PaintBody still measures the shown state");
    Check(Snapshot(pendingCanvas) == missingPixels,
          L"PaintBody still paints shown while staging is pending");

    module->Commit();
    const int rowsH = cardspec_view::kTitleBlock + 2 * cardspec_view::kRowPitch;
    Check(module->BodyHeight() == rowsH, L"Commit publishes the rows card");
    // PendingBodyHeight is only part of the Refresh -> Commit window: after the
    // swap, staging holds the *previous* shown (the contract the board relies
    // on). Prove a fresh cycle instead of expecting pending == shown after
    // Commit: Refresh stages the rows layout again, shown stays put until the
    // next Commit publishes it.
    module->Refresh();
    Check(module->BodyHeight() == rowsH, L"Refresh leaves the shown rows card alone");
    Check(module->PendingBodyHeight() == rowsH,
          L"a new cycle stages the rows layout before its Commit");
    module->Commit();
    Check(module->BodyHeight() == rowsH, L"the second Commit keeps the rows card");
    Check(module->Status().text == L"LIVE" && module->Status().color == theme.ok,
          L"a valid spec reports LIVE");

    wg::Canvas rowsCanvas;
    const int paintedRowsH = PaintCard(*module, rowsCanvas, width, background);
    Check(paintedRowsH == rowsH, L"rows body height fits the content");
    Check(HasInk(rowsCanvas, left, 0, width - 2 * left, cardspec_view::kLineH, background),
          L"the spec title is painted inside the body");
    Check(HasInk(rowsCanvas, left, cardspec_view::kTitleBlock, 300,
                 cardspec_view::kLineH, background),
          L"the row label is painted");
    Check(HasInk(rowsCanvas, left, cardspec_view::kTitleBlock + 18, 300,
                 cardspec_view::kLineH, background),
          L"Chinese row text is painted (non-empty pixels)");

    // ---- valid bar: strict value/max --------------------------------------
    Check(WriteBytes(specPath, BarJson()), L"write the bar fixture");
    module->Refresh();
    module->Commit();
    const int barH = cardspec_view::kTitleBlock + 2 * cardspec_view::kRowPitch;
    Check(module->BodyHeight() == barH, L"the bar card uses the same row pitch");

    wg::Canvas barCanvas;
    PaintCard(*module, barCanvas, width, background);
    const int barTop0 = cardspec_view::kTitleBlock + cardspec_view::kBarOffsetY;
    const int barTop1 = barTop0 + cardspec_view::kRowPitch;
    Check(PixelIs(barCanvas, left + 100, barTop0 + 2, theme.ok),
          L"3/5 fills the left 60% of the bar");
    Check(PixelIs(barCanvas, left + 500, barTop0 + 2, theme.track),
          L"3/5 leaves the right 40% as track");
    Check(PixelIs(barCanvas, left + 2, barTop1 + 2, theme.track),
          L"0/5 paints only the track");
    Check(!PixelIs(barCanvas, left + 2, barTop1 + 2, theme.ok),
          L"0/5 paints no fill");
    Check(HasInk(barCanvas, right - 60, cardspec_view::kTitleBlock, 60,
                 cardspec_view::kLineH, background),
          L"the bar value is right-aligned inside the card");

    // ---- invalid JSON: error card, no stale data, no echo -----------------
    Check(WriteBytes(specPath, BadJson()), L"write the invalid fixture");
    module->Refresh();
    Check(module->BodyHeight() == barH, L"a failed refresh does not disturb shown");
    const int errorH = module->PendingBodyHeight();
    Check(errorH < barH, L"the pending error card is shorter than the bar card");
    module->Commit();
    Check(module->BodyHeight() == errorH, L"Commit publishes the error card");
    Check(module->Status().text == L"INVALID", L"invalid JSON reports INVALID");

    const cardspec_view::ViewState bad = cardspec_view::LoadSpecFile(specPath);
    Check(bad.state == cardspec_view::State::Invalid, L"invalid file state");
    Check(bad.message.find(L"坏") == std::wstring::npos &&
              bad.detail.find(L"坏") == std::wstring::npos,
          L"the invalid state never echoes the payload");
    Check(bad.message.find(root) == std::wstring::npos &&
              bad.detail.find(root) == std::wstring::npos,
          L"the invalid state never echoes the absolute path");
    Check(errorH == cardspec_view::BodyHeight(bad),
          L"the module's error height matches the loader state");

    wg::Canvas errorCanvas;
    const int paintedErrorH = PaintCard(*module, errorCanvas, width, background);
    Check(paintedErrorH == errorH, L"the error card height is its content height");
    Check(HasInk(errorCanvas, left, 0, 300, cardspec_view::kLineH, background),
          L"the error card paints a heading");
    Check(HasInk(errorCanvas, left, 20, 500, cardspec_view::kLineH, background),
          L"the error card paints a readable message");
    Check(!PixelIs(errorCanvas, left + 100, barTop0 + 2, theme.ok),
          L"the old bar fill is gone (no fake stale success)");
    Check(!PixelIs(errorCanvas, left + 500, barTop0 + 2, theme.track),
          L"the old bar track is gone too");

    // ---- fixing the file + Refresh recovers -------------------------------
    Check(WriteBytes(specPath, BarJson()), L"rewrite the bar fixture");
    module->Refresh();
    module->Commit();
    Check(module->BodyHeight() == barH, L"a fixed file restores the bar card");
    Check(module->Status().text == L"LIVE", L"recovery returns to LIVE");
    wg::Canvas fixedCanvas;
    PaintCard(*module, fixedCanvas, width, background);
    Check(PixelIs(fixedCanvas, left + 100, barTop0 + 2, theme.ok),
          L"recovery paints the bar fill again");

    // ---- oversized file ---------------------------------------------------
    Check(WriteBytes(specPath, std::string(cardspec::kMaxSpecBytes + 1, 'x')),
          L"write an oversized fixture");
    const cardspec_view::ViewState big = cardspec_view::LoadSpecFile(specPath);
    Check(big.state == cardspec_view::State::TooLarge, L"oversized file state");
    Check(big.message.find(L"32") != std::wstring::npos,
          L"the oversized message names the 32 KiB limit");
    module->Refresh();
    module->Commit();
    Check(module->Status().text == L"TOO BIG", L"oversized file reports TOO BIG");
    Check(module->BodyHeight() == cardspec_view::BodyHeight(big),
          L"the oversized card uses its own content height");
    wg::Canvas bigCanvas;
    PaintCard(*module, bigCanvas, width, background);
    Check(!PixelIs(bigCanvas, left + 100, barTop0 + 2, theme.ok),
          L"no stale bar after the oversized file");

    // ---- removed file returns to the missing state ------------------------
    Check(DeleteFileW(specPath.c_str()) != 0, L"delete the spec fixture");
    Check(cardspec_view::LoadSpecFile(specPath).state == cardspec_view::State::Missing,
          L"a deleted file reports missing again");
    module->Refresh();
    module->Commit();
    Check(module->Status().text == L"NO SPEC", L"the removed file reports NO SPEC");

    // ---- a directory where the spec should be -----------------------------
    // Synthetic (no privilege needed): a leaf directory has to fail closed with
    // a specific message instead of a generic open error.
    {
        Check(CreateDirectoryW(specPath.c_str(), nullptr) != 0,
              L"create a directory at the spec path");
        const cardspec_view::ViewState dir = cardspec_view::LoadSpecFile(specPath);
        Check(dir.state == cardspec_view::State::Invalid, L"a directory is invalid");
        Check(dir.message.find(L"目录") != std::wstring::npos,
              L"the directory error is specific and readable");
        Check(dir.message.find(root) == std::wstring::npos,
              L"the directory error hides the absolute path");
        module->Refresh();
        module->Commit();
        Check(module->Status().text == L"INVALID", L"a directory reports INVALID");
        Check(RemoveDirectoryW(specPath.c_str()) != 0, L"remove the directory fixture");
    }

    // ---- manual Reload menu: request only, no IO --------------------------
    // The board owns the refresh that follows a menu action (OnMenu ->
    // StartRefresh), so the callback itself must ask for nothing: 0 module
    // requests here, and the host-side refresh is the single worker.
    const std::vector<MenuItem> menu = module->Menu();
    Check(menu.size() == 1, L"exactly one manual reload entry");
    Check(!menu.empty() && menu[0].label.find(L"Reload") == 0,
          L"the entry is a reload action");
    const int beforeMenu = refreshes;
    const int heightBeforeMenu = module->BodyHeight();
    if (!menu.empty()) module->OnMenu(menu[0].id);
    Check(refreshes == beforeMenu,
          L"Reload leaves the single refresh to the host menu dispatch");
    Check(module->BodyHeight() == heightBeforeMenu,
          L"the menu entry performs no IO and no re-load");
    module->OnMenu(-7);
    Check(refreshes == beforeMenu, L"unknown menu ids do nothing");
    // The host's own refresh after the menu action is what reloads the card;
    // proving it here keeps "no IO in the callback" from hiding a dead reload.
    module->Refresh();
    module->Commit();
    Check(refreshes == beforeMenu, L"the host refresh needs no module request");

    // ---- eliding and safe clipping ---------------------------------------
    {
        cardspec_view::ViewState longState;
        longState.state = cardspec_view::State::Ok;
        longState.spec.title = L"长文本";
        longState.spec.layout = cardspec::Layout::Rows;
        cardspec::Row longRow;
        longRow.label = L"测试";
        longRow.text = std::wstring(96, L'W');
        longState.spec.rows.push_back(std::move(longRow));

        const int bodyH = cardspec_view::BodyHeight(longState);
        wg::Canvas canvas;
        Check(canvas.Resize(width, bodyH), L"long-text canvas");
        canvas.Clear(background);
        cardspec_view::DrawBody(longState, canvas, Rect{0, 0, width, bodyH}, theme);
        const int textTop = cardspec_view::kTitleBlock + 18;
        Check(HasInk(canvas, left, textTop, 400, cardspec_view::kLineH, background),
              L"over-long legal text is painted (elided, not dropped)");
        Check(!HasInk(canvas, right + 2, textTop, width - (right + 2),
                      cardspec_view::kLineH, background),
              L"over-long text never past the inner right edge");
    }
    {
        // A body the host shrank: only whole line boxes may be painted, and
        // nothing may land outside the given rectangle.
        cardspec_view::ViewState state = MakeRowsView(8);
        state.spec.rows[0].text = std::wstring(96, L'W');
        wg::Canvas canvas;
        Check(canvas.Resize(120, 120), L"clip canvas");
        canvas.Clear(background);
        const Rect small{20, 30, 60, 40};
        cardspec_view::DrawBody(state, canvas, small, theme);
        Check(HasInk(canvas, small.x, small.y, small.w, small.h, background),
              L"a small body still paints what fits");
        Check(!HasInkOutside(canvas, small, background),
              L"painting stays inside the given body rectangle");

        // Bar layout in the same shrunken body.
        cardspec_view::ViewState bars = MakeRowsView(4);
        bars.spec.layout = cardspec::Layout::Bar;
        for (cardspec::Row& row : bars.spec.rows) {
            row.value = 1.0;
            row.max = 4.0;
        }
        canvas.Clear(background);
        cardspec_view::DrawBody(bars, canvas, small, theme);
        Check(!HasInkOutside(canvas, small, background),
              L"bar painting stays inside the given body rectangle");

        // Degenerate body width: nothing at all.
        canvas.Clear(background);
        cardspec_view::DrawBody(state, canvas, Rect{5, 5, 0, 40}, theme);
        Check(!HasInkOutside(canvas, Rect{5, 5, 0, 40}, background),
              L"a zero-width body paints nothing");
    }
    {
        // Extremely narrow bodies: the ellipsis can itself be wider than the
        // room, so FitTo must fall back to fewer dots (or nothing) instead of
        // painting a marker that overflows. Nothing may land outside the body.
        cardspec_view::ViewState rows = MakeRowsView(3);
        rows.spec.title = L"很长的中文标题";
        cardspec_view::ViewState bars = MakeRowsView(3);
        bars.spec.layout = cardspec::Layout::Bar;
        for (cardspec::Row& row : bars.spec.rows) {
            row.value = 2.0;
            row.max = 5.0;
        }
        cardspec_view::ViewState invalid;
        invalid.state = cardspec_view::State::Invalid;
        invalid.message = L"cardspec.json 不符合 v1 规格";
        invalid.detail = L"root.rows[0].label：至少 1 个字符";

        for (const int bodyWidth : {1, 12, 24, 26, 40}) {
            const Rect body{20, 30, bodyWidth, 60};
            for (const cardspec_view::ViewState* state : {&rows, &bars, &invalid}) {
                wg::Canvas canvas;
                Check(canvas.Resize(120, 120), L"tiny-body canvas");
                canvas.Clear(background);
                cardspec_view::DrawBody(*state, canvas, body, theme);
                Check(!HasInkOutside(canvas, body, background),
                      L"a tiny body never paints outside its rectangle");
            }
            if (bodyWidth == 40) {
                wg::Canvas canvas;
                Check(canvas.Resize(120, 120), L"tiny-body ink canvas");
                canvas.Clear(background);
                cardspec_view::DrawBody(rows, canvas, body, theme);
                Check(HasInk(canvas, body.x, body.y, body.w, body.h, background),
                      L"a 40px body still paints an elided marker inside");
            }
        }
    }

    // ---- MeasureCardSize accepts every content height --------------------
    {
        Check(kCardChromeHeight == 48, L"the host adds 48 px of card chrome");
        for (int rows = 0; rows <= 8; ++rows) {
            const cardspec_view::ViewState state = MakeRowsView(rows);
            const int body = cardspec_view::BodyHeight(state);
            const CardSize size = MeasureCardSize(SizePreset::Wide, 0, body);
            Check(size.representable && size.withinPreset && size.width == width,
                  L"every rows height fits the Wide preset");
            const int expected =
                ((std::max(SizePresetRule(SizePreset::Wide).minHeight, body + 48) + 7) / 8) * 8;
            Check(size.height == expected, L"host height is content + 48 rounded to 8");
        }
        cardspec_view::ViewState missingState;
        missingState.state = cardspec_view::State::Missing;
        missingState.message = L"未找到配置目录内的 cardspec.json";
        cardspec_view::ViewState invalidState;
        invalidState.state = cardspec_view::State::Invalid;
        invalidState.message = L"cardspec.json 不符合 v1 规格";
        invalidState.detail = L"rows[0].text";
        for (const cardspec_view::ViewState& state : {missingState, invalidState}) {
            const int body = cardspec_view::BodyHeight(state);
            const CardSize size = MeasureCardSize(SizePreset::Wide, 0, body);
            Check(size.representable && size.withinPreset && size.width == width,
                  L"error cards fit the Wide preset");
        }
    }

    // ---- a BOM is handed to the parser untouched --------------------------
    {
        const std::string withBom = std::string("\xEF\xBB\xBF") + RowsJson();
        Check(WriteBytes(specPath, withBom), L"write a BOM-prefixed fixture");
        module->Refresh();
        module->Commit();
        Check(module->Status().text == L"LIVE", L"a UTF-8 BOM is accepted by the loader");
        Check(module->BodyHeight() == rowsH, L"a BOM does not change the rows layout");
    }

    // ---- the card never writes host or user files -------------------------
    Check(WriteBytes(specPath, RowsJson()), L"restore the rows fixture");
    module->Refresh();
    module->Commit();
    const std::string specBefore = ReadBytes(specPath);
    module->Refresh();
    module->Commit();
    Check(ReadBytes(specPath) == specBefore,
          L"refresh never rewrites the user spec");
    Check(GetFileAttributesW(configPath.c_str()) == INVALID_FILE_ATTRIBUTES,
          L"the module never creates or writes cardspec.ini itself");

    // ---- optional preview for a human reviewer ---------------------------
    {
        const cardspec_view::ViewState rowsView = MakeRowsView(2);
        cardspec_view::ViewState barView = MakeRowsView(2);
        barView.spec.layout = cardspec::Layout::Bar;
        barView.spec.rows[0].value = 3.0;
        barView.spec.rows[1].value = 0.0;
        for (cardspec::Row& row : barView.spec.rows) row.max = 5.0;
        const int h1 = cardspec_view::BodyHeight(rowsView);
        const int h2 = cardspec_view::BodyHeight(barView);
        wg::Canvas preview;
        if (preview.Resize(width + 32, h1 + h2 + 32)) {
            preview.Clear(background);
            cardspec_view::DrawBody(rowsView, preview, Rect{16, 8, width, h1}, theme);
            cardspec_view::DrawBody(barView, preview, Rect{16, 8 + h1 + 16, width, h2},
                                    theme);
            const std::wstring path = ExeDir() + L"\\cardspec-preview.bmp";
            if (test::WriteBmp(path.c_str(), preview, Rgba(0x101820))) {
                std::wprintf(L"[cardspec-render-test] wrote %ls (%dx%d)\n",
                             path.c_str(), preview.width(), preview.height());
            }
        }
    }

    RemoveTree(root);

    if (failures) {
        std::wprintf(L"[cardspec-render-test] %d failure(s)\n", failures);
        return 1;
    }
    std::wprintf(L"[cardspec-render-test] ok\n");
    return 0;
}
