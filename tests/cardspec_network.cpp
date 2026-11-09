// Headless M2.1 regression for the CARDSPEC HTTP path.
//
// No socket, no window, no DSH/PI, no real board: a private synthetic config
// directory under the test exe, a wg::Store bound exactly the way the board
// binds one, and an injected fake transport. The production transport
// (DefaultHttpDependencies/FetchHttp) is never invoked here: every module is
// built through CreateCardSpecModule(CardSpecOptions{...}) so an empty or
// fake-only fetch stays fail-closed.
//
// Covers: initial Load is zero-network even when approved; missing/invalid/
// changed approval is Blocked with zero fetch; approve cancel changes nothing;
// approve persists the exact profile+URL binding and then fetches once; v2
// rows/bar bind and paint with an "AS OF" line and the HTTP chip; non-200,
// oversized, bad JSON and missing-binding responses become HTTP errors that
// clear the previous values; a fixed endpoint recovers; revoke persists and
// blocks; an in-flight result is discarded after a revoke; a failed approval
// save rolls back; a failed revoke save still blocks the process.
#include "data/cardspec/cardspec.h"
#include "data/cardspec/http.h"
#include "data/cardspec/http_source.h"
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
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wprintf(L"[cardspec-network-test] FAIL %ls\n", message);
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
// that a state painted only inside the body rectangle it was handed.
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

// JSON fixtures are encoded at runtime so the test does not depend on the
// compiler's narrow execution charset.
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

std::string Narrow(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (const wchar_t c : text) {
        if (c < 0x20 || c > 0x7E) continue;
        out.push_back((char)c);
    }
    return out;
}

// v1 stays available with the unchanged inline schema.
std::string V1RowsJson() {
    return "{\"version\":1,\"title\":" + JsonString(L"本地") +
           ",\"source\":{\"type\":\"inline\"},\"layout\":\"rows\",\"rows\":["
           "{\"label\":" + JsonString(L"学习") + ",\"text\":" +
           JsonString(L"阅读三十分钟") + "}]}";
}

std::string HttpRowsJson() {
    return "{\"version\":2,\"title\":" + JsonString(L"公网仓库") +
           ",\"source\":{\"type\":\"http\",\"profile\":\"demo\"},"
           "\"layout\":\"rows\",\"rows\":["
           "{\"label\":" + JsonString(L"名称") + ",\"text_path\":\"/full_name\"},"
           "{\"label\":" + JsonString(L"星标") + ",\"text_path\":\"/stargazers_count\"}]}";
}

std::string HttpBarJson() {
    return "{\"version\":2,\"title\":" + JsonString(L"进度") +
           ",\"source\":{\"type\":\"http\",\"profile\":\"demo\"},"
           "\"layout\":\"bar\",\"rows\":["
           "{\"label\":" + JsonString(L"完成") + ",\"value_path\":\"/done\",\"max\":10},"
           "{\"label\":" + JsonString(L"剩余") + ",\"value_path\":\"/left\","
           "\"max_path\":\"/total\"}]}";
}

std::string RowsBody() {
    return "{\"full_name\":" + JsonString(L"microsoft/Windows-classic-samples") +
           ",\"stargazers_count\":12345}";
}

std::string BarBody() { return "{\"done\":4,\"left\":6,\"total\":10}"; }

// Trusted endpoint used by the fixtures. Public DNS host, https 443, no auth.
const wchar_t* const kUrl =
    L"https://api.github.com/repos/microsoft/Windows-classic-samples";
const wchar_t* const kHost = L"api.github.com";

cardspec::HttpResult OkBody(const std::string& body) {
    cardspec::HttpResult result;
    result.failure = cardspec::HttpFailure::None;
    result.status = 200;
    result.body = body;
    return result;
}

cardspec::HttpResult StatusOnly(unsigned status) {
    cardspec::HttpResult result;
    result.failure = cardspec::HttpFailure::None;
    result.status = status;
    return result;
}

cardspec::HttpResult TransportFailure() {
    cardspec::HttpResult result;  // default failure = Transport
    return result;
}

struct FakeFetch {
    int calls = 0;
    std::string lastProfile;
    cardspec::HttpPolicy lastPolicy;
    std::function<cardspec::HttpResult()> next;

    cardspec::HttpResult operator()(std::string_view profile,
                                    const cardspec::HttpPolicy& policy) {
        ++calls;
        lastProfile.assign(profile);
        lastPolicy = policy;
        if (next) return next();
        return cardspec::HttpResult{};  // never an accidental fake success
    }
};

cardspec::HttpPolicy PolicyOf(const wg::Store& store) {
    cardspec::HttpPolicy policy;
    policy.profile = Narrow(store.Get(L"http.profile"));
    policy.url = store.Get(L"http.url");
    policy.approvedProfile = Narrow(store.Get(L"http.approved_profile"));
    policy.approvedUrl = store.Get(L"http.approved_url");
    return policy;
}

// Approved policy whose approved URL is the *canonical* form of `url`, exactly
// what the card saves after approval.
cardspec::HttpPolicy ApprovedPolicy(const std::wstring& url, const char* profile = "demo") {
    cardspec::HttpPolicy policy;
    cardspec::HttpEndpoint endpoint;
    if (!cardspec::ParseHttpEndpoint(url, &endpoint)) return policy;
    policy.profile = profile;
    policy.url = url;
    policy.approvedProfile = profile;
    policy.approvedUrl = endpoint.url;
    return policy;
}

cardspec_view::LoadOptions Options(const cardspec::HttpPolicy& policy, FakeFetch* fake,
                                   bool allowNetwork) {
    cardspec_view::LoadOptions options;
    options.allowNetwork = allowNetwork;
    options.policy = policy;
    if (fake) {
        options.fetch = [fake](std::string_view profile, const cardspec::HttpPolicy& p) {
            return (*fake)(profile, p);
        };
    }
    return options;
}

int PaintState(const cardspec_view::ViewState& state, wg::Canvas& canvas, int width,
               const wg::Theme& theme) {
    const int height = std::max(1, cardspec_view::BodyHeight(state));
    if (!canvas.Resize(width, height)) return 0;
    canvas.Clear(theme.panelSolid);
    cardspec_view::DrawBody(state, canvas, wg::Rect{0, 0, width, height}, theme);
    return height;
}

int PaintCard(wg::Module& module, wg::Canvas& canvas, int width, const wg::Theme& theme) {
    const int height = std::max(1, module.BodyHeight());
    if (!canvas.Resize(width, height)) return 0;
    canvas.Clear(theme.panelSolid);
    module.PaintBody(canvas, wg::Rect{0, 0, width, height});
    return height;
}

// Chip assertion with a payload-free diagnostic: only the chip text and the two
// heights are printed on a mismatch (never rows, URL, path or store contents).
void CheckChip(const wg::Module& module, const wchar_t* expected, const wchar_t* message) {
    const wg::StatusChip chip = module.Status();
    if (chip.text == expected) return;
    std::wprintf(L"[cardspec-network-test] FAIL %ls (chip=%ls body=%d pending=%d)\n",
                 message, chip.text.c_str(), module.BodyHeight(),
                 module.PendingBodyHeight());
    ++failures;
}

} // namespace

int wmain() {
    using namespace wg;

    const Theme theme;
    const Color background = theme.panelSolid;
    const int width = cardspec_view::kCardWidth;
    const int left = cardspec_view::kPadX;
    const int rows2H = cardspec_view::kTitleBlock + 2 * cardspec_view::kRowPitch;
    const int asOfH = rows2H + cardspec_view::kAsOfGap + cardspec_view::kLineH;
    // A two-row card paints row 2's text here; an error card is shorter, so an
    // empty band is the proof that no stale values survived a failure.
    const int secondRowTop = cardspec_view::kTitleBlock + cardspec_view::kRowPitch + 18;
    const int errorNoDetailH = cardspec_view::kLineH + 20 + 20;
    const int errorWithDetailH = cardspec_view::kLineH + 20 + 40;

    const std::wstring root = ExeDir() + L"\\cardspec-network-test";
    const std::wstring configPath = root + L"\\cardspec.ini";
    const std::wstring specPath = root + L"\\cardspec.json";
    RemoveTree(root);
    if (!CreateDirectoryW(root.c_str(), nullptr)) {
        std::wprintf(L"[cardspec-network-test] FAIL cannot create %s\n", root.c_str());
        return 1;
    }
    Check(WriteBytes(specPath, HttpRowsJson()), L"write the v2 rows fixture");

    const cardspec::HttpPolicy approved = ApprovedPolicy(kUrl);
    Check(approved.approvedProfile == "demo" && !approved.approvedUrl.empty(),
          L"the fixture endpoint canonicalizes");

    // ---- no approval: Blocked, zero fetch ---------------------------------
    for (const bool allowNetwork : {false, true}) {
        FakeFetch fake;
        cardspec::HttpPolicy unapproved = approved;
        unapproved.approvedProfile.clear();
        unapproved.approvedUrl.clear();
        const cardspec_view::ViewState state =
            cardspec_view::LoadSpecFile(specPath, Options(unapproved, &fake, allowNetwork));
        Check(state.state == cardspec_view::State::Blocked, L"no approval is Blocked");
        Check(state.isHttp, L"a v2 document is marked as HTTP");
        Check(state.request.profile == "demo" && state.request.rows.size() == 2 &&
                  state.request.rows[0].textPointer == "/full_name",
              L"the request keeps the profile and bindings");
        Check(state.asOf.empty(), L"Blocked has no as-of caption");
        Check(fake.calls == 0, L"Blocked never fetches");
        Check(state.message.find(L"https://") == std::wstring::npos &&
                  state.message.find(kHost) == std::wstring::npos &&
                  state.detail.find(L"https://") == std::wstring::npos,
              L"the blocked card never prints the URL");
        Check(cardspec_view::Status(state, theme).text == L"BLOCKED", L"Blocked chip");
    }
    {
        // Missing config entirely.
        FakeFetch fake;
        const cardspec_view::ViewState state =
            cardspec_view::LoadSpecFile(specPath, Options(cardspec::HttpPolicy{}, &fake, true));
        Check(state.state == cardspec_view::State::Blocked && fake.calls == 0,
              L"an unconfigured endpoint is Blocked with zero fetch");
        Check(state.detail.find(L"http.profile") != std::wstring::npos,
              L"the blocked card says which keys are missing");
    }
    {
        // Approved profile / URL must match exactly.
        FakeFetch fake;
        cardspec::HttpPolicy wrongProfile = approved;
        wrongProfile.approvedProfile = "other";
        Check(cardspec_view::LoadSpecFile(specPath, Options(wrongProfile, &fake, true)).state ==
                  cardspec_view::State::Blocked && fake.calls == 0,
              L"a different approved profile is Blocked");

        cardspec::HttpPolicy wrongUrl = approved;
        wrongUrl.approvedUrl += L"/other";
        Check(cardspec_view::LoadSpecFile(specPath, Options(wrongUrl, &fake, true)).state ==
                  cardspec_view::State::Blocked && fake.calls == 0,
              L"a changed approved URL is Blocked");

        cardspec::HttpPolicy badUrl = approved;
        badUrl.url = L"http://127.0.0.1/spec";
        badUrl.approvedUrl.clear();
        badUrl.approvedProfile.clear();
        Check(cardspec_view::LoadSpecFile(specPath, Options(badUrl, &fake, true)).state ==
                  cardspec_view::State::Blocked && fake.calls == 0,
              L"a non-HTTPS/IP endpoint is Blocked");
        Check(fake.calls == 0, L"no invalid configuration fetches");
    }

    // ---- authorized but the initial Load must stay offline ----------------
    {
        FakeFetch fake;
        fake.next = [&] { return OkBody(RowsBody()); };
        const cardspec_view::ViewState state =
            cardspec_view::LoadSpecFile(specPath, Options(approved, &fake, false));
        Check(state.state == cardspec_view::State::Ready, L"authorized + no network is Ready");
        Check(fake.calls == 0, L"an approved initial Load never fetches");
        Check(cardspec_view::Status(state, theme).text == L"READY", L"Ready chip");
        Check(cardspec_view::BodyHeight(state) == errorWithDetailH,
              L"the Ready card uses the error layout");
    }

    // ---- authorized + allowed: one GET, bound rows, HTTP + as-of -----------
    {
        FakeFetch fake;
        fake.next = [&] { return OkBody(RowsBody()); };
        const cardspec_view::ViewState state =
            cardspec_view::LoadSpecFile(specPath, Options(approved, &fake, true));
        Check(fake.calls == 1, L"an authorized refresh fetches exactly once");
        Check(fake.lastProfile == "demo", L"the fetch sees the trusted profile");
        Check(fake.lastPolicy.url == kUrl, L"the fetch sees the trusted config");
        Check(state.state == cardspec_view::State::Ok && state.isHttp, L"HTTP rows are Ok");
        Check(!state.asOf.empty(), L"a successful fetch captures the as-of caption");
        Check(state.spec.rows.size() == 2, L"two bound rows");
        Check(cardspec_view::Status(state, theme).text == L"HTTP", L"HTTP chip");
        Check(cardspec_view::BodyHeight(state) == asOfH,
              L"the as-of line is part of the content height");

        wg::Canvas canvas;
        Check(PaintState(state, canvas, width, theme) == asOfH, L"paint height matches");
        Check(HasInk(canvas, left, cardspec_view::kTitleBlock + 18, 300,
                     cardspec_view::kLineH, background),
              L"the bound row text is painted");
        Check(HasInk(canvas, left, secondRowTop, 300, cardspec_view::kLineH, background),
              L"the second bound row is painted");
        Check(HasInk(canvas, left, rows2H + cardspec_view::kAsOfGap, 200,
                     cardspec_view::kLineH, background),
              L"the AS OF line is painted");
    }
    {
        // The new AS OF line obeys the same tiny-body bounds as every other
        // line: an elided marker inside the body, never outside it.
        FakeFetch fake;
        fake.next = [&] { return OkBody(RowsBody()); };
        const cardspec_view::ViewState state =
            cardspec_view::LoadSpecFile(specPath, Options(approved, &fake, true));
        Check(state.state == cardspec_view::State::Ok && !state.asOf.empty(),
              L"tiny-body HTTP fixture is a success");
        wg::Canvas canvas;
        Check(canvas.Resize(120, 120), L"tiny-body canvas");
        canvas.Clear(background);
        const Rect small{20, 30, 60, 160};
        cardspec_view::DrawBody(state, canvas, small, theme);
        Check(!HasInkOutside(canvas, small, background),
              L"the as-of line stays inside the body rectangle");
    }
    {
        // Bar layout with a literal max and a max_path.
        Check(WriteBytes(specPath, HttpBarJson()), L"write the v2 bar fixture");
        FakeFetch fake;
        fake.next = [&] { return OkBody(BarBody()); };
        const cardspec_view::ViewState state =
            cardspec_view::LoadSpecFile(specPath, Options(approved, &fake, true));
        Check(state.state == cardspec_view::State::Ok && state.spec.layout == cardspec::Layout::Bar,
              L"the bar document binds");
        Check(state.spec.rows.size() == 2 && state.spec.rows[0].value == 4.0 &&
                  state.spec.rows[0].max == 10.0 && state.spec.rows[1].value == 6.0 &&
                  state.spec.rows[1].max == 10.0,
              L"bar values come from value_path/max and max_path");
        Check(cardspec_view::BodyHeight(state) == asOfH, L"bar height includes as-of");
        wg::Canvas canvas;
        PaintState(state, canvas, width, theme);
        const int barTop = cardspec_view::kTitleBlock + cardspec_view::kBarOffsetY;
        Check(PixelIs(canvas, left + 50, barTop + 2, theme.ok), L"4/10 bar is 40% filled");
        Check(PixelIs(canvas, left + 500, barTop + 2, theme.track), L"the rest is track");
    }

    // ---- failures replace values instead of faking freshness --------------
    {
        Check(WriteBytes(specPath, HttpRowsJson()), L"restore the rows fixture");
        FakeFetch fake;
        fake.next = [&] { return StatusOnly(500); };
        const cardspec_view::ViewState state =
            cardspec_view::LoadSpecFile(specPath, Options(approved, &fake, true));
        Check(state.state == cardspec_view::State::HttpError, L"non-200 is a HTTP error");
        Check(state.asOf.empty() && state.spec.rows.empty(), L"no old data or as-of survives");
        Check(state.request.profile == "demo", L"the request survives for the menu");
        Check(cardspec_view::BodyHeight(state) == errorNoDetailH, L"error card height");
        Check(cardspec_view::Status(state, theme).text == L"HTTP ERR", L"HTTP ERR chip");
        Check(state.message.find(L"500") != std::wstring::npos, L"the status code is shown");
        wg::Canvas canvas;
        PaintState(state, canvas, width, theme);
        Check(!HasInk(canvas, left, secondRowTop, 300, cardspec_view::kLineH, background),
              L"the old row band is empty after a failure");
    }
    {
        FakeFetch fake;
        fake.next = [&] { return OkBody(std::string(cardspec::kMaxHttpBodyBytes + 1, 'x')); };
        const cardspec_view::ViewState state =
            cardspec_view::LoadSpecFile(specPath, Options(approved, &fake, true));
        Check(state.state == cardspec_view::State::HttpError && state.spec.rows.empty(),
              L"an oversized body is rejected");
        Check(state.message.find(L"32 KiB") != std::wstring::npos,
              L"the oversized message names the limit");
    }
    {
        FakeFetch fake;
        fake.next = [&] { return OkBody("{oops"); };
        const cardspec_view::ViewState state =
            cardspec_view::LoadSpecFile(specPath, Options(approved, &fake, true));
        Check(state.state == cardspec_view::State::HttpError, L"bad JSON is a HTTP error");
        Check(!state.detail.empty(), L"the JSON failure keeps a readable detail");
        Check(cardspec_view::BodyHeight(state) == errorWithDetailH, L"detail adds one line");
    }
    {
        FakeFetch fake;
        fake.next = [&] { return OkBody("{\"other\":1}"); };
        const cardspec_view::ViewState state =
            cardspec_view::LoadSpecFile(specPath, Options(approved, &fake, true));
        Check(state.state == cardspec_view::State::HttpError, L"a missing binding fails");
        Check(state.message.find(L"字段绑定") != std::wstring::npos,
              L"the binding failure is readable");
    }
    {
        FakeFetch fake;
        fake.next = [&] { return TransportFailure(); };
        const cardspec_view::ViewState state =
            cardspec_view::LoadSpecFile(specPath, Options(approved, &fake, true));
        Check(state.state == cardspec_view::State::HttpError, L"a transport failure is an error");
        Check(!state.message.empty() && state.message.find(L"https://") == std::wstring::npos &&
                  state.message.find(kHost) == std::wstring::npos,
              L"a transport failure never echoes the URL");
    }
    {
        // No injected transport: fail closed, no fetch, readable error.
        const cardspec_view::ViewState state =
            cardspec_view::LoadSpecFile(specPath, Options(approved, nullptr, true));
        Check(state.state == cardspec_view::State::HttpError, L"an empty fetch is an error");
        Check(state.message.find(L"未配置网络依赖") != std::wstring::npos,
              L"the empty fetch fails closed");
    }
    {
        // Recovery: the same approved endpoint never stays broken.
        FakeFetch fake;
        fake.next = [&] { return OkBody(RowsBody()); };
        const cardspec_view::ViewState state =
            cardspec_view::LoadSpecFile(specPath, Options(approved, &fake, true));
        Check(state.state == cardspec_view::State::Ok && !state.asOf.empty(),
              L"a fixed endpoint recovers");
    }

    // ---- v1 is untouched by the HTTP path --------------------------------
    {
        const std::wstring v1Path = root + L"\\v1.json";
        Check(WriteBytes(v1Path, V1RowsJson()), L"write an inline v1 fixture");
        FakeFetch fake;
        const cardspec_view::ViewState state =
            cardspec_view::LoadSpecFile(v1Path, Options(approved, &fake, true));
        Check(state.state == cardspec_view::State::Ok && !state.isHttp, L"v1 stays inline");
        Check(fake.calls == 0, L"v1 never fetches even when allowed");
        Check(cardspec_view::Status(state, theme).text == L"LIVE", L"v1 keeps the LIVE chip");
        Check(cardspec_view::BodyHeight(state) ==
                  cardspec_view::kTitleBlock + cardspec_view::kRowPitch,
              L"v1 height has no as-of line");
    }

    // ---- native module: approve / persist / fetch / clear / recover -------
    {
        DeleteFileW(configPath.c_str());
        DeleteFileW((configPath + L".tmp").c_str());
        wg::Store store;
        store.Open(configPath);  // absent: the path is still recorded
        store.Set(L"http.profile", L"demo");
        store.Set(L"http.url", kUrl);
        Check(WriteBytes(specPath, HttpRowsJson()), L"restore the rows fixture");

        FakeFetch fake;
        fake.next = [&] { return OkBody(RowsBody()); };
        std::vector<std::wstring> prompts;
        bool confirmAnswer = false;
        int refreshes = 0;

        CardSpecOptions options;
        options.fetch = [&fake](std::string_view profile, const cardspec::HttpPolicy& policy) {
            return fake(profile, policy);
        };
        options.confirmNetwork = [&](const std::wstring& prompt) {
            prompts.push_back(prompt);
            return confirmAnswer;
        };
        std::unique_ptr<Module> module = CreateCardSpecModule(std::move(options));
        Check(module != nullptr, L"the options factory returns a module");
        module->Bind(&store, [] {});
        module->BindRefresh([&refreshes] { ++refreshes; });

        module->Load();
        Check(fake.calls == 0, L"module Load is zero-network");
        // This fixture is deliberately *unapproved* (only http.profile/http.url
        // are configured, no approved_* keys), so the first Load must land in
        // Blocked - not Ready. "Approved Load -> Ready, still zero fetch" is
        // covered by the approved fixtures below (revoke/window blocks).
        CheckChip(*module, L"BLOCKED", L"an unapproved HTTP card loads as Blocked");
        Check(module->BodyHeight() == errorWithDetailH,
              L"the Blocked card uses the error layout");

        const std::vector<MenuItem> before = module->Menu();
        Check(before.size() == 2 && before[0].id == 1 && before[1].id == 2 &&
                  before[1].label.find(L"Approve") == 0,
              L"an unapproved HTTP card offers Approve");

        confirmAnswer = false;
        module->OnMenu(2);
        Check(prompts.size() == 1, L"approve asks for confirmation");
        // The prompt shows the exact canonical URL from the trusted config (the
        // same form the card will persist), never anything from the spec.
        Check(prompts[0].find(approved.approvedUrl) != std::wstring::npos,
              L"the prompt shows the exact trusted URL");
        Check(prompts[0].find(kHost) != std::wstring::npos &&
                  prompts[0].find(L"https://") != std::wstring::npos,
              L"the prompt includes the scheme and host");
        Check(prompts[0].find(L"32 KiB") != std::wstring::npos,
              L"the prompt states the response limit");
        Check(store.Get(L"http.approved_profile").empty() &&
                  store.Get(L"http.approved_url").empty(),
              L"a cancelled approve writes nothing");
        Check(fake.calls == 0 && refreshes == 0, L"a cancelled approve fetches nothing");

        confirmAnswer = true;
        module->OnMenu(2);
        Check(store.Get(L"http.approved_profile") == L"demo", L"approve saves the profile");
        const std::wstring approvedUrl = store.Get(L"http.approved_url");
        Check(!approvedUrl.empty() && approvedUrl.find(L"https://") == 0,
              L"approve saves the canonical URL");
        Check(store.Get(L"http.url") == kUrl, L"approve never rewrites the user endpoint");
        Check(refreshes == 1, L"approve asks the host for exactly one refresh");

        {
            wg::Store saved;
            saved.Open(configPath);
            Check(saved.Get(L"http.approved_profile") == L"demo" &&
                      saved.Get(L"http.approved_url") == approvedUrl,
                  L"the approval is persisted to cardspec.ini");
            Check(cardspec_view::LoadSpecFile(specPath, Options(PolicyOf(saved), nullptr, false))
                          .state == cardspec_view::State::Ready,
                  L"the persisted approval authorizes the same endpoint");
        }

        module->Refresh();
        Check(fake.calls == 1, L"the refresh performs one GET");
        Check(fake.lastPolicy.profile == "demo" && fake.lastPolicy.url == kUrl,
              L"the fetch uses the trusted config");
        Check(module->PendingBodyHeight() == asOfH, L"pending height includes the as-of line");
        Check(module->BodyHeight() == errorWithDetailH,
              L"shown stays on the pre-commit card (Blocked layout)");
        module->Commit();
        CheckChip(*module, L"HTTP", L"a fetched HTTP card reports HTTP");
        Check(module->BodyHeight() == asOfH, L"the committed HTTP card is taller by as-of");

        wg::Canvas canvas;
        PaintCard(*module, canvas, width, theme);
        Check(HasInk(canvas, left, cardspec_view::kTitleBlock + 18, 300,
                     cardspec_view::kLineH, background),
              L"the fetched rows are painted");
        Check(HasInk(canvas, left, rows2H + cardspec_view::kAsOfGap, 200,
                     cardspec_view::kLineH, background),
              L"the as-of caption is painted");

        const std::vector<MenuItem> afterFetch = module->Menu();
        Check(afterFetch.size() == 2 && afterFetch[1].id == 3 &&
                  afterFetch[1].label.find(L"Revoke") == 0,
              L"an approved HTTP card offers Revoke");

        // A later failure clears the old rows (no fake fresh data).
        fake.next = [&] { return StatusOnly(500); };
        module->Refresh();
        module->Commit();
        CheckChip(*module, L"HTTP ERR", L"a failed refresh reports HTTP ERR");
        Check(module->BodyHeight() == errorNoDetailH, L"the failed card is an error card");
        wg::Canvas failed;
        PaintCard(*module, failed, width, theme);
        Check(!HasInk(failed, left, secondRowTop, 300, cardspec_view::kLineH, background),
              L"the old rows are gone after a failure");
        Check(!HasInk(failed, left, rows2H + cardspec_view::kAsOfGap, 200,
                      cardspec_view::kLineH, background),
              L"the old as-of caption is gone after a failure");

        fake.next = [&] { return OkBody(RowsBody()); };
        module->Refresh();
        module->Commit();
        CheckChip(*module, L"HTTP", L"a recovered endpoint reports HTTP again");
        wg::Canvas recovered;
        PaintCard(*module, recovered, width, theme);
        Check(HasInk(recovered, left, cardspec_view::kTitleBlock + 18, 300,
                     cardspec_view::kLineH, background),
              L"recovery paints the rows again");
    }

    // ---- revoke: persisted and blocking -----------------------------------
    {
        wg::Store store;
        store.Open(configPath);
        Check(store.Get(L"http.approved_profile") == L"demo", L"approval is on disk");
        FakeFetch fake;
        fake.next = [&] { return OkBody(RowsBody()); };
        int refreshes = 0;
        CardSpecOptions options;
        options.fetch = [&fake](std::string_view profile, const cardspec::HttpPolicy& policy) {
            return fake(profile, policy);
        };
        options.confirmNetwork = [](const std::wstring&) { return true; };
        std::unique_ptr<Module> module = CreateCardSpecModule(std::move(options));
        module->Bind(&store, [] {});
        module->BindRefresh([&refreshes] { ++refreshes; });

        module->Load();
        CheckChip(*module, L"READY", L"an approved Load reads local state only");
        Check(fake.calls == 0, L"an approved Load stays zero-network");

        module->OnMenu(3);
        Check(store.Get(L"http.approved_profile").empty() &&
                  store.Get(L"http.approved_url").empty(),
              L"revoke clears the in-memory approval");
        Check(refreshes == 1, L"revoke requests a refresh");
        module->Refresh();
        module->Commit();
        CheckChip(*module, L"BLOCKED", L"a revoked card is Blocked");
        Check(fake.calls == 0, L"a revoked card never fetches");
        {
            wg::Store saved;
            saved.Open(configPath);
            Check(saved.Get(L"http.approved_profile").empty(),
                  L"revoke is persisted to cardspec.ini");
            Check(saved.Get(L"http.url") == kUrl, L"revoke keeps the user endpoint");
        }
    }

    // ---- an in-flight result is discarded after a revoke ------------------
    {
        wg::Store store;
        store.Open(configPath);
        store.Set(L"http.approved_profile", L"demo");
        store.Set(L"http.approved_url", approved.approvedUrl);
        Check(WriteBytes(specPath, HttpRowsJson()), L"restore the rows fixture");

        FakeFetch fake;
        int refreshes = 0;
        std::unique_ptr<Module> module;
        fake.next = [&] {
            module->OnMenu(3);  // revoke lands while the request is in flight
            return OkBody(RowsBody());
        };
        CardSpecOptions options;
        options.fetch = [&fake](std::string_view profile, const cardspec::HttpPolicy& policy) {
            return fake(profile, policy);
        };
        options.confirmNetwork = [](const std::wstring&) { return true; };
        module = CreateCardSpecModule(std::move(options));
        module->Bind(&store, [] {});
        module->BindRefresh([&refreshes] { ++refreshes; });

        module->Load();
        CheckChip(*module, L"READY", L"the epoch fixture starts approved and offline");
        Check(fake.calls == 0, L"the epoch fixture stays zero-network");
        module->Refresh();
        Check(fake.calls == 1, L"the in-flight request was sent once");
        Check(refreshes == 1, L"the in-flight revoke requested a refresh");
        module->Commit();
        CheckChip(*module, L"BLOCKED",
                  L"a stale in-flight result is never published as HTTP");
        Check(module->BodyHeight() == errorWithDetailH, L"the discarded card is a Blocked card");
        // Status + height already prove the stale rows were not published; the
        // row-2 band is outside a Blocked card, so it must stay blank.
        wg::Canvas canvas;
        PaintCard(*module, canvas, width, theme);
        Check(!HasInk(canvas, left, secondRowTop, 300, cardspec_view::kLineH, background),
              L"the discarded rows are not painted");

        module->Refresh();
        module->Commit();
        Check(fake.calls == 1, L"a revoked card issues no further request");
        CheckChip(*module, L"BLOCKED", L"the card stays Blocked");
    }

    // ---- revoke between Refresh returning and the host's Commit -----------
    // The reviewed window: the worker already produced a good Ok state, the UI
    // revokes before Commit. Commit still publishes that (now stale) view into
    // shown, so Status/BodyHeight/Paint must refuse it on their own.
    {
        wg::Store store;
        store.Open(configPath);
        store.Set(L"http.profile", L"demo");
        store.Set(L"http.url", kUrl);
        store.Set(L"http.approved_profile", L"demo");
        store.Set(L"http.approved_url", approved.approvedUrl);
        Check(WriteBytes(specPath, HttpRowsJson()), L"restore the rows fixture");

        FakeFetch fake;
        fake.next = [&] { return OkBody(RowsBody()); };
        int refreshes = 0;
        CardSpecOptions options;
        options.fetch = [&fake](std::string_view profile, const cardspec::HttpPolicy& policy) {
            return fake(profile, policy);
        };
        options.confirmNetwork = [](const std::wstring&) { return true; };
        std::unique_ptr<Module> module = CreateCardSpecModule(std::move(options));
        module->Bind(&store, [] {});
        module->BindRefresh([&refreshes] { ++refreshes; });

        module->Load();
        CheckChip(*module, L"READY", L"the window fixture starts approved and offline");
        Check(fake.calls == 0, L"the window fixture stays zero-network");

        module->Refresh();  // the worker produced a fresh Ok state...
        Check(fake.calls == 1, L"the worker fetched once");
        module->OnMenu(3);  // ...and the user revokes before Commit.
        Check(refreshes == 1, L"the revoke requests a refresh");
        Check(module->Menu().size() == 2 && module->Menu()[1].id == 2,
              L"the menu offers Approve right after a revoke (no wait for Refresh)");

        module->Commit();
        CheckChip(*module, L"BLOCKED",
                  L"a stale committed result is never reported as HTTP");
        Check(module->BodyHeight() == errorWithDetailH,
              L"a stale committed result uses the expired height");
        Check(module->PendingBodyHeight() == errorWithDetailH,
              L"the pending height is expired as well");
        wg::Canvas stale;
        PaintCard(*module, stale, width, theme);
        Check(!HasInk(stale, left, secondRowTop, 300, cardspec_view::kLineH, background),
              L"no stale row is painted after the revoke");
        Check(!HasInk(stale, left, rows2H + cardspec_view::kAsOfGap, 200,
                      cardspec_view::kLineH, background),
              L"no stale as-of caption is painted after the revoke");

        // Re-approving must not revive the discarded result: it needs a new GET.
        const int callsBefore = fake.calls;
        module->OnMenu(2);
        Check(store.Get(L"http.approved_profile") == L"demo", L"approval can be granted again");
        Check(fake.calls == callsBefore, L"re-approval performs no fetch by itself");
        Check(module->Menu().size() == 2 && module->Menu()[1].id == 3,
              L"the menu offers Revoke again after re-approval");
        CheckChip(*module, L"BLOCKED",
                  L"the old result stays expired after re-approval");
        Check(module->BodyHeight() == errorWithDetailH,
              L"re-approval does not resurrect the old height");

        module->Refresh();
        Check(fake.calls == callsBefore + 1, L"a new approval needs a new fetch");
        module->Commit();
        CheckChip(*module, L"HTTP", L"the new result is published as HTTP");
        Check(module->BodyHeight() == asOfH, L"the new result has the as-of height");
        wg::Canvas fresh;
        PaintCard(*module, fresh, width, theme);
        Check(HasInk(fresh, left, cardspec_view::kTitleBlock + 18, 300,
                     cardspec_view::kLineH, background),
              L"the newly fetched rows are painted");
    }

    // ---- approve save failure: rolled back, never effective --------------
    {
        const std::wstring saveFail = ExeDir() + L"\\cardspec-network-test-savefail";
        RemoveTree(saveFail);
        CreateDirectoryW(saveFail.c_str(), nullptr);
        const std::wstring cfgDir = saveFail + L"\\cardspec.ini";
        // A directory at the Store path makes wg::Store::Save fail deterministically
        // (its temp file cannot replace a directory); no privilege is needed.
        Check(CreateDirectoryW(cfgDir.c_str(), nullptr) != 0, L"block the store file");
        const std::wstring spec2 = saveFail + L"\\cardspec.json";
        Check(WriteBytes(spec2, HttpRowsJson()), L"write a spec next to the blocked store");

        wg::Store store;
        store.Open(cfgDir);
        store.Set(L"http.profile", L"demo");
        store.Set(L"http.url", kUrl);

        FakeFetch fake;
        fake.next = [&] { return OkBody(RowsBody()); };
        int refreshes = 0;
        CardSpecOptions options;
        options.fetch = [&fake](std::string_view profile, const cardspec::HttpPolicy& policy) {
            return fake(profile, policy);
        };
        options.confirmNetwork = [](const std::wstring&) { return true; };
        std::unique_ptr<Module> module = CreateCardSpecModule(std::move(options));
        module->Bind(&store, [] {});
        module->BindRefresh([&refreshes] { ++refreshes; });

        module->Load();
        CheckChip(*module, L"BLOCKED", L"the blocked store starts unapproved");
        Check(fake.calls == 0, L"the blocked store starts zero-network");
        module->OnMenu(2);
        Check(store.Get(L"http.approved_profile").empty(),
              L"a failed approve save is rolled back in memory");
        Check(fake.calls == 0, L"a failed approve save fetches nothing");
        Check(refreshes == 1, L"the failed approve still republishes a readable state");
        module->Refresh();
        module->Commit();
        CheckChip(*module, L"BLOCKED", L"a failed approve is not effective");
        Check(fake.calls == 0, L"the refresh after a failed approve fetches nothing");

        // An invalid endpoint must show a blocked card without a modal prompt.
        int confirmCalls = 0;
        {
            wg::Store badStore;
            badStore.Open(cfgDir);
            badStore.Set(L"http.profile", L"demo");
            badStore.Set(L"http.url", L"http://127.0.0.1/x");
            FakeFetch badFake;
            CardSpecOptions badOptions;
            badOptions.fetch = [&badFake](std::string_view profile,
                                          const cardspec::HttpPolicy& policy) {
                return badFake(profile, policy);
            };
            badOptions.confirmNetwork = [&confirmCalls](const std::wstring&) {
                ++confirmCalls;
                return true;
            };
            std::unique_ptr<Module> bad = CreateCardSpecModule(std::move(badOptions));
            bad->Bind(&badStore, [] {});
            bad->Load();
            CheckChip(*bad, L"BLOCKED", L"an invalid endpoint is Blocked");
            bad->OnMenu(2);
            Check(confirmCalls == 0, L"an invalid endpoint never opens a confirmation");
            Check(badFake.calls == 0, L"an invalid endpoint never fetches");
        }
        RemoveTree(saveFail);
    }

    // ---- revoke save failure: still blocked in this process --------------
    {
        const std::wstring saveFail = ExeDir() + L"\\cardspec-network-test-revokefail";
        RemoveTree(saveFail);
        CreateDirectoryW(saveFail.c_str(), nullptr);
        const std::wstring cfgDir = saveFail + L"\\cardspec.ini";
        Check(CreateDirectoryW(cfgDir.c_str(), nullptr) != 0, L"block the revoke store");
        const std::wstring spec2 = saveFail + L"\\cardspec.json";
        Check(WriteBytes(spec2, HttpRowsJson()), L"write the revoke spec");

        wg::Store store;
        store.Open(cfgDir);
        store.Set(L"http.profile", L"demo");
        store.Set(L"http.url", kUrl);
        store.Set(L"http.approved_profile", L"demo");
        store.Set(L"http.approved_url", approved.approvedUrl);

        FakeFetch fake;
        fake.next = [&] { return OkBody(RowsBody()); };
        int refreshes = 0;
        CardSpecOptions options;
        options.fetch = [&fake](std::string_view profile, const cardspec::HttpPolicy& policy) {
            return fake(profile, policy);
        };
        options.confirmNetwork = [](const std::wstring&) { return true; };
        std::unique_ptr<Module> module = CreateCardSpecModule(std::move(options));
        module->Bind(&store, [] {});
        module->BindRefresh([&refreshes] { ++refreshes; });

        module->Load();
        CheckChip(*module, L"READY", L"the revoke fixture starts approved");
        Check(fake.calls == 0, L"the revoke fixture starts zero-network");

        module->OnMenu(3);
        Check(store.Get(L"http.approved_profile").empty(),
              L"a failed revoke save still clears the in-memory approval");
        Check(refreshes == 1, L"the failed revoke requests a refresh");
        module->Refresh();
        module->Commit();
        CheckChip(*module, L"BLOCKED",
                  L"a failed revoke save still blocks this process");
        Check(fake.calls == 0, L"a failed revoke save fetches nothing");
        RemoveTree(saveFail);
    }

    RemoveTree(root);

    if (failures) {
        std::wprintf(L"[cardspec-network-test] %d failure(s)\n", failures);
        return 1;
    }
    std::wprintf(L"[cardspec-network-test] ok\n");
    return 0;
}
