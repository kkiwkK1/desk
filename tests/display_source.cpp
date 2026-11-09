#include "widget/display.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <windows.h>

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wprintf(L"[display-source-test] FAIL %ls\n", message);
        ++failures;
    }
}

const wchar_t* kScriptPath = L"build\\tmp\\wg-display-source-test.txt";

bool WriteScript(const char* ascii) {
    CreateDirectoryW(L"build\\tmp", nullptr);
    const HANDLE handle =
        CreateFileW(kScriptPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const DWORD length = static_cast<DWORD>(std::strlen(ascii));
    DWORD written = 0;
    const bool ok = WriteFile(handle, ascii, length, &written, nullptr) != FALSE &&
                    written == length;
    CloseHandle(handle);
    return ok;
}

void RemoveScript() {
    DeleteFileW(kScriptPath);
}

bool SameRect(const RECT& rect, int left, int top, int right, int bottom) {
    return rect.left == left && rect.top == top && rect.right == right &&
           rect.bottom == bottom;
}

bool EmptyRect(const RECT& rect) {
    return rect.right <= rect.left || rect.bottom <= rect.top;
}

} // namespace

int wmain() {
    using namespace wg;

    // ---- system source: live desktop sanity --------------------------------
    {
        const std::shared_ptr<DisplaySource> system = CreateSystemDisplaySource();
        Check(system != nullptr, L"system display source is created");
        if (system) {
            const RECT primary = system->PrimaryWorkArea();
            Check(!EmptyRect(primary), L"system primary work area is non-empty");
            const RECT nearPoint =
                system->WorkAreaForPoint(POINT{primary.left + 1, primary.top + 1});
            Check(!EmptyRect(nearPoint),
                  L"system work area for a point inside the primary monitor is non-empty");
            Check(system->SystemDpi() > 0, L"system DPI is positive");
            Check(system->DpiForWindow(nullptr) == system->SystemDpi(),
                  L"null window falls back to the system DPI");
            Check(system->DpiForWindow(GetDesktopWindow()) > 0,
                  L"a valid window returns a positive DPI");
        }
    }

    // ---- scripted source: parsing, re-read, defaults -----------------------
    RemoveScript();
    {
        const std::shared_ptr<ScriptedDisplaySource> fresh =
            ScriptedDisplaySource::Create(kScriptPath);
        Check(fresh != nullptr, L"scripted source is created for a missing file");
        if (fresh) {
            Check(EmptyRect(fresh->PrimaryWorkArea()),
                  L"missing file: scripted source has no work area");
            Check(fresh->SystemDpi() == 96, L"missing file: DPI defaults to 96");
            RECT suggested{};
            Check(!fresh->SuggestedRect(&suggested),
                  L"missing file: no suggested rectangle");
            Check(!fresh->SuggestedRect(nullptr), L"null suggestion output is rejected");
        }
    }

    Check(WriteScript("work=100,200,800,600\r\ndpi=144\r\nrect=10,20,300,200\r\n"),
          L"script file is written");
    {
        const std::shared_ptr<ScriptedDisplaySource> scripted =
            ScriptedDisplaySource::Create(kScriptPath);
        Check(scripted != nullptr, L"scripted source is created");
        if (!scripted) {
            std::wprintf(L"[display-source-test] %d failure(s)\n", failures);
            return failures ? 1 : 0;
        }

        const RECT work = scripted->PrimaryWorkArea();
        Check(SameRect(work, 100, 200, 900, 800),
              L"work=x,y,w,h parses to a right/bottom rectangle");
        Check(SameRect(scripted->WorkAreaForPoint(POINT{-500, 900}), 100, 200, 900, 800),
              L"scripted point query returns the single work area");
        Check(scripted->SystemDpi() == 144, L"dpi= parses for SystemDpi");
        Check(scripted->DpiForWindow(nullptr) == 144, L"dpi= parses for DpiForWindow");
        Check(scripted->DpiForWindow(GetDesktopWindow()) == 144,
              L"scripted DpiForWindow ignores the window handle");
        RECT suggested{};
        Check(scripted->SuggestedRect(&suggested) &&
                  SameRect(suggested, 10, 20, 310, 220),
              L"rect=x,y,w,h parses to a suggested rectangle");

        // A query re-reads the file; a full new script replaces the values.
        Check(WriteScript("work=0,0,1024,768\ndpi=96\nrect=5,6,200,100\n"),
              L"second script is written");
        Check(SameRect(scripted->PrimaryWorkArea(), 0, 0, 1024, 768),
              L"work area is re-read on every query");
        Check(scripted->SystemDpi() == 96, L"DPI is re-read on every query");
        Check(scripted->SuggestedRect(&suggested) && SameRect(suggested, 5, 6, 205, 106),
              L"the suggestion is re-read on every query");

        // A readable file without rect= reports no suggestion.
        Check(WriteScript("work=0,0,1024,768\ndpi=96\n"), L"script without rect is written");
        Check(!scripted->SuggestedRect(&suggested),
              L"SuggestedRect is false when the current file has no rect=");

        // Invalid lines are ignored; the other fields still update.
        Check(WriteScript("garbage line\nwork=10,20,30,40\ndpi=not-a-number\n"
                          "rect=1,2,3\nbad=9,9,9,9\nwork=10,20,30,40 extra\n"),
              L"script with invalid lines is written");
        Check(SameRect(scripted->PrimaryWorkArea(), 10, 20, 40, 60),
              L"invalid lines are ignored while valid work= still applies");
        Check(scripted->SystemDpi() == 96, L"invalid dpi= keeps the last valid DPI");
        Check(!scripted->SuggestedRect(&suggested),
              L"invalid rect= line leaves no suggestion");

        // A zero-sized work rectangle is invalid.
        Check(WriteScript("work=1,2,0,50\ndpi=120\n"), L"zero-sized work script is written");
        Check(SameRect(scripted->PrimaryWorkArea(), 10, 20, 40, 60),
              L"zero-sized work= is ignored and keeps the last valid work area");
        Check(scripted->SystemDpi() == 120, L"the valid dpi= line still applies");

        // dpi is clamped into 48..960.
        Check(WriteScript("dpi=10\n"), L"low DPI script is written");
        Check(scripted->SystemDpi() == 48, L"dpi below 48 is clamped to 48");
        Check(WriteScript("dpi=5000\n"), L"high DPI script is written");
        Check(scripted->SystemDpi() == 960, L"dpi above 960 is clamped to 960");

        // Later valid lines win, negative origins are allowed, whitespace is
        // trimmed and trailing junk invalidates a line.
        Check(WriteScript("  work=-1920,0,1920,1080  \r\n\twork=0,0,800,600\t\r\n"
                          "dpi=100\ndpi=110\n"),
              L"multi-line script is written");
        Check(SameRect(scripted->PrimaryWorkArea(), 0, 0, 800, 600),
              L"the last valid work= line wins and whitespace is trimmed");
        Check(scripted->SystemDpi() == 110, L"the last valid dpi= line wins");

        // Missing file keeps every last valid value, including the suggestion.
        Check(WriteScript("work=64,64,800,600\ndpi=120\nrect=7,8,90,70\n"),
              L"script before deletion is written");
        Check(SameRect(scripted->PrimaryWorkArea(), 64, 64, 864, 664) &&
                  scripted->SystemDpi() == 120 &&
                  scripted->SuggestedRect(&suggested) &&
                  SameRect(suggested, 7, 8, 97, 78),
              L"scripted values are readable before the file disappears");
        RemoveScript();
        Check(SameRect(scripted->PrimaryWorkArea(), 64, 64, 864, 664),
              L"a missing file keeps the last valid work area");
        Check(scripted->SystemDpi() == 120, L"a missing file keeps the last valid DPI");
        Check(scripted->SuggestedRect(&suggested) && SameRect(suggested, 7, 8, 97, 78),
              L"a missing file keeps the last valid suggestion");
        // Re-creating the file resumes live updates.
        Check(WriteScript("work=0,0,300,200\ndpi=96\n"), L"script is recreated");
        Check(SameRect(scripted->PrimaryWorkArea(), 0, 0, 300, 200),
              L"queries resume reading when the file reappears");
    }
    RemoveScript();

    if (failures) {
        std::wprintf(L"[display-source-test] %d failure(s)\n", failures);
        return 1;
    }
    std::wprintf(L"[display-source-test] ok\n");
    return 0;
}
