// Headless checks for the terminal card: byte decoding, the grid, the VT
// parser, the key encoder, selection - and one real ConPTY round trip so the
// pseudo console is not first exercised by the card itself.
#include "data/ps/keymap.h"
#include "data/ps/pi_process.h"
#include "data/ps/screen.h"
#include "data/ps/selection.h"
#include "data/ps/terminal.h"
#include "data/ps/text.h"
#include "data/ps/vt.h"

#include "widget/store.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const wchar_t* what) {
    if (!ok) {
        wprintf(L"[ps-test] FAIL %ls\n", what);
        ++g_failures;
    }
}

std::wstring RowText(const ps::Screen& screen, int row) {
    const std::vector<ps::Row> rows = screen.visibleRows();
    if (row < 0 || row >= (int)rows.size()) return std::wstring();
    std::wstring out;
    for (const ps::Cell& cell : rows[(std::size_t)row].cells) {
        if (cell.wideTail) continue;
        out.push_back(cell.ch == 0 ? L' ' : cell.ch);
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

ps::Cell CellAt(const ps::Screen& screen, int row, int col) {
    const std::vector<ps::Row> rows = screen.visibleRows();
    if (row < 0 || row >= (int)rows.size()) return ps::Cell{};
    if (col < 0 || col >= (int)rows[(std::size_t)row].cells.size()) return ps::Cell{};
    return rows[(std::size_t)row].cells[(std::size_t)col];
}


void TestDecoding() {
    Check(ps::DecodeChunk("ascii") == L"ascii", L"decode ascii");
    Check(ps::DecodeChunk("").empty(), L"decode empty");

    const std::string utf8 = "\xE4\xB8\xAD\xE6\x96\x87";
    const std::wstring expected = L"\u4E2D\u6587";
    Check(ps::DecodeChunk(utf8) == expected, L"decode utf-8");
    if (GetACP() == 936) {
        Check(ps::DecodeChunk("\xD6\xD0\xCE\xC4") == expected, L"decode gbk fallback");
    } else {
        wprintf(L"[ps-test] skip gbk fallback (acp %u)\n", GetACP());
    }
    Check(ps::EncodeUtf8(expected) == utf8, L"encode utf-8");
}

void TestTextAndWrap() {
    ps::Screen screen(10, 3);
    ps::Parser parser;
    parser.Feed(L"hello", screen);
    Check(RowText(screen, 0) == L"hello", L"print text");
    Check(screen.cursorCol() == 5 && screen.cursorRow() == 0, L"cursor follows text");

    parser.Feed(L"\r\nworld", screen);
    Check(RowText(screen, 1) == L"world", L"cr lf");

    ps::Screen wrap(4, 3);
    ps::Parser wp;
    wp.Feed(L"abcdef", wrap);
    Check(RowText(wrap, 0) == L"abcd", L"autowrap fills the row");
    Check(RowText(wrap, 1) == L"ef", L"autowrap continues on the next row");

    ps::Screen wide(6, 2);
    ps::Parser wd;
    wd.Feed(L"\u4E2D\u6587", wide);
    Check(RowText(wide, 0) == L"\u4E2D\u6587", L"wide chars occupy two cells");
    Check(CellAt(wide, 0, 1).wideTail, L"wide char has a tail cell");
    Check(wide.cursorCol() == 4, L"wide chars advance two columns");

    ps::Screen edge(3, 2);
    ps::Parser ep;
    ep.Feed(L"ab\u4E2D", edge);
    Check(RowText(edge, 0) == L"ab", L"wide char wraps at the edge");
    Check(RowText(edge, 1) == L"\u4E2D", L"wide char lands on the next row");
}

void TestCsi() {
    ps::Screen screen(8, 4);
    ps::Parser p;

    p.Feed(L"\x1b[2;3Hxy", screen);
    Check(RowText(screen, 1) == L"  xy", L"cup positions the cursor");

    p.Feed(L"\x1b[2J\x1b[1;1HAB\x1b[K", screen);
    Check(RowText(screen, 0) == L"AB", L"erase in line");
    Check(RowText(screen, 3).empty(), L"erase display clears rows");

    ps::Screen lines(6, 4);
    ps::Parser lp;
    lp.Feed(L"a\r\nb\r\nc\r\nd\x1b[1;1H\x1b[2M", lines);
    Check(RowText(lines, 0) == L"c", L"delete lines shifts up");
    Check(RowText(lines, 1) == L"d", L"delete lines keeps the next row");
    Check(RowText(lines, 2).empty(), L"delete lines blanks the tail");

    ps::Screen chars(6, 2);
    ps::Parser cp;
    cp.Feed(L"abc\x1b[1;2H\x1b[1P", chars);
    Check(RowText(chars, 0) == L"ac", L"delete chars");
    cp.Feed(L"\x1b[1;1H\x1b[1@", chars);
    Check(RowText(chars, 0) == L" ac", L"insert chars");

    ps::Screen region(4, 4);
    ps::Parser rp;
    rp.Feed(L"1\r\n2\r\n3\r\n4", region);
    rp.Feed(L"\x1b[2;3r\x1b[3;1H\n", region);
    Check(RowText(region, 0) == L"1", L"scroll region keeps rows above");
    Check(RowText(region, 1) == L"3", L"scroll region scrolled its top");
    Check(RowText(region, 3) == L"4", L"scroll region keeps rows below");

    ps::Screen alt(6, 2);
    ps::Parser ap;
    ap.Feed(L"main", alt);
    ap.Feed(L"\x1b[?1049h", alt);
    Check(alt.alternate(), L"alternate screen entered");
    Check(RowText(alt, 0).empty(), L"alternate screen starts blank");
    ap.Feed(L"alt", alt);
    ap.Feed(L"\x1b[?1049l", alt);
    Check(!alt.alternate(), L"alternate screen left");
    Check(RowText(alt, 0) == L"main", L"main screen restored");

    ps::Screen modes(6, 2);
    ps::Parser mp;
    mp.Feed(L"\x1b[?25l", modes);
    Check(!modes.cursorVisible(), L"decset 25 hides the cursor");
    mp.Feed(L"\x1b[?25h", modes);
    Check(modes.cursorVisible(), L"decset 25 shows the cursor");
    mp.Feed(L"\x1b[?1h", modes);
    Check(modes.applicationCursorKeys(), L"decset 1 application cursor");
    mp.Feed(L"\x1b[?2004h", modes);
    Check(modes.bracketedPaste(), L"decset 2004 bracketed paste");

    ps::Screen junk(6, 2);
    ps::Parser jp;
    jp.Feed(L"\x1b[999;999;99z\x1b]0;title\x07ok\x1bPq#0\x1b\\!", junk);
    Check(RowText(junk, 0) == L"ok!", L"unknown sequences are swallowed");
}

void TestSgr() {
    ps::Screen screen(10, 2);
    ps::Parser p;

    p.Feed(L"\x1b[31mR\x1b[0mN", screen);
    Check(CellAt(screen, 0, 0).fg == (ps::kColorPalette | 1), L"sgr 31 palette red");
    Check(CellAt(screen, 0, 1).fg == ps::kColorDefault, L"sgr 0 resets the colour");

    p.Feed(L"\x1b[1;4;7mB", screen);
    Check((CellAt(screen, 0, 2).flags & ps::kFlagBold) != 0, L"sgr bold");
    Check((CellAt(screen, 0, 2).flags & ps::kFlagUnderline) != 0, L"sgr underline");
    Check((CellAt(screen, 0, 2).flags & ps::kFlagInverse) != 0, L"sgr inverse");

    p.Feed(L"\x1b[0;38;5;196mI", screen);
    Check(CellAt(screen, 0, 3).fg == (ps::kColorPalette | 196), L"sgr 256 colour");

    p.Feed(L"\x1b[0;38;2;1;2;3mT", screen);
    Check(CellAt(screen, 0, 4).fg == (ps::kColorRgb | 0x010203), L"sgr truecolour");

    p.Feed(L"\x1b[0;44mZ", screen);
    Check(CellAt(screen, 0, 5).bg == (ps::kColorPalette | 4), L"sgr background");
}

void TestScrollbackAndSelection() {
    ps::Screen screen(10, 3);
    ps::Parser p;
    p.Feed(L"one\r\ntwo\r\nthree\r\nfour", screen);

    Check(screen.scrollback().size() == 1, L"scrolling fills the scrollback");
    Check(RowText(screen, 0) == L"two", L"screen scrolled up");
    Check(RowText(screen, 2) == L"four", L"newest line at the bottom");

    const std::vector<ps::Row> rows = screen.visibleRows();
    const unsigned long long firstId = screen.scrollback().front().id;
    ps::Selection sel;
    sel.active = true;
    sel.anchor = ps::Pos{firstId, 0};
    sel.focus = ps::Pos{rows[1].id, 3};

    std::vector<ps::Row> all(screen.scrollback().begin(), screen.scrollback().end());
    for (const ps::Row& row : rows) all.push_back(row);
    Check(ps::ExtractText(all, sel) == L"one\r\ntwo\r\nthr", L"selection spans scrollback");

    sel.anchor = sel.focus;
    Check(ps::ExtractText(all, sel).empty(), L"empty selection");
    sel.anchor = ps::Pos{rows[1].id, 3};
    sel.focus = ps::Pos{firstId, 0};
    Check(ps::ExtractText(all, sel) == L"one\r\ntwo\r\nthr", L"selection is direction-free");
}

void TestKeymap() {
    ps::KeyMode normal;
    ps::KeyMode app;
    app.appCursor = true;

    wg::KeyEvent key;
    key.vk = VK_UP;
    Check(ps::EncodeKey(key, normal) == "\x1b[A", L"arrow up");
    Check(ps::EncodeKey(key, app) == "\x1bOA", L"arrow up in application mode");
    key.shift = true;
    Check(ps::EncodeKey(key, normal) == "\x1b[1;2A", L"shift arrow");
    key.shift = false;
    key.ctrl = true;
    Check(ps::EncodeKey(key, normal) == "\x1b[1;5A", L"ctrl arrow");
    key.ctrl = false;

    key = wg::KeyEvent{};
    key.vk = VK_RETURN;
    Check(ps::EncodeKey(key, normal) == "\r", L"enter is CR");
    key.vk = VK_BACK;
    Check(ps::EncodeKey(key, normal) == "\x7f", L"backspace is DEL");
    key.vk = VK_TAB;
    Check(ps::EncodeKey(key, normal) == "\t", L"tab");
    key.shift = true;
    Check(ps::EncodeKey(key, normal) == "\x1b[Z", L"shift tab");
    key.shift = false;

    key = wg::KeyEvent{};
    key.vk = 'C';
    key.ctrl = true;
    Check(ps::EncodeKey(key, normal) == "\x03", L"ctrl+c is ETX");

    key = wg::KeyEvent{};
    key.vk = 'X';
    key.alt = true;
    Check(ps::EncodeKey(key, normal) == "\x1bX", L"alt prefix");

    key = wg::KeyEvent{};
    key.vk = VK_DELETE;
    Check(ps::EncodeKey(key, normal) == "\x1b[3~", L"delete");
    key.vk = VK_HOME;
    Check(ps::EncodeKey(key, normal) == "\x1b[H", L"home");

    key = wg::KeyEvent{};
    key.vk = 'A';
    Check(ps::EncodeKey(key, normal).empty(), L"plain letter defers to WM_CHAR");

    Check(ps::EncodePaste(L"a\r\nb", false) == "a\rb", L"paste normalises newlines");
    Check(ps::EncodePaste(L"a", true) == "\x1b[200~a\x1b[201~", L"bracketed paste");
}

void TestWheel() {
    using ps::WheelMode;
    using ps::WheelRoute;
    Check(ps::ParseWheelMode(L"") == WheelMode::Auto &&
              ps::ParseWheelMode(L"unknown") == WheelMode::Auto &&
              ps::ParseWheelMode(L"history") == WheelMode::History &&
              ps::ParseWheelMode(L"application") == WheelMode::Application,
          L"wheel setting parses and unknown value is safe");
    Check(ps::ParseWheelMode(ps::WheelModeName(WheelMode::Auto)) == WheelMode::Auto &&
              ps::ParseWheelMode(ps::WheelModeName(WheelMode::History)) == WheelMode::History &&
              ps::ParseWheelMode(ps::WheelModeName(WheelMode::Application)) == WheelMode::Application,
          L"wheel setting round trips");
    Check(ps::ChooseWheelRoute(WheelMode::Auto, false, false, false, false) == WheelRoute::History,
          L"auto leaves ordinary shell in history mode");
    Check(ps::ChooseWheelRoute(WheelMode::Auto, false, false, true, false) == WheelRoute::None,
          L"auto does not inject mouse into unknown alternate screen");
    Check(ps::ChooseWheelRoute(WheelMode::Auto, true, false, true, false) == WheelRoute::LegacyMouse &&
              ps::ChooseWheelRoute(WheelMode::Auto, true, true, true, false) == WheelRoute::SgrMouse,
          L"auto respects declared mouse encoding");
    Check(ps::ChooseWheelRoute(WheelMode::Auto, false, false, false, true) == WheelRoute::SgrMouse,
          L"auto uses trusted Pi fullscreen hint");
    Check(ps::ChooseWheelRoute(WheelMode::History, true, true, false, true) == WheelRoute::History &&
              ps::ChooseWheelRoute(WheelMode::History, true, true, true, true) == WheelRoute::None,
          L"history override avoids mouse injection and alternate screen history");
    Check(ps::ChooseWheelRoute(WheelMode::Application, false, false, false, false) == WheelRoute::SgrMouse,
          L"application override explicitly enables SGR");

    ps::WheelAccumulator wheel;
    Check(wheel.Add(30) == 0 && wheel.Add(30) == 0 && wheel.Add(30) == 0 &&
              wheel.Add(30) == 1, L"small wheel deltas accumulate");
    Check(wheel.Add(-30) == 0 && wheel.Add(30) == 0 && wheel.Add(-120) == -1,
          L"opposing wheel deltas cancel");
    Check(ps::EncodeWheel(true, 12, 4, true) == "\x1b[<64;12;4M", L"SGR wheel up");
    Check(ps::EncodeWheel(false, 80, 24, true, true, true, true) == "\x1b[<93;80;24M",
          L"SGR wheel down and modifiers");
    Check(ps::EncodeWheel(false, 1, 1, false) == std::string("\x1b[Ma!!"),
          L"legacy wheel down");

    ps::Screen screen(80, 24);
    ps::Parser parser;
    parser.Feed(L"\x1b[?1000h\x1b[?1002h\x1b[?1003h\x1b[?1006h", screen);
    Check(screen.mouseTracking() && screen.sgrMouse(), L"Pi mouse modes enabled");
    parser.Feed(L"\x1b[?1003l\x1b[?1002l", screen);
    Check(screen.mouseTracking(), L"other mouse mode stays enabled");
    parser.Feed(L"\x1b[?1000l\x1b[?1006l", screen);
    Check(!screen.mouseTracking() && !screen.sgrMouse(), L"mouse modes disabled");
    parser.Feed(L"\x1b[?1000h\x1b[?1006h\x1b" L"c", screen);
    Check(!screen.mouseTracking() && !screen.sgrMouse(), L"terminal reset clears mouse modes");

    Check(ps::AnchorScrollOffset(0, 4, 100) == 0, L"tail follows new output");
    Check(ps::AnchorScrollOffset(5, 3, 100) == 8, L"history stays on same row");
    Check(ps::AnchorScrollOffset(1999, 3, 2000) == 2000,
          L"history anchor remains bounded when ring fills");
}

void TestPiProcessClassification() {
    const std::wstring node = L"C:\\Program Files\\nodejs\\node.exe";
    const std::wstring script =
        L"C:\\Users\\Example User\\node_modules\\@earendil-works\\pi-coding-agent\\dist\\bundle\\cli.js";
    const std::wstring command = L"\"" + node + L"\" \"" + script + L"\"";
    Check(ps::IsPiCliCommand(node, command), L"Pi bundled CLI recognised");
    Check(ps::IsPiCliCommand(node, command + L" --tui-mode fullscreen"),
          L"Pi explicit fullscreen recognised");
    Check(!ps::IsPiCliCommand(node, command + L" --tui-mode regular") &&
              !ps::IsPiCliCommand(node, command + L" --tui-mode=regular") &&
              !ps::IsPiCliCommand(node, command + L" --tui-mode"),
          L"regular or ambiguous Pi mode not auto-detected");
    Check(!ps::IsPiCliCommand(L"C:\\tools\\other.exe", command),
          L"other process images are rejected");
    Check(!ps::IsPiCliCommand(node, L"node.exe -e \"" + script + L"\"") &&
              !ps::IsPiCliCommand(node, L"node.exe other.js \"" + script + L"\""),
          L"Pi path in another Node command is rejected");
    Check(!ps::IsPiCliCommand(node, L"node.exe C:\\tmp\\pi-coding-agent\\dist\\bundle\\cli.js"),
          L"unrelated script path is rejected");
    Check(!ps::HasPiCliDescendant(0), L"missing shell is rejected");
}

void TestPtyHelper() {
    // ConPTY only attaches a client when the caller has no console of its own,
    // so the round trip runs in a helper that drops its console first and
    // reports into a file.
    wchar_t cwd[MAX_PATH] = {0};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    const std::wstring dir = cwd;
    const std::wstring exe = dir + L"\\build\\pty-helper.exe";
    const std::wstring report = dir + L"\\build\\pty-report.txt";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"[ps-test] SKIP pty helper missing (%ls)\n", exe.c_str());
        return;
    }
    DeleteFileW(report.c_str());

    std::wstring cmdline = std::wstring(L"\"") + exe + L"\" \"" + report + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        wprintf(L"[ps-test] FAIL pty helper launch (%lu)\n", (unsigned long)GetLastError());
        ++g_failures;
        return;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    FILE* f = _wfopen(report.c_str(), L"rb");
    std::string body;
    if (f) {
        char buffer[4096];
        std::size_t n = 0;
        while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) body.append(buffer, n);
        fclose(f);
    }
    const bool pass = code == 0 && body.find("ok=1") != std::string::npos &&
                      body.find("HELPER-OK") != std::string::npos &&
                      body.find("mouse=1") != std::string::npos;
    Check(pass, L"pty round trip");
    if (!pass) {
        wprintf(L"[ps-test] helper exit=%lu report:\n%hs\n", (unsigned long)code,
                body.c_str());
    }
}
} // namespace

int wmain() {
    TestDecoding();
    TestTextAndWrap();
    TestCsi();
    TestSgr();
    TestScrollbackAndSelection();
    TestKeymap();
    TestWheel();
    TestPiProcessClassification();
    TestPtyHelper();

    if (g_failures != 0) {
        wprintf(L"[ps-test] %d failure(s)\n", g_failures);
        return 1;
    }
    wprintf(L"[ps-test] ok\n");
    return 0;
}
