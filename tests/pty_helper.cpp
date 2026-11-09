// Console-less ConPTY round trip, used by the test suite.
//
// A process that owns a console passes that console on to the client instead of
// the pseudo console, so the check has to run in a windowless (GUI subsystem)
// helper: it starts a terminal, types a command, writes what the screen showed
// to a report file and exits. ps-test.exe only reads the report.
#include "data/ps/terminal.h"
#include "data/ps/text.h"
#include "data/ps/keymap.h"
#include "widget/conpty.h"
#include "widget/store.h"

#include <shellapi.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::wstring ScreenText(const ps::Terminal& terminal) {
    const ps::View view = terminal.Snapshot();
    std::wstring out;
    for (const ps::ViewRow& row : view.lines) {
        for (const ps::Cell& cell : row.cells) {
            if (cell.wideTail || cell.ch == 0) continue;
            out.push_back(cell.ch);
        }
        out.push_back(L'\n');
    }
    return out;
}

bool RenderedContains(const std::string& output, const wchar_t* text) {
    ps::Screen screen(60, 10);
    ps::Parser parser;
    parser.Feed(ps::DecodeChunk(output), screen);
    std::wstring displayed;
    for (const auto& row : screen.visibleRows()) {
        for (const auto& cell : row.cells) {
            if (!cell.wideTail && cell.ch != 0) displayed.push_back(cell.ch);
        }
        displayed.push_back(L'\n');
    }
    return displayed.find(text) != std::wstring::npos;
}

void WriteReport(const wchar_t* path, bool ok, unsigned long long bytes,
                 const std::wstring& text) {
    FILE* f = _wfopen(path, L"wb");
    if (!f) return;
    fprintf(f, "ok=%d\nbytes=%llu\n", ok ? 1 : 0, bytes);
    const std::string utf8 = ps::EncodeUtf8(text);
    fwrite(utf8.data(), 1, utf8.size(), f);
    fclose(f);
}

bool TestMouse(std::wstring& details) {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    std::wstring client(path, length);
    const auto slash = client.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    client = client.substr(0, slash + 1) + L"pty-mouse-client.exe";

    wg::Pty pty;
    DWORD err = 0;
    if (!pty.Start(client, L"", 60, 10, L"", &err)) {
        details = L"mouse client start failed " + std::to_wstring(err);
        return false;
    }
    std::mutex mu;
    std::string output;
    std::thread reader([&] {
        char buffer[4096];
        for (;;) {
            const int n = pty.Read(buffer, sizeof(buffer));
            if (n <= 0) break;
            std::lock_guard<std::mutex> lock(mu);
            output.append(buffer, (std::size_t)n);
        }
    });

    bool ready = false;
    for (int i = 0; i < 80; ++i) {
        { std::lock_guard<std::mutex> lock(mu); ready = RenderedContains(output, L"MOUSE-READY"); }
        if (ready || !pty.running()) break;
        Sleep(50);
    }
    bool modes = false;
    {
        std::lock_guard<std::mutex> lock(mu);
        ps::Screen screen(60, 10);
        ps::Parser parser;
        parser.Feed(ps::DecodeChunk(output), screen);
        modes = screen.mouseTracking() && screen.sgrMouse();
    }
    if (ready) {
        pty.Write(ps::EncodeWheel(true, 12, 4, true, false, false, true));
        pty.Write(ps::EncodeWheel(false, 60, 10, true, false, true, false));
    }

    bool ok = false;
    for (int i = 0; i < 80; ++i) {
        { std::lock_guard<std::mutex> lock(mu); ok = RenderedContains(output, L"MOUSE-OK"); }
        if (ok || !pty.running()) break;
        Sleep(50);
    }
    pty.Shutdown();
    reader.join();
    pty.Close();
    {
        std::lock_guard<std::mutex> lock(mu);
        if (RenderedContains(output, L"MOUSE-OK")) ok = true;
        details = L"modes=" + std::to_wstring(modes ? 1 : 0) + L"\n" + ps::DecodeChunk(output);
    }
    return ok && modes;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    // A console inherited from the caller is handed straight to the client, and
    // the pseudo console then never sees it. Drop ours first: that is exactly
    // the situation the card runs in (a windowless desktop process).
    FreeConsole();

    // lpCmdLine keeps the quotes, so parse the command line properly.
    std::wstring report = L"build\\pty-report.txt";
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc > 1 && argv[1] && *argv[1]) report = argv[1];
        LocalFree(argv);
    }
    (void)commandLine;

    wg::Store store;
    // cmd.exe is enough for the round trip and starts faster than a profile-less
    // pwsh; the card's own shell choice is covered by ResolveShell's tests.
    store.Set(L"shell", L"C:\\Windows\\System32\\cmd.exe");

    ps::Terminal terminal;
    if (!terminal.Start(store, 60, 10)) {
        WriteReport(report.c_str(), false, 0, terminal.error());
        return 2;
    }

    terminal.Write("echo HELPER-OK\r");

    bool saw = false;
    std::wstring text;
    for (int i = 0; i < 100; ++i) {
        Sleep(100);
        text = ScreenText(terminal);
        if (text.find(L"HELPER-OK") != std::wstring::npos) {
            saw = true;
            break;
        }
    }

    terminal.Stop();
    std::wstring mouseDetails;
    const bool mouseOk = TestMouse(mouseDetails);
    text += L"\nmouse=" + std::to_wstring(mouseOk ? 1 : 0) + L"\n" + mouseDetails;
    WriteReport(report.c_str(), saw && mouseOk, terminal.bytesRead(), text);
    return saw && mouseOk ? 0 : 1;
}
