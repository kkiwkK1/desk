// Renders the whole board without a window so the visual language can be
// reviewed in a BMP and a regression is visible on a build machine.
#include "bmp.h"

#include "panels/account/account_view.h"
#include "panels/assistant/assistant.h"
#include "panels/memory/memory.h"
#include "panels/ps/ps_view.h"
#include "panels/servers/servers_view.h"

#include "widget/cjk.h"
#include "widget/skin.h"
#include "widget/size.h"
#include "widget/store.h"

#include "data/aimeter/aimeter_data.h"
#include "data/aimeter/token_usage.h"
#include "data/assistant/memory.h"
#include "data/ps/screen.h"
#include "data/ps/vt.h"
#include "data/server/providers.h"
#include "data/server/status_view.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

long long NowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER v{};
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return (long long)(v.QuadPart / 10000ULL) - 11644473600000LL;
}

aimeter::AccountSnapshot MakeAccountRow(const wchar_t* label, double fiveUsed, double weekUsed,
                                        double monthly, long long resetFive,
                                        long long resetWeek, long long now) {
    aimeter::AccountSnapshot account;
    account.label = label;
    account.ok = true;
    account.fiveHour = aimeter::QuotaWindow{fiveUsed, 14.0, resetFive, false};
    account.weekly = aimeter::QuotaWindow{weekUsed, 35.0, resetWeek, false};
    account.monthlyCredits = monthly;
    account.asOfMs = now;
    return account;
}

aimeter::AimeterData MakeAccount() {
    aimeter::AimeterData data;
    const long long now = NowMs();
    data.showDeepseek = true;
    data.rows = {
        wg::Row{L"DEEPSEEK", L"CNY 6.75", L"", -1.0f, 0, L"TODAY -0.45", 0},
        wg::Row{L"CODEX", L"6% / 1%", L"", 0.06f, 0, L"", 0},
    };
    data.accounts.accounts = {
        MakeAccountRow(L"ACC1", 5.4, 25.45, 0.78, now + 3 * 3600 * 1000LL,
                       now + 5 * 24 * 3600 * 1000LL, now),
        MakeAccountRow(L"ACC2", 0.0, 10.33, 59.67, 0, now + 4 * 24 * 3600 * 1000LL, now),
    };
    data.accounts.anyOk = true;
    data.accounts.tightestPct = 72.7;
    data.accounts.monthlyCreditsTotal = 60.45;
    data.weekly = {
        aimeter::HistoryPoint{L"2026-11-01", 41.0f},
        aimeter::HistoryPoint{L"2026-11-02", 48.0f},
        aimeter::HistoryPoint{L"2026-11-03", 52.0f},
        aimeter::HistoryPoint{L"2026-11-04", -1.0f},
        aimeter::HistoryPoint{L"2026-11-05", 66.0f},
        aimeter::HistoryPoint{L"2026-11-06", 70.0f},
        aimeter::HistoryPoint{L"2026-11-07", 72.7f},
    };
    data.accountsAsOfMs = now;
    data.balanceOk = true;
    data.balanceText = L"CNY 6.75";
    data.spendCents = 45;
    data.tokensOk = true;
    data.tokens.uncachedInput = 412000;
    data.tokens.cacheRead = 690000;
    data.tokens.cacheWrite = 88000;
    data.tokens.output = 52000;
    return data;
}

assistant_view::AssistantViewState MakeAssistant() {
    assistant_view::AssistantViewState state;
    state.state = assistant_view::State::Idle;
    state.brief = L"桌面组件：Account 卡已合并，下一里程碑是助理与记忆卡。";
    // Already wrapped for the 544px text column, exactly as the engine will
    // hand it over after cjk::Wrap.
    state.answerLines = {
        L"目标：桌面组件成为日常工作的主面板，可召唤助理问答。",
        L"当前进度：Account 卡已合入 dev，MEMORY 卡在实现中。",
        L"卡点：pi 的真实网络链路要在沙箱外验证。",
    };
    state.citation = L"DSH SESSION 2026-11-07 / desktop-desk";
    state.input = L"下一步做什么？";
    state.caret = (int)state.input.size();
    state.inputTokens = 1840;
    state.outputTokens = 326;
    state.elapsedMs = 4200;
    return state;
}

memory_view::MemoryViewState MakeMemory() {
    const long long now = NowMs();
    auto fact = [&](const wchar_t* id, const wchar_t* kind, const wchar_t* text,
                    const wchar_t* source, long long ageMs, bool pinned) {
        memory_view::FactView view;
        view.id = id;
        view.kind = kind;
        view.text = text;
        view.sourceRef = source;
        view.createdMs = now - ageMs;
        view.updatedMs = now - ageMs;
        view.pinned = pinned;
        return view;
    };

    memory_view::MemoryViewState state;
    state.facts = {
        fact(L"f1", L"goal", L"让桌面组件成为日常工作的主面板",
             L"dsh session 11-06", 2 * 3600 * 1000LL, true),
        fact(L"f2", L"pref", L"回答用中文，先结论后依据", L"user 11-04",
             3 * 24 * 3600 * 1000LL, false),
        fact(L"f3", L"constraint", L"不修改主工作区，只在 worktree 内动手",
             L"persona", 5 * 24 * 3600 * 1000LL, false),
        fact(L"f4", L"milestone", L"M0a Account 卡已合并", L"dev @ m0a-final",
             8 * 24 * 3600 * 1000LL, false),
        fact(L"f5", L"project", L"Desktop Desk / 桌面组件", L"workspace.json",
             12 * 3600 * 1000LL, false),
    };
    state.selected = 1;
    return state;
}

// True when any pixel in the rectangle differs from 'background'. The render
// test uses it to prove the first CJK answer line is painted inside its own
// line box: the two rows above the box stay background, the box itself has ink.
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

server::ServerData MakeServers() {
    server::ServerData data;
    data.sshFound = true;
    data.sshPath = L"C:\\Windows\\System32\\OpenSSH\\ssh.exe";
    data.updated = L"14:32";

    auto add = [&](const wchar_t* name, bool ok, int cpu, int mem, int disk, double load,
                   int cores, long long up, int rtt, const wchar_t* state) {
        server::HostStatus host;
        host.name = name;
        host.target = name;
        host.ok = ok;
        host.cpuPercent = cpu;
        host.memPercent = mem;
        host.diskPercent = disk;
        host.load1 = load;
        host.cores = cores;
        host.uptimeSec = up;
        host.rttMs = rtt;
        host.state = state;
        data.hosts.push_back(std::move(host));
    };
    add(L"WEB-01", true, 12, 43, 61, 0.42, 8, 1814400, 118, L"UP");
    add(L"DB-01", true, 3, 27, 24, 0.16, 4, 18000, 96, L"UP");
    add(L"CACHE-01", true, 8, 51, 33, 0.31, 4, 1036800, 41, L"UP");
    add(L"10.0.0.9", false, -1, -1, -1, -1.0, 0, -1, 8000, L"TIMEOUT");

    wg::Theme theme;
    data.rows = server::BuildRows(data, theme);
    return data;
}

// Feed a VT script through the real parser so the snapshot is exactly what the
// card would paint.
ps::View MakeView(int cols, int rows, std::wstring_view script) {
    ps::Screen screen(cols, rows);
    ps::Parser parser;
    parser.Feed(script, screen);

    ps::View view;
    view.cols = screen.cols();
    view.rows = screen.rows();
    for (const ps::Row& row : screen.visibleRows()) {
        ps::ViewRow out;
        out.id = row.id;
        out.cells = row.cells;
        view.lines.push_back(std::move(out));
    }
    view.cursorRow = screen.cursorRow();
    view.cursorCol = screen.cursorCol();
    view.cursorVisible = screen.cursorVisible();
    view.alternate = screen.alternate();
    return view;
}

std::wstring TerminalScript() {
    return
        // Prompt, then a directory listing with palette colours.
        L"\x1b[38;5;39mPS\x1b[0m \x1b[38;5;250mC:\\Users\\kkiwkK1>\x1b[0m ls\r\n"
        L"\x1b[38;5;33mdocs\x1b[0m   \x1b[38;5;40mdesk.exe\x1b[0m   README.md\r\n"
        L"\x1b[38;5;33msrc\x1b[0m    \x1b[38;5;40msrvctl.exe\x1b[0m  build.bat\r\n"
        // An inverse-video header bar, the way a TUI draws one.
        L"\x1b[7m PKG   VERSION   INSTALLED                  \x1b[0m\r\n"
        L" os    10.0.26100 2026-11-06                 \r\n"
        L" pwsh  7.6.9       L:\\powershell             \r\n"
        L" git   2.51.0      C:\\Program Files\\Git      \r\n"
        // A bold/underline line and CJK text.
        L"\x1b[1;4mready\x1b[0m \u5F85\u547D\u4EE4\r\n"
        // Cursor left at a prompt.
        L"\x1b[38;5;39mPS\x1b[0m \x1b[38;5;250mC:\\Users\\kkiwkK1>\x1b[0m";
}

void DrawCard(wg::Canvas& canvas, const wg::Rect& card, const std::wstring& title,
              const std::wstring& status, wg::Color color, bool expanded,
              const wg::Theme& theme, const std::function<void(const wg::Rect&)>& body) {
    wg::skin::CardChrome(canvas, card, title, status, color, theme, true, expanded, false,
                         false);
    const wg::Rect r = wg::skin::CardBody(card);
    canvas.SetClip(r.x, r.y, r.w, r.h);
    body(r);
    canvas.ResetClip();
}

// ---- MEMORY card <-> shared memory store ----------------------------------
//
// Synthetic fixture only: a scratch directory under the test exe (build\), its
// own facts file and a wg::Store bound exactly the way the board binds one. The
// assistant engine, the user's DSH and any real facts file stay untouched, so
// the render snapshot above keeps its fixed data.

constexpr int kStoreWaitMs = 3000;

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

// The store notifies synchronously from the mutating thread, but the card must
// not depend on that: a bounded wait keeps the test honest for either shape.
bool WaitFor(const std::function<bool()>& done) {
    for (int waited = 0; waited <= kStoreWaitMs; waited += 10) {
        if (done()) return true;
        Sleep(10);
    }
    return done();
}

int MemoryIndex(const memory_view::MemoryViewState& state, const wchar_t* id) {
    for (std::size_t i = 0; i < state.facts.size(); ++i) {
        if (state.facts[i].id == id) return (int)i;
    }
    return -1;
}

assistant::Fact MakeStoredFact(const wchar_t* id, const wchar_t* kind,
                               const std::wstring& text, long long updatedMs) {
    assistant::Fact fact;
    if (id != nullptr) fact.id = id;
    fact.kind = kind;
    fact.text = text;
    fact.sourceRef = L"desk-render-test";
    fact.createdMs = updatedMs;
    fact.updatedMs = updatedMs;
    return fact;
}

// Menu id looked up by label, so a menu reorder cannot silently make this test
// press the wrong item (Delete instead of Pin would still "change the store").
int MenuId(const std::vector<wg::MenuItem>& menu, const wchar_t* label) {
    for (const wg::MenuItem& item : menu) {
        if (item.label == label) return item.id;
    }
    return 0;
}

// Card-level integration: the card must act on the *shared* store the assistant
// engine uses (same instance, same facts file), publish changes it did not make
// itself, survive its own destruction with a notification in flight, and keep
// the selected fact by id across a reorder.
int RunMemoryCardWiring() {
    int failures = 0;
    const auto fail = [&failures](const wchar_t* what) {
        wprintf(L"[desk-test] %ls\n", what);
        ++failures;
    };

    const std::wstring dir = ExeDir() + L"\\desk-render-test-memory";
    RemoveTree(dir);
    CreateDirectoryW(dir.c_str(), nullptr);

    // Exactly what the board does: bind the card's own config file. Load()
    // derives the memory directory from its parent.
    wg::Store config;
    config.Open(dir + L"\\memory.ini");
    auto repaints = std::make_shared<int>(0);
    std::unique_ptr<MemoryModule> card = CreateMemoryModule();
    card->Bind(&config, [repaints] { ++*repaints; });

    std::wstring error;
    // The instance the engine would open for the same directory.
    const std::shared_ptr<assistant::MemoryStore> store =
        assistant::OpenSharedMemory(dir, &error);
    if (!store) {
        wprintf(L"[desk-test] cannot open the shared store: %ls\n", error.c_str());
        return failures + 1;
    }

    card->Load();
    if (!card->ViewState().facts.empty() || !card->ViewState().error.empty() ||
        card->ViewState().loading) {
        fail(L"a fresh card publishes an empty snapshot without an error");
    }

    // ---- a change made through the shared instance reaches the card ------
    const long long now = NowMs();
    bool seeded = store->AddFact(MakeStoredFact(L"mem-t1", L"goal", L"共享目标一", now - 30000), &error) &&
                  store->AddFact(MakeStoredFact(L"mem-t2", L"pref", L"共享偏好二", now - 20000), &error) &&
                  store->AddFact(MakeStoredFact(L"mem-t3", L"constraint", L"共享约束三", now - 10000), &error);
    if (!seeded) {
        wprintf(L"[desk-test] cannot seed the shared store: %ls\n", error.c_str());
        RemoveTree(dir);
        return failures + 1;
    }
    if (!WaitFor([&] { return card->ViewState().facts.size() == 3; })) {
        fail(L"facts added to the shared store are not published to the card");
    }

    memory_view::MemoryViewState view = card->ViewState();
    const std::vector<assistant::Fact> stored = store->Facts();
    if (view.facts.size() != stored.size()) {
        fail(L"the card lists a different number of facts than the store");
    } else {
        for (std::size_t i = 0; i < stored.size(); ++i) {
            // Order and every painted field come from the store: the card is
            // not allowed to reorder or to drop part of the mapping.
            if (view.facts[i].id != stored[i].id || view.facts[i].kind != stored[i].kind ||
                view.facts[i].text != stored[i].text ||
                view.facts[i].pinned != stored[i].pinned ||
                view.facts[i].updatedMs != stored[i].updatedMs ||
                view.facts[i].createdMs != stored[i].createdMs ||
                view.facts[i].expiresMs != stored[i].expiresMs ||
                view.facts[i].sourceRef != stored[i].sourceRef) {
                fail(L"Fact -> FactView mapping is incomplete or reordered");
                break;
            }
        }
    }

    // ---- Pin in the card menu is the same durable change -----------------
    const int t3Row = MemoryIndex(view, L"mem-t3");
    if (t3Row < 0) {
        fail(L"the seeded fact mem-t3 is missing from the card");
    } else {
        card->OnClick(0, memory_view::kTopInset + t3Row * memory_view::kRowPitch + 3);
        if (card->ViewState().selected != t3Row) {
            fail(L"a click does not select the row it hit");
        }
        const int pinMenu = MenuId(card->Menu(), L"Pin fact");
        if (pinMenu == 0) fail(L"the card menu has no Pin item");
        card->OnMenu(pinMenu);
        bool pinnedInStore = false;
        for (const assistant::Fact& fact : store->Facts()) {
            if (fact.id == L"mem-t3") pinnedInStore = fact.pinned;
        }
        if (!pinnedInStore) fail(L"Pin in the card menu does not change the shared store");
        view = card->ViewState();
        const int pinnedRow = MemoryIndex(view, L"mem-t3");
        if (pinnedRow < 0 || !view.facts[(std::size_t)pinnedRow].pinned) {
            fail(L"the card does not show the pinned fact");
        } else if (view.selected != pinnedRow) {
            // The store sorts pinned facts first; the selection must follow the
            // fact, not stay on whatever index moved into that row.
            fail(L"a reorder moved the selection onto another fact");
        }
    }

    // ---- Delete in the card menu ---------------------------------------
    view = card->ViewState();
    const int t2Row = MemoryIndex(view, L"mem-t2");
    if (t2Row < 0) {
        fail(L"the seeded fact mem-t2 is missing from the card");
    } else {
        card->OnClick(0, memory_view::kTopInset + t2Row * memory_view::kRowPitch + 3);
        card->OnMenu(MenuId(card->Menu(), L"Delete fact"));
        bool stillListed = false;
        for (const assistant::Fact& fact : store->Facts()) {
            if (fact.id == L"mem-t2") stillListed = true;
        }
        if (stillListed) fail(L"Delete in the card menu does not forget the fact");
        if (MemoryIndex(card->ViewState(), L"mem-t2") >= 0) {
            fail(L"the deleted fact is still painted");
        }
        if (card->ViewState().selected != -1) {
            fail(L"deleting the selected fact leaves the selection on a neighbour");
        }
    }

    // ---- a failed write is shown and rolled back ------------------------
    // An exclusive handle on the facts file makes the atomic rewrite fail, the
    // same shape as a locked or AV-scanned file (see assistant_memory.cpp).
    const std::wstring factsFile = dir + L"\\assistant-facts.jsonl";
    HANDLE factsLock = CreateFileW(factsFile.c_str(), GENERIC_READ, 0, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (factsLock == INVALID_HANDLE_VALUE) {
        fail(L"the facts file cannot be locked for the failure case");
        RemoveTree(dir);
        return failures + 1;
    }
    view = card->ViewState();
    const int t1Row = MemoryIndex(view, L"mem-t1");
    if (t1Row < 0) {
        fail(L"the seeded fact mem-t1 is missing from the card");
    } else {
        card->OnClick(0, memory_view::kTopInset + t1Row * memory_view::kRowPitch + 3);
        card->OnMenu(MenuId(card->Menu(), L"Pin fact"));
        view = card->ViewState();
        if (view.error.empty()) fail(L"a failed Pin is not shown on the card");
        const int t1After = MemoryIndex(view, L"mem-t1");
        if (t1After < 0 || view.facts[(std::size_t)t1After].pinned) {
            fail(L"a failed Pin still looks applied on the card");
        }
        bool storePinned = false;
        for (const assistant::Fact& fact : store->Facts()) {
            if (fact.id == L"mem-t1") storePinned = fact.pinned;
        }
        if (storePinned) fail(L"a failed Pin left the shared store pinned");

        card->OnMenu(MenuId(card->Menu(), L"Delete fact"));
        view = card->ViewState();
        if (view.error.empty()) fail(L"a failed Delete is not shown on the card");
        if (MemoryIndex(view, L"mem-t1") < 0) fail(L"a failed Delete dropped the row");
        bool active = false;
        for (const assistant::Fact& fact : store->Facts()) {
            if (fact.id == L"mem-t1") active = true;
        }
        if (!active) fail(L"a failed Delete dropped the fact from the store");
    }
    CloseHandle(factsLock);

    // ---- recovery: the same menu action works again ----------------------
    card->OnMenu(MenuId(card->Menu(), L"Pin fact"));
    view = card->ViewState();
    if (!view.error.empty()) fail(L"the error line survives a successful change");
    const int recovered = MemoryIndex(view, L"mem-t1");
    if (recovered < 0 || !view.facts[(std::size_t)recovered].pinned) {
        fail(L"Pin does not work again once the file is writable");
    }

    // ---- Refresh re-reads the shared instance ---------------------------
    const std::size_t before = card->ViewState().facts.size();
    card->OnMenu(MenuId(card->Menu(), L"Refresh"));
    view = card->ViewState();
    if (view.facts.size() != before || !view.error.empty()) {
        fail(L"Refresh does not republish the shared store snapshot");
    }

    // ---- destruction: a late notification must not touch the card --------
    const int repaintsBefore = *repaints;
    card.reset();
    if (!store->AddFact(MakeStoredFact(L"mem-t4", L"fact", L"销毁后仍可写入", 0), &error)) {
        fail(L"the shared store is not usable after the card is destroyed");
    }
    if (*repaints != repaintsBefore) {
        fail(L"a notification repainted a destroyed card");
    }

    // A notification racing the destructor: the handler owns the card state, so
    // it may run before, during or after teardown without touching freed memory.
    {
        wg::Store raceConfig;
        raceConfig.Open(dir + L"\\memory-race.ini");
        std::unique_ptr<MemoryModule> racing = CreateMemoryModule();
        racing->Bind(&raceConfig, [repaints] { ++*repaints; });
        racing->Load();
        std::thread writer([store] {
            for (int i = 0; i < 20; ++i) {
                std::wstring ignored;
                store->AddFact(MakeStoredFact(nullptr, L"race",
                                              L"并发事实" + std::to_wstring(i), 0),
                               &ignored);
                Sleep(1);
            }
        });
        racing.reset();
        writer.join();
    }
    const int settled = *repaints;
    if (!store->AddFact(MakeStoredFact(L"mem-t5", L"fact", L"再写一条", 0), &error)) {
        fail(L"the shared store is not usable after the racing card is gone");
    }
    if (*repaints != settled) fail(L"a notification repainted a destroyed card");

    RemoveTree(dir);
    return failures;
}

// ---- MEMORY card: edit + clear all (M1-U) ---------------------------------
//
// The framework prompts are injected here, so no modal loop can open on the
// render machine: the test decides what the user "typed" and whether the
// "clear?" confirmation was accepted. Everything else is the real card talking
// to the real shared store, i.e. the same instance the assistant engine holds.
int RunMemoryEditClearWiring() {
    int failures = 0;
    const auto fail = [&failures](const wchar_t* what) {
        wprintf(L"[desk-test] %ls\n", what);
        ++failures;
    };

    const std::wstring dir = ExeDir() + L"\\desk-render-test-memory-edit";
    RemoveTree(dir);
    CreateDirectoryW(dir.c_str(), nullptr);

    wg::Store config;
    config.Open(dir + L"\\memory.ini");
    auto repaints = std::make_shared<int>(0);
    std::unique_ptr<MemoryModule> card = CreateMemoryModule();
    card->Bind(&config, [repaints] { ++*repaints; });

    std::wstring error;
    const std::shared_ptr<assistant::MemoryStore> store =
        assistant::OpenSharedMemory(dir, &error);
    if (!store) {
        wprintf(L"[desk-test] cannot open the edit/clear store: %ls\n", error.c_str());
        return failures + 1;
    }

    // Injected prompts. editOk == false is the user pressing Esc; confirmOk is
    // the Ok/Cancel answer of the destructive confirmation.
    std::wstring editText;
    bool editOk = false;
    int editCalls = 0;
    std::wstring editInitial;
    card->SetEditPromptHandler([&](const std::wstring& initial, std::wstring* out) {
        ++editCalls;
        editInitial = initial;
        if (!editOk) return false;
        if (out != nullptr) *out = editText;
        return true;
    });
    bool confirmOk = false;
    int confirmCalls = 0;
    std::wstring confirmMessage;
    card->SetConfirmHandler([&](const std::wstring& message) {
        ++confirmCalls;
        confirmMessage = message;
        return confirmOk;
    });

    card->Load();
    if (!card->ViewState().facts.empty() || !card->ViewState().error.empty()) {
        fail(L"a fresh edit/clear card does not start empty and error-free");
    }

    const long long now = NowMs();
    const bool seeded =
        store->AddFact(MakeStoredFact(L"mem-e1", L"pref", L"编辑前的事实", now - 20000),
                       &error) &&
        store->AddFact(MakeStoredFact(L"mem-e2", L"goal", L"第二条事实", now - 10000),
                       &error);
    if (!seeded) {
        wprintf(L"[desk-test] cannot seed the edit/clear store: %ls\n", error.c_str());
        RemoveTree(dir);
        return failures + 1;
    }
    if (!WaitFor([&] { return card->ViewState().facts.size() == 2; })) {
        fail(L"the edit/clear fixture facts are not published to the card");
    }

    if (card->ViewState().facts.empty() || card->ViewState().facts[0].id != L"mem-e2") {
        fail(L"the newest fact is not the first row of the edit/clear card");
    } else {
        card->OnClick(0, memory_view::kTopInset + 3);
        if (card->ViewState().selected != 0) fail(L"a click does not select the edit target row");
    }

    // ---- edit success ---------------------------------------------------
    editText = L"编辑后的事实文本";
    editOk = true;
    editInitial.clear();
    card->OnMenu(MenuId(card->Menu(), L"Edit fact"));
    if (editCalls != 1) fail(L"Edit fact does not open the injected prompt exactly once");
    if (editInitial != L"第二条事实") {
        fail(L"the edit prompt is not seeded with the selected fact text");
    }
    bool edited = false;
    for (const assistant::Fact& fact : store->Facts()) {
        if (fact.id == L"mem-e2" && fact.text == L"编辑后的事实文本") edited = true;
    }
    if (!edited) fail(L"the edited text is not stored in the shared instance");
    {
        const memory_view::MemoryViewState view = card->ViewState();
        const int row = MemoryIndex(view, L"mem-e2");
        if (row < 0 || view.facts[(std::size_t)row].text != L"编辑后的事实文本") {
            fail(L"the card does not paint the edited text");
        }
        if (!view.error.empty()) fail(L"a successful edit leaves an error line");
        if (view.selected != row) fail(L"an edit moved the selection onto another fact");
    }

    // ---- cancel changes nothing -----------------------------------------
    editOk = false;
    card->OnMenu(MenuId(card->Menu(), L"Edit fact"));
    if (editCalls != 2) fail(L"the cancelled edit did not reach the prompt");
    bool cancelled = true;
    for (const assistant::Fact& fact : store->Facts()) {
        if (fact.id == L"mem-e2" && fact.text != L"编辑后的事实文本") cancelled = false;
    }
    if (!cancelled) fail(L"cancelling the edit still changed the fact text");
    if (!card->ViewState().error.empty()) fail(L"cancelling the edit reports an error");

    // ---- empty text is rejected visibly ---------------------------------
    editText.clear();
    editOk = true;
    card->OnMenu(MenuId(card->Menu(), L"Edit fact"));
    if (card->ViewState().error.empty()) {
        fail(L"an empty edit is not shown on the card");
    }
    if (card->ViewState().facts.empty()) {
        fail(L"a rejected edit emptied the fact list");
    }
    for (const assistant::Fact& fact : store->Facts()) {
        if (fact.id == L"mem-e2" && fact.text != L"编辑后的事实文本") {
            fail(L"an empty edit changed the stored fact text");
        }
    }

    // ---- sensitive text is rejected visibly -----------------------------
    editText = L"token sk-abcdef123456";
    card->OnMenu(MenuId(card->Menu(), L"Edit fact"));
    if (card->ViewState().error.empty()) {
        fail(L"a sensitive edit is not shown on the card");
    }
    for (const assistant::Fact& fact : store->Facts()) {
        if (fact.id == L"mem-e2" && fact.text.find(L"sk-") != std::wstring::npos) {
            fail(L"a sensitive edit reached the shared store");
        }
    }

    // ---- the error survives an unrelated store notification -------------
    // The error belongs to this card's failed action; a fact the engine writes
    // after an answer must not wipe it before the user has read it.
    const std::wstring errorBefore = card->ViewState().error;
    if (!store->AddFact(MakeStoredFact(L"mem-e3", L"fact", L"引擎新写的事实", now), &error)) {
        fail(L"cannot add the unrelated notification fact");
    }
    if (!WaitFor([&] { return card->ViewState().facts.size() == 3; })) {
        fail(L"the unrelated fact is not published to the card");
    }
    if (card->ViewState().error != errorBefore) {
        fail(L"an unrelated store notification swallowed the card error");
    }

    // ---- a non-empty list still paints the error line -------------------
    {
        const memory_view::MemoryViewState errorView = card->ViewState();
        if (errorView.facts.empty() || errorView.error.empty()) {
            fail(L"the error-layout fixture does not have facts plus an error");
        } else {
            memory_view::MemoryViewState cleanView = errorView;
            cleanView.error.clear();
            const int withError = memory_view::BodyHeight(errorView, true);
            const int withoutError = memory_view::BodyHeight(cleanView, true);
            if (withError != withoutError) {
                fail(L"the error line is not permanently reserved in the card height");
            }
            wg::Theme theme;
            wg::Canvas canvas;
            if (!canvas.Resize(memory_view::kCardWidth, withError)) return failures + 1;
            canvas.Clear(theme.panelSolid);
            memory_view::DrawBody(errorView, canvas,
                                  wg::Rect{0, 0, memory_view::kCardWidth, withError}, theme,
                                  true);
            const int rows = memory_view::VisibleRows(errorView, true);
            const int errorTop = memory_view::kTopInset + rows * memory_view::kRowPitch +
                                 (memory_view::HasMore(errorView, true)
                                      ? memory_view::kMoreLineH
                                      : 0) +
                                 2;
            if (!HasInk(canvas, memory_view::kPadX, errorTop, memory_view::kTextWidth, 12,
                        theme.panelSolid)) {
                fail(L"the error line under a non-empty fact list is blank");
            }
        }
    }

    // ---- a locked facts file fails the edit visibly ---------------------
    const std::wstring factsFile = dir + L"\\assistant-facts.jsonl";
    HANDLE factsLock = CreateFileW(factsFile.c_str(), GENERIC_READ, 0, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (factsLock == INVALID_HANDLE_VALUE) {
        fail(L"the facts file cannot be locked for the edit failure case");
        RemoveTree(dir);
        return failures + 1;
    }
    editText = L"写不进去的编辑";
    editOk = true;
    card->OnMenu(MenuId(card->Menu(), L"Edit fact"));
    if (card->ViewState().error.empty()) fail(L"a failed edit is not shown on the card");
    bool editKept = false;
    for (const assistant::Fact& fact : store->Facts()) {
        if (fact.id == L"mem-e2" && fact.text == L"编辑后的事实文本") editKept = true;
    }
    if (!editKept) fail(L"a failed edit changed the stored text");
    CloseHandle(factsLock);

    // ---- the board's automatic refresh must not swallow the error -------
    // The board calls a card's Refresh() (worker) right after every menu action
    // and when the card is shown. That path keeps the error; the explicit menu
    // "Refresh" clears it, which is the documented lifetime of the error line.
    card->Refresh();
    if (card->ViewState().error.empty()) {
        fail(L"the board's automatic refresh swallowed the edit error");
    }
    card->OnMenu(MenuId(card->Menu(), L"Refresh"));
    if (!card->ViewState().error.empty()) {
        fail(L"the menu Refresh does not clear the error line");
    }

    // ---- recovery: the next successful edit clears the error ------------
    editText = L"恢复后的编辑";
    card->OnMenu(MenuId(card->Menu(), L"Edit fact"));
    if (!card->ViewState().error.empty()) fail(L"the error survives a successful edit");
    bool recoveredEdit = false;
    for (const assistant::Fact& fact : store->Facts()) {
        if (fact.id == L"mem-e2" && fact.text == L"恢复后的编辑") recoveredEdit = true;
    }
    if (!recoveredEdit) fail(L"Edit does not work again once the file is writable");

    // ---- Clear all memory: cancel first ---------------------------------
    if (!store->AppendEpisode(L"清空测试问", L"清空测试答", L"", &error)) {
        fail(L"cannot seed the clear-all episode");
    }
    const std::size_t factsBefore = store->Facts().size();
    confirmOk = false;
    card->OnMenu(MenuId(card->Menu(), L"Clear all memory"));
    if (confirmCalls != 1) fail(L"Clear all memory does not ask for confirmation");
    if (confirmMessage.empty()) fail(L"the clear confirmation has no message");
    if (store->Facts().size() != factsBefore) fail(L"a cancelled clear deleted facts");
    if (store->RecentEpisodes(0).empty()) fail(L"a cancelled clear deleted episodes");
    if (card->ViewState().facts.size() != factsBefore) {
        fail(L"a cancelled clear changed the card list");
    }

    // ---- confirm: one notification, both files and the card empty -------
    int notified = 0;
    const uint64_t token = store->Subscribe([&notified] { ++notified; });
    confirmOk = true;
    card->OnMenu(MenuId(card->Menu(), L"Clear all memory"));
    if (confirmCalls != 2) fail(L"the confirmed clear did not ask again");
    if (notified != 1) fail(L"Clear all memory does not notify exactly once");
    if (!card->ViewState().facts.empty()) {
        fail(L"the card still lists facts right after a successful clear");
    }
    if (card->ViewState().selected != -1) {
        fail(L"a cleared list keeps a stale selection");
    }
    if (!card->ViewState().error.empty()) fail(L"a successful clear leaves an error line");
    if (!store->Facts().empty() || !store->AllFacts().empty()) {
        fail(L"a confirmed clear left facts (or tombstones) in the shared store");
    }
    if (!store->RecentEpisodes(0).empty()) {
        fail(L"a confirmed clear left episodes in the shared store");
    }
    store->Unsubscribe(token);
    if (assistant::OpenSharedMemory(dir).get() != store.get()) {
        fail(L"the card and the engine no longer share one memory instance");
    }

    // ---- an already-empty clear still notifies ---------------------------
    // Clear all is also the user's explicit "forget the conversation" barrier:
    // the engine drops a Pi session that may hold cancelled/failed turns, so the
    // signal must fire even when facts and episodes are already empty.
    {
        int emptyNotifies = 0;
        const uint64_t emptyToken = store->Subscribe([&emptyNotifies] { ++emptyNotifies; });
        confirmOk = true;
        card->OnMenu(MenuId(card->Menu(), L"Clear all memory"));
        if (emptyNotifies != 1) {
            fail(L"an already-empty Clear all does not notify the engine");
        }
        if (!card->ViewState().error.empty() || !card->ViewState().facts.empty()) {
            fail(L"an already-empty Clear all changed the card");
        }
        store->Unsubscribe(emptyToken);
    }

    // ---- restart: a fresh store sees an empty memory --------------------
    {
        assistant::MemoryStore restarted;
        if (!restarted.Load(dir, &error)) fail(L"a fresh store cannot load after the clear");
        if (!restarted.AllFacts().empty() || !restarted.RecentEpisodes(0).empty()) {
            fail(L"cleared memory came back after a restart");
        }
    }

    card.reset();
    RemoveTree(dir);
    return failures;
}

// ---- MEMORY card: a fact-count change must re-plan the board ---------------
//
// The board only re-plans a card's rectangle on a layout commit; a repaint alone
// keeps the old height, so facts arriving from the engine (or a delete) could be
// clipped or overlap the next card. Each store notification therefore asks the
// host for exactly one refresh (an async post), and the card's own Refresh() -
// which is how the host answers that request - must not ask again.
int RunMemoryReplanWiring() {
    int failures = 0;
    const auto fail = [&failures](const wchar_t* what) {
        wprintf(L"[desk-test] %ls\n", what);
        ++failures;
    };

    const std::wstring dir = ExeDir() + L"\\desk-render-test-memory-replan";
    RemoveTree(dir);
    CreateDirectoryW(dir.c_str(), nullptr);

    wg::Store config;
    config.Open(dir + L"\\memory.ini");
    auto repaints = std::make_shared<int>(0);
    std::unique_ptr<MemoryModule> card = CreateMemoryModule();
    card->Bind(&config, [repaints] { ++*repaints; });
    // What the board binds: the counter stands in for PostMessageW(kMsgModuleRefresh).
    int replans = 0;
    card->BindRefresh([&replans] { ++replans; });

    std::wstring error;
    const std::shared_ptr<assistant::MemoryStore> store =
        assistant::OpenSharedMemory(dir, &error);
    if (!store) {
        wprintf(L"[desk-test] cannot open the replan store: %ls\n", error.c_str());
        return failures + 1;
    }

    card->Load();
    if (replans != 0) fail(L"loading/resubscribing a card already asks for a re-plan");
    const int heightEmpty = card->BodyHeight();

    // One fact added by someone else (the engine worker) -> one host re-plan.
    const long long now = NowMs();
    if (!store->AddFact(MakeStoredFact(L"mem-p1", L"goal", L"重排事实一", now), &error)) {
        fail(L"cannot seed the first replan fact");
        RemoveTree(dir);
        return failures + 1;
    }
    if (!WaitFor([&] { return card->ViewState().facts.size() == 1; })) {
        fail(L"the first replan fact is not published to the card");
    }
    if (replans != 1) {
        fail(L"a store notification does not request exactly one host re-plan");
    }

    // A second row grows the card past its empty height: that is exactly the case
    // a bare repaint would have clipped.
    if (!store->AddFact(MakeStoredFact(L"mem-p2", L"pref", L"重排事实二", now - 1000),
                        &error)) {
        fail(L"cannot seed the second replan fact");
    }
    if (!WaitFor([&] { return card->ViewState().facts.size() == 2; })) {
        fail(L"the second replan fact is not published to the card");
    }
    if (replans != 2) fail(L"every store notification requests one host re-plan");
    if (card->BodyHeight() <= heightEmpty) {
        fail(L"the replan fixture does not actually change the card height");
    }

    // The card's own Refresh is the host's answer to the request above. It
    // republishes (and repaints) but must never post another refresh, or the
    // board would refresh this card forever.
    const int repaintsBeforeRefresh = *repaints;
    card->Refresh();
    if (replans != 2) fail(L"the card's Refresh recursively requests another host refresh");
    if (*repaints == repaintsBeforeRefresh) fail(L"the card's Refresh does not repaint");
    card->Refresh();
    if (replans != 2) fail(L"a repeated Refresh keeps asking the host for refreshes");

    // Deleting a row shrinks the card again: still exactly one request.
    if (!store->DeleteFact(L"mem-p2", &error)) fail(L"cannot delete the replan fact");
    if (!WaitFor([&] { return card->ViewState().facts.size() == 1; })) {
        fail(L"the delete is not published to the card");
    }
    if (replans != 3) fail(L"a delete does not request exactly one host re-plan");
    if (card->BodyHeight() != heightEmpty) {
        fail(L"the card height does not shrink back after the delete");
    }

    // Teardown: 'replan' is cleared with 'wake' under the state lock, so a late
    // notification must neither repaint nor ask a destroyed card for a refresh.
    card.reset();
    const int repaintsBefore = *repaints;
    if (!store->AddFact(MakeStoredFact(L"mem-p3", L"fact", L"销毁后的事实", 0), &error)) {
        fail(L"the store is not usable after the replan card is gone");
    }
    if (replans != 3) fail(L"a notification after teardown requested a host refresh");
    if (*repaints != repaintsBefore) fail(L"a notification after teardown repainted a dead card");

    RemoveTree(dir);
    return failures;
}

// ---- ASSISTANT BRIEF refresh (M1-U) ---------------------------------------
//
// Engine::Brief() reads the DSH project files, so the card must never call it
// from a pi callback or the UI thread: it asks the host, which posts the request
// into the board dispatcher and runs Refresh() on a worker. This checks the
// request side of that contract (a cheap callback, not a blocking read) and the
// cancelled-request guard.
int RunAssistantBriefRefresh() {
    int failures = 0;
    const auto fail = [&failures](const wchar_t* what) {
        wprintf(L"[desk-test] %ls\n", what);
        ++failures;
    };

    std::unique_ptr<AssistantModule> card = CreateAssistantModule();
    int requested = 0;
    card->BindRefresh([&requested] { ++requested; });

    card->OnFocusChanged(true);
    if (requested != 1) fail(L"focus does not request a host BRIEF refresh");
    card->OnFocusChanged(false);
    if (requested != 1) fail(L"losing focus requests a BRIEF refresh");

    card->FinishAnswer(L"DSH SESSION 1", 10, 20, 30);
    if (requested != 2) fail(L"FinishAnswer does not request a host BRIEF refresh");

    // The BRIEF text itself arrives through the host's Refresh -> SetBrief path;
    // the card only stores and paints that snapshot.
    card->SetBrief(L"PROJECT A - LAST: BRIEF REFRESHED");
    if (card->ViewState().brief != L"PROJECT A - LAST: BRIEF REFRESHED") {
        fail(L"the card does not paint the brief the host published");
    }

    // The engine's BRIEF is one block per project (project / last conclusion /
    // blocker, '\n' separated). The card keeps those breaks, caps the block at
    // 6 lines and stays inside the Wide 720 contract.
    card->SetBrief(L"项目：A now\n最近结论：X\n卡点：Y\n"
                   L"项目：B 2h\n最近结论：Z\n卡点：未记录\n"
                   L"项目：C 5d\n最近结论：W\n卡点：未记录");
    const assistant_view::Layout briefLayout =
        assistant_view::ComputeLayout(card->ViewState(), true);
    if (briefLayout.briefLines != assistant_view::kExpandedMaxBriefLines) {
        fail(L"the multi-line brief is not capped at the expanded line budget");
    }
    if (assistant_view::BodyHeight(card->ViewState(), true) + wg::kCardChromeHeight >
        720) {
        fail(L"the multi-line brief leaves the Wide preset height");
    }
    if (assistant_view::ComputeLayout(card->ViewState(), false).briefLines !=
        assistant_view::kCollapsedBriefLines) {
        fail(L"the collapsed brief no longer reserves exactly one line");
    }

    // A cancelled request is a normal end: its late completion must not queue
    // another refresh (or a brief for an answer the card no longer owns).
    wg::KeyEvent enter;
    enter.vk = VK_RETURN;
    card->OnChar(L'q');
    card->OnKey(enter);
    if (requested != 2) fail(L"submitting a question refreshed the brief too early");
    wg::KeyEvent esc;
    esc.vk = VK_ESCAPE;
    card->OnKey(esc);
    card->FinishAnswer(L"", 0, 0, 0);
    if (requested != 2) fail(L"a cancelled answer requested another brief refresh");

    // Refresh() with no started engine (this card was never Load()ed) must be a
    // harmless no-op and is never the source of a new request.
    card->Refresh();
    if (requested != 2) fail(L"Refresh does not come from the host request path");

    return failures;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const wchar_t* path = argc > 1 ? argv[1] : L"build\\desk-board-preview.bmp";

    wg::Theme theme;
    const aimeter::AimeterData account = MakeAccount();
    const server::ServerData servers = MakeServers();

    const int terminalExpanded = wg::MeasureCardSize(
        wg::SizePreset::Wide, 568, ps_view::BodyHeight(true)).height;
    const int terminalCollapsed = wg::MeasureCardSize(
        wg::SizePreset::Wide, 568, ps_view::BodyHeight(false)).height;
    const int accountExpanded = wg::MeasureCardSize(
        wg::SizePreset::Standard, 340, account_view::BodyHeight(account, true)).height;
    const int serversExpanded = wg::MeasureCardSize(
        wg::SizePreset::Standard, 340, servers_view::BodyHeight(servers, true)).height;

    const assistant_view::AssistantViewState assistant = MakeAssistant();
    const assistant_view::AssistantViewState idleAssistant;   // first paint, before ask
    const assistant_view::Layout assistantLayout =
        assistant_view::ComputeLayout(assistant, true);
    const memory_view::MemoryViewState memory = MakeMemory();

    const int assistantCollapsed = wg::MeasureCardSize(
        wg::SizePreset::Wide, 568, assistant_view::BodyHeight(idleAssistant, false)).height;
    const int assistantExpanded = wg::MeasureCardSize(
        wg::SizePreset::Wide, 568, assistant_view::BodyHeight(assistant, true)).height;
    const int memoryCollapsed = wg::MeasureCardSize(
        wg::SizePreset::Standard, 340, memory_view::BodyHeight(memory, false)).height;
    const int memoryExpanded = wg::MeasureCardSize(
        wg::SizePreset::Standard, 340, memory_view::BodyHeight(memory, true)).height;

    // Layout the cards on the sheet. The terminal keeps its fixed grid width;
    // the assistant takes the Wide card; MEMORY the Standard one below it.
    const wg::Rect terminalCard{20, 500, ps_view::CardWidth(), terminalExpanded};
    const wg::Rect assistantCard{20, terminalCard.bottom() + 20, assistant_view::kCardWidth,
                                 assistantExpanded};
    const wg::Rect memoryCard{20, assistantCard.bottom() + 20, memory_view::kCardWidth,
                              memoryExpanded};
    const wg::Rect addSlot{20, memoryCard.bottom() + 20, 340, 56};
    const int canvasHeight = addSlot.bottom() + 20;

    if (accountExpanded != 456 || serversExpanded != 224) {
        wprintf(L"[desk-test] unexpected card heights %d/%d\n", accountExpanded,
                serversExpanded);
        return 1;
    }
    if (terminalExpanded != 392 || terminalCollapsed != 168) {
        wprintf(L"[desk-test] unexpected terminal height %d/%d\n", terminalCollapsed,
                terminalExpanded);
        return 1;
    }
    if (assistantCollapsed != 168) {
        wprintf(L"[desk-test] unexpected assistant collapsed height %d\n", assistantCollapsed);
        return 1;
    }
    if (assistantExpanded < 168 || assistantExpanded > 720) {
        wprintf(L"[desk-test] assistant height %d outside the Wide preset\n",
                assistantExpanded);
        return 1;
    }
    if (assistantLayout.answerLines < (int)assistant.answerLines.size()) {
        wprintf(L"[desk-test] assistant shows %d of %d answer lines\n",
                assistantLayout.answerLines, (int)assistant.answerLines.size());
        return 1;
    }
    if (assistantLayout.height + wg::kCardChromeHeight > assistantExpanded) {
        wprintf(L"[desk-test] assistant body %d clipped by card %d\n",
                assistantLayout.height, assistantExpanded);
        return 1;
    }
    if (memoryCollapsed < 112 || memoryExpanded > 640 || memoryCollapsed > memoryExpanded) {
        wprintf(L"[desk-test] unexpected memory heights %d/%d\n", memoryCollapsed,
                memoryExpanded);
        return 1;
    }
    if (memory_view::BodyHeight(memory, true) > 640 - wg::kCardChromeHeight) {
        wprintf(L"[desk-test] memory expanded body exceeds the Standard preset\n");
        return 1;
    }
    if (memory_view::VisibleRows(memory, false) != 3 ||
        memory_view::VisibleRows(memory, true) != (int)memory.facts.size()) {
        wprintf(L"[desk-test] memory row counts do not follow expand/collapse\n");
        return 1;
    }
    if (memory_view::RowAt(memory, memory_view::kTopInset + memory_view::kRowPitch + 3,
                           true) != 1) {
        wprintf(L"[desk-test] memory click row mapping is wrong\n");
        return 1;
    }

    if (ps_view::CardWidth() != 568) {
        wprintf(L"[desk-test] unexpected terminal width %d\n", ps_view::CardWidth());
        return 1;
    }

    const ps::View terminal = MakeView(ps::kCols, ps::kRowsExpanded, TerminalScript());
    const wg::Rect terminalBody{0, 0, ps_view::CardWidth(), ps_view::BodyHeight(true)};
    const auto lastCell = ps_view::HitTest(terminal, 4 + 79 * 7, 4, terminalBody);
    if (!lastCell.ok || lastCell.pos.col != 79 || ps_view::HitTest(terminal, 0, 0, terminalBody).ok) {
        wprintf(L"[desk-test] terminal hit-test does not preserve 80 columns\n");
        return 1;
    }
    ps::View scrolled = terminal;
    scrolled.scrollOffset = 2;
    if (!ps_view::BottomHit(scrolled, terminalBody.right() - 10, terminalBody.bottom() - 10,
                            terminalBody) ||
        ps_view::BottomHit(terminal, terminalBody.right() - 10, terminalBody.bottom() - 10,
                           terminalBody)) {
        wprintf(L"[desk-test] return-to-bottom hit-test failed\n");
        return 1;
    }

    // Select the two listing rows: the card should draw them washed in blue.
    ps::Selection selected;
    if (terminal.lines.size() > 3) {
        selected.active = true;
        selected.anchor = ps::Pos{terminal.lines[1].id, 0};
        selected.focus = ps::Pos{terminal.lines[2].id, 30};
    }

    wg::Canvas canvas;
    if (!canvas.Resize(760, canvasHeight)) return 1;
    canvas.Clear(0);
    wg::skin::BoardChrome(canvas, wg::Rect{0, 0, 760, canvasHeight}, L"DESKTOP BOARD",
                          L"RIGHT CLICK FOR MODULES", theme, true);

    wg::skin::AddSlot(canvas, addSlot, theme);

    DrawCard(canvas, wg::Rect{20, 32, 340, accountExpanded}, L"ACCOUNT",
             account_view::Status(account, theme).text,
             account_view::Status(account, theme).color, true, theme,
             [&](const wg::Rect& body) {
                 account_view::DrawBody(account, canvas, body, theme, true);
             });

    DrawCard(canvas, wg::Rect{380, 32, 340, serversExpanded}, L"SERVERS",
             servers_view::Status(servers, theme).text,
             servers_view::Status(servers, theme).color, true, theme,
             [&](const wg::Rect& body) {
                 servers_view::DrawBody(servers, canvas, body, theme, true);
             });

    DrawCard(canvas, terminalCard, L"POWERSHELL",
             ps_view::Status(true, true, L"PWSH 7.6", 0, theme).text,
             ps_view::Status(true, true, L"PWSH 7.6", 0, theme).color, true, theme,
             [&](const wg::Rect& body) {
                 ps_view::DrawBody(terminal, selected, true, true, canvas, body, theme);
             });

    const wg::StatusChip assistantChip = assistant_view::Status(assistant, theme);
    DrawCard(canvas, assistantCard, L"ASSISTANT", assistantChip.text, assistantChip.color,
             true, theme, [&](const wg::Rect& body) {
                 assistant_view::DrawBody(assistant, canvas, body, theme, true, true, true);
             });

    const wg::StatusChip memoryChip = memory_view::Status(memory, theme);
    DrawCard(canvas, memoryCard, L"MEMORY", memoryChip.text, memoryChip.color, true, theme,
             [&](const wg::Rect& body) {
                 memory_view::DrawBody(memory, canvas, body, theme, true);
             });

    // The first CJK answer line must be painted inside its own line box. The
    // old cjk::Text baseline path put a 14px mask above the box and clipped it
    // at the top of the text block; DrawBody uses cjk::TextTop for that reason.
    const wg::Color cardBg = theme.panelSolid;
    const wg::Rect assistantBody = wg::skin::CardBody(assistantCard);
    const int answerBoxTop = assistantBody.y + assistantLayout.answerTextTop;
    const int answerBoxLeft = assistantCard.x + assistant_view::kPadX;
    if (HasInk(canvas, answerBoxLeft, answerBoxTop - 2, assistant_view::kTextWidth, 2,
               cardBg)) {
        wprintf(L"[desk-test] CJK answer paints above its first line box\n");
        return 1;
    }
    if (!HasInk(canvas, answerBoxLeft, answerBoxTop, assistant_view::kTextWidth,
                assistant_view::kAnswerLineH, cardBg)) {
        wprintf(L"[desk-test] first CJK answer line is blank\n");
        return 1;
    }

    // The first MEMORY row (kind tag + fact text) must actually render.
    const wg::Rect memoryBody = wg::skin::CardBody(memoryCard);
    if (!HasInk(canvas, memoryCard.x + memory_view::kPadX,
                memoryBody.y + memory_view::kTopInset, memory_view::kTextWidth, 14,
                cardBg)) {
        wprintf(L"[desk-test] first MEMORY row is blank\n");
        return 1;
    }

    // ---- assistant failure: the error replaces the partial answer --------
    // A request that dies mid-stream leaves answerLines behind. Before the
    // fix VisibleAnswer returned those lines first, so the card painted the
    // truncated answer as valid prose and never showed the error text.
    {
        assistant_view::AssistantViewState failed = MakeAssistant();
        failed.state = assistant_view::State::Error;
        failed.error = L"PI NOT READY";
        const assistant_view::Layout failLayout =
            assistant_view::ComputeLayout(failed, true);
        const wg::Rect failBody{0, 0, assistant_view::kCardWidth, failLayout.height};
        wg::Canvas failCanvas;
        if (!failCanvas.Resize(failBody.w, failBody.h)) return 1;
        failCanvas.Clear(theme.panelSolid);
        assistant_view::DrawBody(failed, failCanvas, failBody, theme, true, false, false);
        const int failLeft = failBody.x + assistant_view::kPadX;
        if (!HasInk(failCanvas, failLeft, failLayout.answerTextTop,
                    assistant_view::kTextWidth, assistant_view::kAnswerLineH,
                    theme.panelSolid)) {
            wprintf(L"[desk-test] assistant error line is blank\n");
            return 1;
        }
        if (HasInk(failCanvas, failLeft,
                   failLayout.answerTextTop + assistant_view::kAnswerLineH,
                   assistant_view::kTextWidth, assistant_view::kAnswerLineH,
                   theme.panelSolid)) {
            wprintf(L"[desk-test] stale answer still painted under the error\n");
            return 1;
        }
        if (assistant_view::BodyHeight(failed, false) !=
            assistant_view::kCollapsedHeight) {
            wprintf(L"[desk-test] assistant error state changed the collapsed height\n");
            return 1;
        }
    }

    // ---- assistant cancel: a late delta must not revive the request -------
    // Esc puts the card back to IDLE; the engine may still flush buffered
    // tokens. Without a guard the chip flipped to THINKING again and stayed
    // there, because the cancelled request never delivers onDone.
    {
        std::unique_ptr<AssistantModule> card = CreateAssistantModule();
        assistant_view::AssistantViewState thinking = MakeAssistant();
        thinking.state = assistant_view::State::Thinking;
        thinking.answerLines = {L"PARTIAL"};
        card->SetViewState(thinking);

        wg::KeyEvent esc;
        esc.vk = VK_ESCAPE;
        if (!card->OnKey(esc)) return 1;
        const std::vector<std::wstring> before = card->ViewState().answerLines;
        card->AppendDelta(L" LATE");
        card->SetError(L"CANCELLED");
        const assistant_view::AssistantViewState after = card->ViewState();
        if (after.state != assistant_view::State::Idle) {
            wprintf(L"[desk-test] late delta revived a cancelled request\n");
            return 1;
        }
        if (after.answerLines != before) {
            wprintf(L"[desk-test] late delta mutated the cancelled answer\n");
            return 1;
        }

        // The next question must stream normally: the guard is per request.
        card->OnChar(L'?');
        wg::KeyEvent enter;
        enter.vk = VK_RETURN;
        if (!card->OnKey(enter)) return 1;
        card->AppendDelta(L"FRESH");
        const assistant_view::AssistantViewState fresh = card->ViewState();
        if (fresh.state != assistant_view::State::Thinking ||
            fresh.answerLines != std::vector<std::wstring>{L"FRESH"}) {
            wprintf(L"[desk-test] the card no longer streams after a cancel\n");
            return 1;
        }
    }

    // ---- MEMORY card wiring against a synthetic shared store -------------
    // Runs after the fixed render snapshot so the preview keeps its fixtures;
    // the card test uses its own scratch directory and facts file only.
    if (RunMemoryCardWiring() != 0) return 1;
    if (RunMemoryEditClearWiring() != 0) return 1;
    if (RunMemoryReplanWiring() != 0) return 1;
    if (RunAssistantBriefRefresh() != 0) return 1;

    if (!test::WriteBmp(path, canvas, wg::Rgba(0x101820))) {
        wprintf(L"[desk-test] cannot write %ls\n", path);
        return 1;
    }

    wprintf(L"[desk-test] account=%d servers=%d terminal=%d/%d width=%d\n", accountExpanded,
            serversExpanded, terminalCollapsed, terminalExpanded, ps_view::CardWidth());
    wprintf(L"[desk-test] assistant=%d/%d memory=%d/%d answerTop=%d\n", assistantCollapsed,
            assistantExpanded, memoryCollapsed, memoryExpanded, assistantLayout.answerTextTop);
    wprintf(L"[desk-test] wrote %ls (%dx%d)\n", path, canvas.width(), canvas.height());
    return 0;
}
