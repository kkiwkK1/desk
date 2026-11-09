// Engine integration regression: the executable doubles as a synthetic RPC
// peer. No real pi, credentials, DSH data, network or desktop window is used.
//
// Covered here (M1 acceptance hooks):
//   * local deterministic memory commands: remember/forget/clear, zero child
//     process, no cost, no episode, visible Chinese confirmation/error;
//   * BRIEF + project facts from a synthetic DSH root, and the anti-resurrect
//     rule after forget/clear-all (checkpoint + tombstone);
//   * citation filtering (valid id kept, bogus stripped, plain [[text]] kept,
//     marker split across deltas, unterminated tail flushed at done);
//   * session continuity: assistant-session.ini path+digest, resume across
//     restart, invalidation on fact/policy change, idle stop in milliseconds;
//   * daily token budget degradation and enabled=0;
//   * sensitive questions/persona/context never reach pi (synthetic canary).
#include "data/assistant/engine.h"
#include "data/assistant/json.h"
#include "widget/store.h"

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

namespace {
using assistant::JsonValue;
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("[assistant-engine-test] FAIL %s\n", what); ++failures; }
}
std::wstring ExePath() {
    wchar_t path[32768]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, 32768);
    return std::wstring(path, n);
}
void Emit(JsonValue value) { std::cout << value.Dump() << '\n' << std::flush; }
JsonValue Event(const char* type) {
    auto value = JsonValue::MakeObject();
    value.Set("type", JsonValue::MakeString(type));
    return value;
}
void Delta(const std::string& text) {
    auto update = Event("text_delta");
    update.Set("delta", JsonValue::MakeString(text));
    auto value = Event("message_update");
    value.Set("assistantMessageEvent", std::move(update));
    Emit(std::move(value));
}
void Settled(bool aborted) {
    auto value = Event("agent_settled");
    value.Set("aborted", JsonValue::MakeBool(aborted));
    Emit(std::move(value));
}

// --- synthetic peer -------------------------------------------------------

int g_argc = 0;
wchar_t** g_argv = nullptr;

std::wstring PeerArgAfter(const wchar_t* flag) {
    for (int i = 1; i + 1 < g_argc; ++i) {
        if (std::wstring(g_argv[i]) == flag) return g_argv[i + 1];
    }
    return L"";
}

std::string WideToUtf8(const std::wstring& text);

std::string SliceAfter(const std::string& text, const std::string& marker, char terminator) {
    const std::size_t at = text.find(marker);
    if (at == std::string::npos) return {};
    const std::size_t begin = at + marker.size();
    const std::size_t end = text.find(terminator, begin);
    if (end == std::string::npos) return {};
    return text.substr(begin, end - begin);
}

void AppendMarker(const char* name, const std::string& line) {
    std::ofstream out(name, std::ios::app | std::ios::binary);
    out << line << "\n";
}

int Peer() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    // One line per child process: the regression asserts "0 subprocesses" for
    // local commands by the absence of this file.
    AppendMarker("peer-starts.txt", "start");
    std::string line;
    while (std::getline(std::cin, line)) {
        JsonValue request;
        if (!assistant::ParseJson(line, &request)) continue;
        const auto type = request["type"].String();
        if (type == "get_state" &&
            std::filesystem::current_path().parent_path().filename() ==
                L"handshake-stop") {
            std::ofstream("handshake-entered.txt") << "synthetic handshake entered\n";
            continue;  // wait for EOF; never answer this synthetic handshake
        }
        auto response = Event("response");
        response.Set("id", request["id"]);
        response.Set("command", JsonValue::MakeString(type));
        response.Set("success", JsonValue::MakeBool(true));
        if (type == "get_state") {
            // A real session file, so "resume" below is an honest claim and the
            // engine's existence check has something to find.
            const std::filesystem::path sessionFile =
                std::filesystem::current_path() / L"synthetic-session.jsonl";
            {
                std::ofstream touch(sessionFile, std::ios::app | std::ios::binary);
                touch << "synthetic session turn\n";
            }
            auto data = JsonValue::MakeObject();
            data.Set("sessionFile", JsonValue::MakeString(WideToUtf8(sessionFile.wstring())));
            response.Set("data", std::move(data));
        }
        Emit(std::move(response));
        if (type == "abort") { Settled(true); continue; }
        if (type != "prompt") continue;
        const auto prompt = request["message"].String();
        {
            std::ofstream dump("prompt-dump.txt", std::ios::app | std::ios::binary);
            dump << prompt << "\n----\n";
        }
        // Branch on the user's question only. The context above it now quotes
        // recent episode questions verbatim, so a fixture keyword used by an
        // earlier turn must not re-trigger its branch on a later request.
        const std::size_t questionAt = prompt.rfind("USER QUESTION:");
        const std::string ask =
            questionAt == std::string::npos ? prompt : prompt.substr(questionAt);
        std::string answer = "resume=0 ";
        {
            const std::wstring resumePath = PeerArgAfter(L"--session");
            if (!resumePath.empty()) {
                answer = std::filesystem::exists(resumePath) ? "resume=1 "
                                                            : "resume=missing ";
            }
        }
        if (ask.find("ABORT_FIXTURE") != std::string::npos) {
            Delta(answer);
            continue;
        }
        if (ask.find("EMPTY_ANSWER") != std::string::npos) {
            // A successful turn without text: the engine keeps the session but
            // writes no episode, so an empty ClearAll leaves the digest equal.
            Settled(false);
            continue;
        }
        if (ask.find("ERROR_FIXTURE") != std::string::npos) {
            auto error = Event("extension_error");
            error.Set("error", JsonValue::MakeString("SYNTHETIC FAILURE"));
            Emit(std::move(error));
            Settled(false);
            continue;
        }
        if (ask.find("ECHO_MEM") != std::string::npos) {
            const std::string id = SliceAfter(prompt, "- [mem-", '|');
            if (!id.empty()) {
                answer += "结论 [[mem:mem-" + id + "]] [[mem:mem-not-in-sources]]"
                          " 普通[[不是引用]]";
            }
        }
        if (ask.find("ECHO_SRC") != std::string::npos) {
            const std::string id = SliceAfter(prompt, "- [dsh-session-", ']');
            if (!id.empty()) answer += " [[src:dsh-session-" + id + "]]";
        }
        if (ask.find("SPLIT_MARKER") != std::string::npos) {
            // A citation marker cut across two deltas must not flash as text.
            Delta(answer + "结论 [[me");
            Delta("m:mem-split-check]] 结束");
            Settled(false);
            continue;
        }
        if (ask.find("PLAIN_TAIL") != std::string::npos) {
            Delta(answer + "结尾[[未闭合");
            Settled(false);
            continue;
        }
        Delta(answer + "synthetic answer");
        Settled(false);
    }
    return 0;
}

// --- test helpers ---------------------------------------------------------

struct Events {
    std::mutex mu;
    std::condition_variable cv;
    bool delta = false;
    int done = 0, errors = 0;
    std::wstring text;
    std::wstring error;
    std::wstring citation;
    long long inputTokens = 0, outputTokens = 0;
    assistant::Engine* engine = nullptr;
    void Reset(assistant::Engine* e) {
        std::lock_guard<std::mutex> lock(mu);
        delta = false;
        done = errors = 0;
        text.clear();
        error.clear();
        citation.clear();
        inputTokens = outputTokens = 0;
        engine = e;
    }
    assistant::Engine::Callbacks Callbacks() {
        assistant::Engine::Callbacks cb;
        cb.onDelta = [this](std::wstring_view piece) {
            std::lock_guard<std::mutex> lock(mu);
            delta = true;
            text.append(piece);
            cv.notify_all();
        };
        cb.onDone = [this](const std::wstring& sources, long long in, long long out,
                           long long) {
            std::lock_guard<std::mutex> lock(mu);
            ++done;
            citation = sources;
            inputTokens = in;
            outputTokens = out;
            cv.notify_all();
        };
        cb.onError = [this](std::wstring_view message) {
            std::lock_guard<std::mutex> lock(mu);
            ++errors;
            error = message;
            cv.notify_all();
        };
        return cb;
    }
    bool WaitDelta() {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, std::chrono::seconds(15), [&] { return delta; });
    }
    bool Wait() {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, std::chrono::seconds(15),
                           [&] { return done > 0 || errors > 0; });
    }
};

void Settings(const std::wstring& dir, const std::wstring& piCommand) {
    std::filesystem::create_directories(dir);
    wg::Store store;
    store.Open(dir + L"\\assistant.ini");
    store.Set(L"pi_command", piCommand);
    store.Set(L"provider", L"");
    store.Set(L"model", L"");
    store.Set(L"key_ref", L"");
    store.SetInt(L"idle_stop_minutes", 0);
    store.SetInt(L"enabled", 1);
    store.SetInt(L"daily_token_limit", 0);
    store.Set(L"provider_extension", L"");
    store.Save();
}

void SetSetting(const std::wstring& dir, const wchar_t* key, const std::wstring& value) {
    wg::Store store;
    store.Open(dir + L"\\assistant.ini");
    store.Set(key, value);
    store.Save();
}

void SetSettingInt(const std::wstring& dir, const wchar_t* key, long long value) {
    wg::Store store;
    store.Open(dir + L"\\assistant.ini");
    store.SetInt(key, value);
    store.Save();
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring ReadAll(const std::wstring& path) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return L"";
    std::string bytes;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        bytes.append(buffer, got);
    }
    CloseHandle(handle);
    if (bytes.empty()) return L"";
    const int size = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(),
                                         nullptr, 0);
    if (size <= 0) return L"";
    std::wstring out((std::size_t)size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), out.data(), size);
    return out;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out((std::size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), size,
                        nullptr, nullptr);
    return out;
}

int CountLines(const std::wstring& path) {
    const std::wstring text = ReadAll(path);
    int lines = 0;
    for (wchar_t c : text) {
        if (c == L'\n') ++lines;
    }
    return lines;
}

std::wstring Trim(const std::wstring& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\r' ||
                           text[begin] == L'\n' || text[begin] == L'\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\r' ||
                           text[end - 1] == L'\n' || text[end - 1] == L'\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool Contains(const std::wstring& haystack, const wchar_t* needle) {
    return haystack.find(needle) != std::wstring::npos;
}

// --- synthetic DSH fixture ------------------------------------------------

void WriteFileText(const std::wstring& path, const std::wstring& text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    const std::string bytes = WideToUtf8(text);
    out.write(bytes.data(), (std::streamsize)bytes.size());
}

struct DshFixtureOptions {
    // A workspace file makes the engine auto-import project facts on Start;
    // citation/episode scenarios that count facts opt out of that side effect.
    bool withWorkspace = true;
    bool withSession = true;
    bool withSecondProject = false;
    bool withSensitiveProject = false;
    std::wstring blocker = L"卡点：等额度审批";
};

void WriteDshFixture(const std::wstring& dsh, const DshFixtureOptions& options) {
    std::wstring workspace =
        L"{\n  \"tables\": {\n    \"workspaces\": {\n"
        L"      \"ws-a\": {\n        \"path\": \"M:\\\\fixtures\\\\alpha\",\n"
        L"        \"title\": \"alpha 项目\",\n"
        L"        \"updatedAt\": \"2026-11-06T17:15:00.751Z\"\n      }";
    if (options.withSecondProject) {
        workspace +=
            L",\n      \"ws-b\": {\n        \"path\": \"M:\\\\fixtures\\\\beta\",\n"
            L"        \"title\": \"beta 项目\",\n"
            L"        \"updatedAt\": \"2026-11-05T10:00:00.000Z\"\n      }";
    }
    if (options.withSensitiveProject) {
        workspace +=
            L",\n      \"ws-ssh\": {\n        \"path\": \"C:\\\\Users\\\\me\\\\.ssh\",\n"
            L"        \"title\": \"secret keys\",\n"
            L"        \"updatedAt\": \"2026-11-07T10:00:00.000Z\"\n      }";
    }
    workspace += L"\n    }\n  }\n}\n";
    if (options.withWorkspace) {
        WriteFileText(dsh + L"\\storages\\workspace.json", workspace);
    }
    if (!options.withSession) return;
    const std::wstring response =
        L"最后结论：alpha 已完成一半。" + options.blocker;
    const std::wstring session =
        L"{\n  \"record\": {\n    \"identity\": { \"cwd\": \"M:\\\\fixtures\\\\alpha\" },\n"
        L"    \"rows\": {\n      \"title\": { \"val\": \"alpha 会话\" },\n"
        L"      \"turnOutline\": { \"val\": { \"turns\": [\n"
        L"        { \"turn\": 1, \"prompt\": \"进展\", \"response\": \"" + response +
        L"\" }\n      ], \"draft\": \"\" } }\n    }\n  }\n}\n";
    WriteFileText(dsh + L"\\storages\\session_projcache\\sessions\\session-AAA.json",
                  session);
}

std::wstring TodayKeyPrefix() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    wchar_t buffer[32];
    swprintf(buffer, 32, L"day.%04d-%02d-%02d", local.wYear, local.wMonth, local.wDay);
    return buffer;
}

std::wstring LocalDayTokenKey() { return TodayKeyPrefix() + L".tokens"; }

// Defined with the other environment helpers below.
std::wstring GetEnv(const wchar_t* name);

// --- scenarios ------------------------------------------------------------

// Remote question lifecycle: success persists an episode+cost exactly once;
// cancel/failure persist nothing and stay visible.
void RunRemoteCase(const std::wstring& dir, const std::wstring& question, bool cancel,
                   bool failure, bool beforeDelta = false) {
    Settings(dir, ExePath());
    Events events;
    assistant::Engine engine;
    engine.Start(dir, assistant::Engine::Options{dir + L"\\dsh", 0}, nullptr);
    std::wstring error;
    auto shared = assistant::OpenSharedMemory(dir, &error);
    Check((bool)shared, "external shared memory opens");
    if (!shared) return;
    std::atomic<int> changes{0};
    const auto subscription = shared->Subscribe([&] { ++changes; });
    events.Reset(&engine);
    engine.Ask(question, events.Callbacks());
    if (cancel) {
        if (!beforeDelta) Check(events.WaitDelta(), "partial response arrives before cancel");
        engine.Cancel();
    }
    Check(events.Wait(), "request receives a terminal callback");
    // Stop joins every event producer before assertions on the collector.
    engine.Stop();
    shared->Unsubscribe(subscription);
    Check(changes.load() == 0, "a remote answer never publishes a fact change");
    if (cancel || failure) {
        Check(events.done == 0, "failure/cancel is not reported as success");
        Check(events.errors == 1, "one visible failure/cancel callback");
        Check(engine.memory().Facts().empty(), "failure/cancel does not create facts");
        Check(engine.memory().RecentEpisodes(10).empty(),
              "failure/cancel does not write an episode");
        Check(!Exists(dir + L"\\assistant-cost.ini"),
              "failure/cancel does not record a successful call");
        if (cancel) Check(events.error == L"CANCELLED", "cancel status is explicit");
    } else {
        Check(events.done == 1 && events.errors == 0, "success callback exactly once");
        Check(events.text.find(L"resume=") != std::wstring::npos,
              "the peer answer is streamed");
        Check(engine.memory().Facts().empty(), "a remote answer creates no fact");
        Check(engine.memory().RecentEpisodes(10).size() == 1,
              "successful answer is committed before onDone");
        wg::Store costs;
        Check(costs.Open(dir + L"\\assistant-cost.ini"), "successful call writes cost file");
        int callKeys = 0;
        for (const auto& [key, value] : costs.Snapshot()) {
            if (key.ends_with(L".calls")) {
                ++callKeys;
                Check(value == L"1", "successful call is counted exactly once");
            }
        }
        Check(callKeys == 1, "cost has one daily call counter");
        assistant::MemoryStore reloaded;
        Check(reloaded.Load(dir, &error), "reload committed memory");
        Check(reloaded.Facts().empty() && reloaded.RecentEpisodes(10).size() == 1,
              "the episode survives restart");
    }
}

// Local commands: deterministic, visible, no child process, no cost, no
// episode. The configured pi command does not exist, so any launch attempt
// would surface as an error.
void LocalCommands(const std::wstring& dir) {
    Settings(dir, dir + L"\\never-launched.exe");
    assistant::Engine engine;
    std::wstring error;
    Check(engine.Start(dir, assistant::Engine::Options{dir + L"\\dsh", 0}, &error),
          "local: engine starts");
    Events events;

    events.Reset(&engine);
    engine.Ask(L"记住 完成 M1 验收的目标是全部测试通过", events.Callbacks());
    Check(events.Wait(), "local remember answers without pi");
    Check(events.done == 1 && events.errors == 0, "local remember is a success");
    Check(Contains(events.text, L"已记住"), "local remember confirms in Chinese");
    Check(events.citation.empty(), "a local command has no model citation");
    {
        const auto facts = engine.memory().Facts();
        Check(facts.size() == 1, "local remember stores one fact");
        if (!facts.empty()) {
            Check(facts[0].kind == L"milestone", "\"X 的目标是 Y\" becomes a milestone");
            Check(facts[0].sourceRef == L"user", "a remembered fact records the user source");
        }
    }

    events.Reset(&engine);
    engine.Ask(L"记住 目标：完成 M1 记忆", events.Callbacks());
    Check(events.Wait() && events.done == 1, "local remember goal works");
    events.Reset(&engine);
    engine.Ask(L"记住 回答用中文", events.Callbacks());
    Check(events.Wait() && events.done == 1, "local remember preference works");
    {
        bool goal = false, preference = false;
        for (const auto& fact : engine.memory().Facts()) {
            if (fact.kind == L"goal" && Contains(fact.text, L"完成 M1 记忆")) goal = true;
            if (fact.kind == L"preference" && Contains(fact.text, L"回答用中文")) {
                preference = true;
            }
        }
        Check(goal, "a generic goal becomes kind=goal");
        Check(preference, "anything else becomes kind=preference");
    }
    Check(engine.memory().RecentEpisodes(10).empty(), "local commands write no episode");
    Check(!Exists(dir + L"\\assistant-cost.ini"), "local commands write no cost");
    Check(!Exists(dir + L"\\pi-work\\peer-starts.txt"), "local commands start no pi child");

    events.Reset(&engine);
    engine.Ask(L"忘掉 完成 M1 验收", events.Callbacks());
    Check(events.Wait() && events.done == 1, "local forget succeeds");
    Check(Contains(events.text, L"已忘掉"), "local forget confirms in Chinese");
    Check(engine.memory().Facts().size() == 2, "the forgotten fact leaves the active list");
    {
        int tombstones = 0;
        for (const auto& fact : engine.memory().AllFacts()) {
            if (fact.status != L"active" && Contains(fact.text, L"完成 M1 验收")) ++tombstones;
        }
        Check(tombstones == 1, "forget keeps a tombstone instead of reviving later");
    }
    events.Reset(&engine);
    engine.Ask(L"忘掉 不存在的记忆内容", events.Callbacks());
    Check(events.Wait() && events.errors == 1, "forget without a match is a visible error");
    Check(!events.error.empty(), "the forget error carries text");

    events.Reset(&engine);
    engine.Ask(L"记住 凭据 sk-abcdef123456", events.Callbacks());
    Check(events.Wait() && events.errors == 1,
          "a sensitive remember is rejected by the store and visible");
    Check(engine.memory().Facts().size() == 2, "the sensitive remember stores nothing");

    events.Reset(&engine);
    engine.Ask(L"清空全部记忆", events.Callbacks());
    Check(events.Wait() && events.done == 1, "local clear-all succeeds");
    Check(Contains(events.text, L"已清空"), "local clear-all confirms in Chinese");
    Check(engine.memory().Facts().empty(), "clear-all removes every fact");
    Check(engine.memory().RecentEpisodes(10).empty(), "clear-all removes the episodes");
    Check(!Exists(dir + L"\\pi-work\\peer-starts.txt"), "clear-all starts no pi child");
    engine.Stop();
}

// BRIEF + project facts + the anti-resurrection rule.
void BriefAndProjects(const std::wstring& root) {
    const std::wstring dir = root + L"\\projects";
    const std::wstring dsh = root + L"\\dsh";
    DshFixtureOptions options;
    options.withSecondProject = true;
    options.withSensitiveProject = true;
    WriteDshFixture(dsh, options);
    Settings(dir, ExePath());
    {
        assistant::Engine engine;
        std::wstring error;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, &error),
              "projects: engine starts");
        {
            const auto facts = engine.memory().Facts();
            Check(facts.size() == 2, "only the two non-sensitive projects are imported");
            int alpha = 0;
            for (const auto& fact : facts) {
                if (fact.kind != L"project") continue;
                if (fact.sourceRef == L"dsh-project-ws-a") {
                    ++alpha;
                    Check(Contains(fact.text, L"alpha 项目"), "the project fact keeps the title");
                }
                Check(fact.sourceRef != L"dsh-project-ws-ssh",
                      "the sensitive project is never imported");
            }
            Check(alpha == 1, "the real project id is the fact source");
        }
        const std::wstring brief = engine.Brief();
        Check(Contains(brief, L"alpha 项目"), "BRIEF lists the real project");
        Check(Contains(brief, L"最近结论："), "BRIEF includes the latest conclusion");
        Check(Contains(brief, L"alpha 已完成一半"), "BRIEF reuses the cached conclusion");
        Check(Contains(brief, L"卡点：等额度审批"), "BRIEF extracts the recorded blocker");
        Check(Contains(brief, L"beta 项目"), "BRIEF lists the second project");
        Check(Contains(brief, L"卡点：未记录"), "a project without a blocker says so");
        Check(Contains(brief, L"最近结论：未记录"),
              "a project without a cached session says so");
        Check(!Contains(brief, L".ssh"), "BRIEF never shows a sensitive project");
        engine.Stop();
    }
    {
        // A second Start must not duplicate the project facts.
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "projects: restart");
        Check(engine.memory().Facts().size() == 2, "restart does not duplicate project facts");
        Events events;
        engine.Ask(L"忘掉 alpha 项目", events.Callbacks());
        Check(events.Wait() && events.done == 1, "the project fact can be forgotten");
        Check(engine.memory().Facts().size() == 1, "forget removes the project fact");
        engine.Stop();
    }
    {
        // The forgotten project must not be re-created by the next Start.
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "projects: restart after forget");
        bool resurrected = false;
        for (const auto& fact : engine.memory().Facts()) {
            if (Contains(fact.text, L"alpha 项目")) resurrected = true;
        }
        Check(!resurrected, "a forgotten project fact is not re-imported on Start");
        engine.Stop();
    }
    {
        // Clear-all must stick for project facts too.
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "projects: restart before clear-all");
        Events events;
        engine.Ask(L"清空全部记忆", events.Callbacks());
        Check(events.Wait() && events.done == 1, "projects: clear-all");
        Check(engine.memory().Facts().empty(), "projects: clear-all empties the store");
        engine.Stop();
    }
    {
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "projects: restart after clear-all");
        Check(engine.memory().Facts().empty(),
              "clear-all is not undone by re-importing project facts");
        engine.Stop();
    }
}

// Session continuity: save/resume/invalidate.
void SessionContinuity(const std::wstring& root) {
    const std::wstring dir = root + L"\\session";
    const std::wstring dsh = root + L"\\session-dsh";
    std::filesystem::create_directories(dsh);
    Settings(dir, ExePath());
    const std::wstring sessionFile = dir + L"\\assistant-session.ini";
    {
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "session: first engine starts");
        Events events;
        engine.Ask(L"第一问", events.Callbacks());
        Check(events.Wait() && events.done == 1, "session: first remote answer");
        Check(Contains(events.text, L"resume=0"), "a fresh session is started first");
        engine.Stop();
        wg::Store saved;
        Check(saved.Open(sessionFile), "the session file is written");
        const std::wstring savedPath = saved.Get(L"session_path", L"");
        const std::wstring suffix = L"synthetic-session.jsonl";
        Check(savedPath.size() >= suffix.size() &&
                  savedPath.compare(savedPath.size() - suffix.size(), suffix.size(),
                                    suffix) == 0,
              "the pi session path is saved");
        Check(Exists(savedPath), "the saved pi session file really exists");
        Check(!saved.Get(L"facts_digest", L"").empty(), "the fact digest is saved");
        Check(!saved.Get(L"policy_digest", L"").empty(), "the policy digest is saved");
        // Only the path, two hashes and the memory reset revision are stored: no
        // provider/model/credential material can leak into this file.
        Check(saved.Snapshot().size() == 4,
              "the session file holds only path + digests + reset revision");
    }
    {
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "session: restart");
        Events events;
        engine.Ask(L"第二问", events.Callbacks());
        Check(events.Wait() && events.done == 1, "session: answer after restart");
        Check(Contains(events.text, L"resume=1"), "the saved pi session is resumed");
        // A memory change made by the UI/MEMORY card while the engine runs.
        assistant::Fact fact;
        fact.kind = L"preference";
        fact.text = L"偏好简洁回答";
        std::wstring error;
        Check(engine.memory().AddFact(fact, &error), "session: external fact is added");
        engine.Stop();
    }
    {
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "session: restart after a fact change");
        Events events;
        engine.Ask(L"第三问", events.Callbacks());
        Check(events.Wait() && events.done == 1, "session: answer after fact change");
        Check(Contains(events.text, L"resume=0"),
              "a changed fact digest drops the old pi session");
        engine.Stop();
    }
    {
        SetSetting(dir, L"model", L"other/model");
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "session: restart after a policy change");
        Events events;
        engine.Ask(L"第四问", events.Callbacks());
        Check(events.Wait() && events.done == 1, "session: answer after policy change");
        Check(Contains(events.text, L"resume=0"), "a changed policy drops the old pi session");
        engine.Stop();
        SetSetting(dir, L"model", L"");
    }
}

// Idle stop with a millisecond seam, then resume on the next question.
void IdleStop(const std::wstring& root) {
    const std::wstring dir = root + L"\\idle";
    const std::wstring dsh = root + L"\\idle-dsh";
    std::filesystem::create_directories(dsh);
    Settings(dir, ExePath());
    {
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 180}, nullptr),
              "idle: engine starts");
        Check(engine.idle_stop_ms() == 180, "the millisecond idle seam is honoured");
        Events events;
        engine.Ask(L"第一问", events.Callbacks());
        Check(events.Wait() && events.done == 1, "idle: first answer");
        Sleep(900);  // well past the 180 ms idle timeout
        events.Reset(&engine);
        engine.Ask(L"第二问", events.Callbacks());
        Check(events.Wait() && events.done == 1, "idle: second answer after idle stop");
        Check(Contains(events.text, L"resume=1"), "the idle restart resumes the session");
        const std::wstring marker = dir + L"\\pi-work\\peer-starts.txt";
        Check(CountLines(marker) == 2, "idle stop ends one child and the next Ask starts one");
        engine.Stop();
    }
    {
        // Default: idle_stop_minutes (10) applies when no seam is given.
        assistant::Engine engine;
        Settings(dir, ExePath());
        SetSettingInt(dir, L"idle_stop_minutes", 10);
        Check(engine.Start(dir, nullptr), "idle: default engine starts");
        Check(engine.idle_stop_ms() == 10 * 60 * 1000, "the default idle timeout stays 10 min");
        engine.Stop();
    }
}

// Daily token budget degrades to a local BRIEF; enabled=0 refuses everything.
void BudgetAndDisabled(const std::wstring& root) {
    const std::wstring dir = root + L"\\budget";
    const std::wstring dsh = root + L"\\budget-dsh";
    DshFixtureOptions options;
    WriteDshFixture(dsh, options);
    Settings(dir, dir + L"\\never-launched.exe");
    SetSettingInt(dir, L"daily_token_limit", 100);
    {
        wg::Store costs;
        costs.Open(dir + L"\\assistant-cost.ini");
        costs.SetInt(LocalDayTokenKey().c_str(), 250);
        costs.Save();
    }
    {
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "budget: engine starts");
        Events events;
        engine.Ask(L"问一个问题", events.Callbacks());
        Check(events.Wait(), "budget: degraded answer arrives");
        Check(events.done == 1 && events.errors == 0, "budget: degradation is a success");
        Check(Contains(events.text, L"预算已用完"), "budget: the footer explains the budget");
        Check(Contains(events.text, L"alpha 项目"), "budget: the local BRIEF is used");
        Check(!Exists(dir + L"\\pi-work\\peer-starts.txt"),
              "budget: no pi child is started");
        wg::Store costs;
        costs.Open(dir + L"\\assistant-cost.ini");
        Check(costs.GetInt(LocalDayTokenKey().c_str(), 0) == 250,
              "budget: a degraded answer adds no tokens");
        int calls = 0;
        for (const auto& [key, value] : costs.Snapshot()) {
            if (key.ends_with(L".calls")) ++calls;
        }
        Check(calls == 0, "budget: a degraded answer records no call");
        Check(engine.memory().RecentEpisodes(10).empty(),
              "budget: a degraded answer writes no episode");
        engine.Stop();
    }
    {
        const std::wstring offDir = root + L"\\disabled";
        Settings(offDir, ExePath());
        SetSettingInt(offDir, L"enabled", 0);
        assistant::Engine engine;
        Check(engine.Start(offDir, assistant::Engine::Options{dsh, 0}, nullptr),
              "disabled: Start succeeds so the card can explain");
        Events events;
        engine.Ask(L"问一个问题", events.Callbacks());
        Check(events.Wait(), "disabled: rejection is delivered");
        Check(events.errors == 1 && events.done == 0, "disabled: Ask is explicitly refused");
        Check(Contains(events.error, L"DISABLED"), "disabled: the error names the setting");
        Check(!Exists(offDir + L"\\pi-work\\peer-starts.txt"),
              "disabled: no pi child is started");
        Check(!Exists(offDir + L"\\assistant-cost.ini"), "disabled: no cost is recorded");
        engine.Stop();
    }
}

// Citations end to end: valid ids kept, bogus ids stripped, plain [[text]]
// preserved, a marker split across deltas stays hidden, an unterminated tail
// is flushed literally at done.
void Citations(const std::wstring& root) {
    const std::wstring dir = root + L"\\citation";
    const std::wstring dsh = root + L"\\citation-dsh";
    // Session cache only: without a workspace file the engine imports no
    // project facts, so "exactly one fact" below measures the remembered fact
    // itself instead of the current project-import feature.
    DshFixtureOptions options;
    options.withWorkspace = false;
    WriteDshFixture(dsh, options);
    Settings(dir, ExePath());
    assistant::Engine engine;
    Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
          "citation: engine starts");
    Events events;
    engine.Ask(L"记住 ECHO_MEM 偏好简洁输出", events.Callbacks());
    Check(events.Wait() && events.done == 1, "citation: seed a remembered fact");
    Check(engine.memory().Facts().size() == 1, "citation: the fact is stored");

    events.Reset(&engine);
    engine.Ask(L"解释 ECHO_MEM", events.Callbacks());
    Check(events.Wait() && events.done == 1, "citation: answer with citations");
    Check(Contains(events.text, L"普通[[不是引用]]"),
          "plain [[text]] is not swallowed by citation stripping");
    Check(!Contains(events.text, L"mem-not-in-sources"),
          "a bogus citation id never reaches the reader");
    Check(Contains(events.citation, L"mem-"), "a real memory id is accepted");
    Check(!Contains(events.citation, L"mem-not-in-sources"),
          "a bogus id is not reported as a citation");

    events.Reset(&engine);
    engine.Ask(L"解释 ECHO_SRC", events.Callbacks());
    Check(events.Wait() && events.done == 1, "citation: DSH source answer");
    Check(Contains(events.citation, L"dsh-session-session-AAA"),
          "a real DSH session id is accepted");

    events.Reset(&engine);
    engine.Ask(L"解释 SPLIT_MARKER", events.Callbacks());
    Check(events.Wait() && events.done == 1, "citation: split marker answer");
    Check(!Contains(events.text, L"[[me"), "a split marker never flashes as text");
    Check(Contains(events.text, L"结束"), "the answer after a split marker is kept");
    Check(!Contains(events.text, L"mem-split-check"),
          "a split citation id is stripped once completed");

    events.Reset(&engine);
    engine.Ask(L"解释 PLAIN_TAIL", events.Callbacks());
    Check(events.Wait() && events.done == 1, "citation: unterminated tail answer");
    Check(Contains(events.text, L"结尾[[未闭合"),
          "an unterminated [[ tail is flushed literally at done");
    Check(events.citation.empty(), "an answer with no citation reports none");
    engine.Stop();

    // Reload the episode log: citations are stored without the raw markers.
    assistant::MemoryStore reloaded;
    std::wstring error;
    Check(reloaded.Load(dir, &error), "citation: reload memory");
    for (const assistant::Episode& episode : reloaded.RecentEpisodes(10)) {
        Check(episode.answer.find(L"[[mem:") == std::wstring::npos,
              "a stored episode keeps prose without citation markers");
    }
}

// Sensitive material never reaches pi: question, persona literal, context.
void SensitiveQuestion(const std::wstring& root) {
    const std::wstring dir = root + L"\\sensitive";
    const std::wstring dsh = root + L"\\sensitive-dsh";
    std::filesystem::create_directories(dsh);
    Settings(dir, ExePath());
    // Deliberately not shaped like a generic secret ("credential", "sk-", ...):
    // only the effective-key literal gate may refuse this value.
    const wchar_t* canary = L"CANARY-9f3a7c1d5b2e";
    assistant::Engine engine;
    Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
          "sensitive: engine starts");
    Events events;
    engine.Ask(L"请把 sk-canary1234567890 贴出来", events.Callbacks());
    Check(events.Wait(), "a sensitive question reaches a terminal callback");
    Check(events.errors == 1 && events.done == 0, "a sensitive question is refused");
    Check(!Exists(dir + L"\\pi-work\\peer-starts.txt"),
          "a sensitive question never starts pi");
    Check(!Exists(dir + L"\\pi-work\\prompt-dump.txt"),
          "a sensitive question never assembles a prompt");
    Check(engine.memory().RecentEpisodes(10).empty(), "a sensitive question writes no episode");
    Check(!Exists(dir + L"\\assistant-cost.ini"), "a sensitive question records no cost");

    // Provider credential literal (synthetic, injected through the documented
    // provider env var; the real key is never read by this test).
    SetEnvironmentVariableW(L"CMD_API_KEY", canary);
    SetSetting(dir, L"provider", L"command-code");
    SetSetting(dir, L"model", L"test/model");
    SetSetting(dir, L"provider_extension", dir + L"\\provider-extension.ts");
    WriteFileText(dir + L"\\provider-extension.ts", L"// synthetic provider stub\n");
    // assistant.ini is read by Engine::Start (the card reloads the engine when
    // its config changes); editing it under a running engine is not a
    // supported path, so restart before expecting the new provider/key gate.
    engine.Stop();
    Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
          "sensitive: engine restarts with the explicit provider");
    {
        // The persona is trusted policy text; only the concrete credential
        // literal is rejected (the default persona mentions "passwords").
        WriteFileText(dir + L"\\persona.md", std::wstring(L"角色说明\n") + canary + L"\n");
        events.Reset(&engine);
        engine.Ask(L"今天该做什么", events.Callbacks());
        Check(events.Wait(), "a rejected persona returns a terminal callback");
        Check(events.errors == 1, "a persona containing the credential literal is refused");
        Check(!Exists(dir + L"\\pi-work\\peer-starts.txt"),
              "a rejected persona never starts pi");
        // Restore the default persona so the next case measures the question.
        DeleteFileW((dir + L"\\persona.md").c_str());
    }
    {
        events.Reset(&engine);
        engine.Ask(std::wstring(L"请复述 ") + canary, events.Callbacks());
        Check(events.Wait(), "a credential question returns a terminal callback");
        Check(events.errors == 1, "a question quoting the credential literal is refused");
        Check(!Exists(dir + L"\\pi-work\\peer-starts.txt"),
              "a credential question never starts pi");
        events.Reset(&engine);
        engine.Ask(L"今天该做什么", events.Callbacks());
        Check(events.Wait() && events.done == 1,
              "a normal question works with an explicit provider extension");
        const std::wstring dump = ReadAll(dir + L"\\pi-work\\prompt-dump.txt");
        Check(!dump.empty(), "the prompt was delivered to the peer");
        Check(!Contains(dump, canary), "the credential literal never enters the prompt");
        Check(!Contains(events.text, canary), "the credential literal never returns");
        Check(Contains(events.text, L"resume="), "the peer answered normally");
    }
    SetEnvironmentVariableW(L"CMD_API_KEY", nullptr);

    // A missing official provider extension is a visible failure, not a silent
    // fallback to pi's own providers.
    {
        // USERPROFILE/HOME are redirected to the fixture root by wmain, so the
        // default installed extension path cannot exist here: the engine must
        // fail closed instead of silently using pi's own providers.
        SetSetting(dir, L"provider", L"command-code");
        SetSetting(dir, L"provider_extension", L"");
        engine.Stop();
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "sensitive: engine restarts without an explicit extension");
        events.Reset(&engine);
        engine.Ask(L"今天该做什么", events.Callbacks());
        Check(events.Wait(), "a missing extension returns a terminal callback");
        Check(events.errors == 1, "a missing official provider extension is visible");
        Check(Contains(events.error, L"provider"), "the error names the provider extension");
        SetSetting(dir, L"provider", L"");
    }
    engine.Stop();
}

// A ClearAll of an already-empty store still notifies: the session must be
// dropped even though the fact digest is unchanged. The invalidation is also
// persisted by Stop so a restart cannot resume the old pi session.
void ClearAllInvalidatesSession(const std::wstring& root) {
    const std::wstring dir = root + L"\\clear-session";
    const std::wstring dsh = root + L"\\clear-session-dsh";
    std::filesystem::create_directories(dsh);
    Settings(dir, ExePath());
    const std::wstring sessionFile = dir + L"\\assistant-session.ini";
    {
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "clear-session: engine starts");
        Events events;
        engine.Ask(L"第一问", events.Callbacks());
        Check(events.Wait() && events.done == 1, "clear-session: first answer");
        Check(Exists(sessionFile), "clear-session: the session is saved");
        Check(engine.memory().Facts().empty(), "clear-session: the store starts empty");
        std::wstring error;
        Check(engine.memory().ClearAll(&error),
              "clear-session: ClearAll of an empty store succeeds");
        events.Reset(&engine);
        engine.Ask(L"第二问", events.Callbacks());
        Check(events.Wait() && events.done == 1, "clear-session: answer after ClearAll");
        Check(Contains(events.text, L"resume=0"),
              "ClearAll drops the old session even when the digest is unchanged");
        engine.Stop();
    }
    {
        // A memory change followed by Stop (no next Ask) is persisted: the
        // session file is gone before the next Start reads it.
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "clear-session: restart");
        Events events;
        engine.Ask(L"第三问", events.Callbacks());
        Check(events.Wait() && events.done == 1, "clear-session: third answer");
        Check(Exists(sessionFile), "clear-session: the session is saved again");
        assistant::Fact fact;
        fact.kind = L"preference";
        fact.text = L"偏好先给结论";
        std::wstring error;
        Check(engine.memory().AddFact(fact, &error), "clear-session: UI adds a fact");
        engine.Stop();  // no Ask in between: the invalidation must persist
        Check(!Exists(sessionFile),
              "Stop persists the invalidation when the memory epoch changed");
    }
    {
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "clear-session: restart after Stop invalidation");
        Events events;
        engine.Ask(L"第四问", events.Callbacks());
        Check(events.Wait() && events.done == 1, "clear-session: fourth answer");
        Check(Contains(events.text, L"resume=0"),
              "a Stop-persisted invalidation is not resumed after restart");
        engine.Stop();
    }
}

// A callback runs on the engine worker (local commands) or on pi's reader
// thread; the UI contract is to marshal it back to the UI thread. A submit made
// from the engine worker itself must be refused visibly, without detaching or
// self-joining the worker thread.
void ReentrantAskRejected(const std::wstring& root) {
    const std::wstring dir = root + L"\\reentrant-ask";
    const std::wstring dsh = root + L"\\reentrant-ask-dsh";
    std::filesystem::create_directories(dsh);
    Settings(dir, ExePath());
    assistant::Engine engine;
    Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
          "reentrant ask: engine starts");
    std::mutex mu;
    std::condition_variable cv;
    bool outerDone = false;
    bool innerError = false;
    std::wstring innerText;
    assistant::Engine::Callbacks cb;
    cb.onDelta = [](std::wstring_view) {};
    cb.onDone = [&](const std::wstring&, long long, long long, long long) {
        assistant::Engine::Callbacks inner;
        inner.onDelta = [](std::wstring_view) {};
        inner.onDone = [&](const std::wstring&, long long, long long, long long) {};
        inner.onError = [&](std::wstring_view text) {
            std::lock_guard<std::mutex> lock(mu);
            innerError = true;
            innerText.assign(text);
            cv.notify_all();
        };
        engine.Ask(L"记住 重入回调内的记忆", inner);
        std::lock_guard<std::mutex> lock(mu);
        outerDone = true;
        cv.notify_all();
    };
    cb.onError = [&](std::wstring_view text) {
        std::lock_guard<std::mutex> lock(mu);
        innerError = true;
        innerText.assign(text);
        cv.notify_all();
    };
    engine.Ask(L"记住 重入回归基准", cb);
    {
        std::unique_lock<std::mutex> lock(mu);
        Check(cv.wait_for(lock, std::chrono::seconds(10),
                          [&] { return outerDone && innerError; }),
              "reentrant ask: both callbacks arrive");
    }
    Check(Contains(innerText, L"REENTRANT"),
          "a re-entrant Ask from an engine callback is refused");
    Check(engine.memory().Facts().size() == 1, "the re-entrant Ask stores nothing");
    Events after;
    engine.Ask(L"记住 重入之后仍可写入", after.Callbacks());
    Check(after.Wait() && after.done == 1,
          "the engine still serves requests after a refused re-entrant Ask");
    const auto start = std::chrono::steady_clock::now();
    engine.Stop();
    Check(std::chrono::steady_clock::now() - start < std::chrono::seconds(6),
          "Stop remains bounded after a refused re-entrant Ask");
}

// Local commands and the resolved provider credential: a remember may not store
// it, a missing key reference must not break local memory, and a forget of a
// secret payload must work without echoing it. All values here are synthetic.
void LocalSecrets(const std::wstring& root) {
    const std::wstring dir = root + L"\\local-secrets";
    const std::wstring dsh = root + L"\\local-secrets-dsh";
    std::filesystem::create_directories(dsh);
    Settings(dir, dir + L"\\never-launched.exe");
    SetSetting(dir, L"provider", L"command-code");
    SetSetting(dir, L"model", L"test/model");
    SetSetting(dir, L"provider_extension", dir + L"\\provider-extension.ts");
    WriteFileText(dir + L"\\provider-extension.ts", L"// synthetic provider stub\n");
    const std::wstring key = L"LOCALKEY-8b21f5";
    const std::wstring other = L"OTHERKEY-77c1";
    const std::wstring previousKey = GetEnv(L"CMD_API_KEY");
    SetEnvironmentVariableW(L"CMD_API_KEY", key.c_str());
    assistant::Engine engine;
    Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
          "local secrets: engine starts");
    Events events;
    engine.Ask(L"记住 主渠道标记 " + key + L" 备用", events.Callbacks());
    Check(events.Wait(), "local secrets: refused remember returns a callback");
    Check(events.errors == 1 && events.done == 0,
          "local remember refuses the resolved provider key (no sk- shape needed)");
    Check(Contains(events.error, L"凭据"), "the refusal names the credential");
    Check(engine.memory().Facts().empty(), "the key is not stored as a fact");
    Check(engine.memory().RecentEpisodes(10).empty(), "the key is not written as an episode");
    Check(!Exists(dir + L"\\assistant-send.log"), "no context/send log for a local command");
    Check(!Exists(dir + L"\\assistant-cost.ini"), "no cost for a local command");
    Check(!Exists(dir + L"\\pi-work\\peer-starts.txt"), "no pi child for a local command");
    Check(!Contains(ReadAll(dir + L"\\assistant-facts.jsonl"), key.c_str()),
          "the provider key never reaches the facts file");

    // A missing key reference must not break ordinary local memory.
    SetEnvironmentVariableW(L"CMD_API_KEY", L"");
    events.Reset(&engine);
    engine.Ask(L"记住 回答用中文", events.Callbacks());
    Check(events.Wait() && events.done == 1,
          "local remember works without a resolved key reference");

    // A secret payload can be forgotten locally and is never echoed back.
    events.Reset(&engine);
    engine.Ask(L"记住 备份标记 " + other, events.Callbacks());
    Check(events.Wait() && events.done == 1,
          "the backup marker is stored while it is not the current key");
    SetEnvironmentVariableW(L"CMD_API_KEY", key.c_str());
    events.Reset(&engine);
    engine.Ask(L"忘掉 " + other, events.Callbacks());
    Check(events.Wait() && events.done == 1, "a secret payload can still be forgotten");
    Check(Contains(events.text, L"已忘掉"), "local forget confirms in Chinese");
    Check(!Contains(events.text, other.c_str()),
          "the forget confirmation never echoes the payload");
    for (const auto& fact : engine.memory().Facts()) {
        Check(!Contains(fact.text, other.c_str()),
              "the forgotten payload is gone from the active facts");
    }
    Check(engine.memory().RecentEpisodes(10).empty(),
          "local commands never write an episode");
    Check(!Contains(ReadAll(dir + L"\\assistant-facts.jsonl"), key.c_str()),
          "the provider key never reaches the facts file after local flows");
    engine.Stop();
    SetEnvironmentVariableW(L"CMD_API_KEY", previousKey.c_str());
}

// A saved session whose pi session file disappeared must not make every Ask
// fail: the stale state is dropped and a fresh session is used.
void MissingSessionFile(const std::wstring& root) {
    const std::wstring dir = root + L"\\missing-session";
    const std::wstring dsh = root + L"\\missing-session-dsh";
    std::filesystem::create_directories(dsh);
    Settings(dir, ExePath());
    const std::wstring sessionIni = dir + L"\\assistant-session.ini";
    std::wstring piSession;
    {
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "missing-session: engine starts");
        Events events;
        engine.Ask(L"第一问", events.Callbacks());
        Check(events.Wait() && events.done == 1, "missing-session: first answer");
        engine.Stop();
        wg::Store saved;
        saved.Open(sessionIni);
        piSession = saved.Get(L"session_path", L"");
        Check(!piSession.empty() && Exists(piSession),
              "missing-session: the fixture session file exists");
    }
    Check(!piSession.empty() && DeleteFileW(piSession.c_str()) != 0,
          "missing-session: the fixture session file is removed");
    {
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "missing-session: Start succeeds after the file is gone");
        Check(!Exists(sessionIni), "missing-session: stale session state is cleared on load");
        Events events;
        engine.Ask(L"第二问", events.Callbacks());
        Check(events.Wait() && events.done == 1,
              "missing-session: a fresh session answers instead of failing");
        Check(Contains(events.text, L"resume=0"),
              "missing-session: a missing session file is never resumed");
        engine.Stop();
    }
}

// A ClearAll performed while the engine is stopped must not be missed: the
// episodes-only case, and a fully empty store whose old session had no episode
// (where only the persisted reset revision differs).
void ClearAllAcrossRestart(const std::wstring& root) {
    const std::wstring dsh = root + L"\\clearall-dsh";
    std::filesystem::create_directories(dsh);
    const std::wstring episodesDir = root + L"\\clearall-episodes";
    {
        Settings(episodesDir, ExePath());
        assistant::Engine engine;
        Check(engine.Start(episodesDir, assistant::Engine::Options{dsh, 0}, nullptr),
              "clearall-episodes: engine starts");
        Events events;
        engine.Ask(L"第一问", events.Callbacks());
        Check(events.Wait() && events.done == 1, "clearall-episodes: first answer");
        Check(engine.memory().RecentEpisodes(10).size() == 1,
              "clearall-episodes: an episode is stored");
        engine.Stop();
        std::wstring error;
        auto shared = assistant::OpenSharedMemory(episodesDir, &error);
        Check((bool)shared, "clearall-episodes: the external store opens");
        if (shared) Check(shared->ClearAll(&error), "clearall-episodes: external ClearAll");
    }
    {
        assistant::Engine engine;
        Check(engine.Start(episodesDir, assistant::Engine::Options{dsh, 0}, nullptr),
              "clearall-episodes: restart");
        Events events;
        engine.Ask(L"第二问", events.Callbacks());
        Check(events.Wait() && events.done == 1,
              "clearall-episodes: answer after restart");
        Check(Contains(events.text, L"resume=0"),
              "a ClearAll while stopped drops the old session (episodes-only case)");
        engine.Stop();
    }
    const std::wstring emptyDir = root + L"\\clearall-empty";
    {
        Settings(emptyDir, ExePath());
        assistant::Engine engine;
        Check(engine.Start(emptyDir, assistant::Engine::Options{dsh, 0}, nullptr),
              "clearall-empty: engine starts");
        Events events;
        engine.Ask(L"EMPTY_ANSWER 第一问", events.Callbacks());
        Check(events.Wait() && events.done == 1, "clearall-empty: empty answer succeeds");
        Check(engine.memory().Facts().empty(), "clearall-empty: no facts");
        Check(engine.memory().RecentEpisodes(10).empty(), "clearall-empty: no episode");
        engine.Stop();
        // The saved session now belongs to an empty store: an empty ClearAll is
        // invisible in the digest, so the reset revision has to catch it.
        std::wstring error;
        auto shared = assistant::OpenSharedMemory(emptyDir, &error);
        Check((bool)shared, "clearall-empty: the external store opens");
        if (shared) Check(shared->ClearAll(&error), "clearall-empty: external empty ClearAll");
    }
    {
        assistant::Engine engine;
        Check(engine.Start(emptyDir, assistant::Engine::Options{dsh, 0}, nullptr),
              "clearall-empty: restart");
        Events events;
        engine.Ask(L"第二问", events.Callbacks());
        Check(events.Wait() && events.done == 1, "clearall-empty: answer after restart");
        Check(Contains(events.text, L"resume=0"),
              "an empty ClearAll while stopped still drops the old session");
        engine.Stop();
    }
    const std::wstring cancelledDir = root + L"\\clearall-cancelled";
    {
        Settings(cancelledDir, ExePath());
        assistant::Engine engine;
        Check(engine.Start(cancelledDir, assistant::Engine::Options{dsh, 0}, nullptr),
              "clearall-cancelled: engine starts");
        Events events;
        engine.Ask(L"解释 ABORT_FIXTURE", events.Callbacks());
        Check(events.WaitDelta(), "clearall-cancelled: the answer starts");
        engine.Cancel();
        Check(events.Wait() && events.errors == 1, "clearall-cancelled: cancelled");
        engine.Stop();
        Check(!Exists(cancelledDir + L"\\assistant-session.ini"),
              "clearall-cancelled: an aborted turn saves no session state");
        std::wstring error;
        auto shared = assistant::OpenSharedMemory(cancelledDir, &error);
        if (shared) Check(shared->ClearAll(&error), "clearall-cancelled: empty ClearAll");
    }
    {
        assistant::Engine engine;
        Check(engine.Start(cancelledDir, assistant::Engine::Options{dsh, 0}, nullptr),
              "clearall-cancelled: restart");
        Events events;
        engine.Ask(L"第二问", events.Callbacks());
        Check(events.Wait() && events.done == 1,
              "clearall-cancelled: answer after restart");
        Check(Contains(events.text, L"resume=0"),
              "a cancelled-only history never resumes an old session");
        engine.Stop();
    }
}

// Stop is called twice per card lifetime (the card destructor calls it, the
// Engine destructor calls it again). A memory change before a successful answer
// makes the saved session belong to a non-zero epoch; the second Stop must not
// compare that saved epoch against the released (zero) baseline and delete a
// valid session.
void DoubleStopKeepsSession(const std::wstring& root) {
    const std::wstring dir = root + L"\\double-stop";
    const std::wstring dsh = root + L"\\double-stop-dsh";
    DshFixtureOptions options;  // a real project import bumps the epoch at Start
    WriteDshFixture(dsh, options);
    Settings(dir, ExePath());
    {
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "double-stop: engine starts");
        Check(!engine.memory().Facts().empty(),
              "double-stop: the project import changed memory before the answer");
        Events events;
        engine.Ask(L"第一问", events.Callbacks());
        Check(events.Wait() && events.done == 1, "double-stop: first answer");
        engine.Stop();  // the card destructor's explicit stop
    }                   // the Engine destructor stops a second time
    {
        wg::Store saved;
        Check(saved.Open(dir + L"\\assistant-session.ini"),
              "double-stop: the normal session survives the second Stop");
        const std::wstring path = saved.Get(L"session_path", L"");
        Check(!path.empty() && Exists(path), "double-stop: the saved session path still exists");
    }
    {
        assistant::Engine engine;
        Check(engine.Start(dir, assistant::Engine::Options{dsh, 0}, nullptr),
              "double-stop: restart");
        Events events;
        engine.Ask(L"第二问", events.Callbacks());
        Check(events.Wait() && events.done == 1, "double-stop: answer after restart");
        Check(Contains(events.text, L"resume=1"),
              "double-stop: a normal session is still resumed after two Stops");
        engine.Stop();
    }
}

void StopAndRestart(const std::wstring& dir) {
    Settings(dir, ExePath());
    Events first, second;
    assistant::Engine engine;
    std::wstring error;
    Check(engine.Start(dir, assistant::Engine::Options{dir + L"\\dsh", 0}, &error),
          "restart: initial Start");
    auto shared = assistant::OpenSharedMemory(dir, &error);
    Check((bool)shared, "restart: shared store opens");
    if (!shared) return;
    assistant::Engine observer;
    Check(observer.Start(dir, assistant::Engine::Options{dir + L"\\dsh", 0}, &error),
          "second Engine opens the same config");
    Check(&observer.memory() == &engine.memory(), "two engines share one memory instance");
    observer.Stop();
    engine.Ask(L"解释 ABORT_FIXTURE 现状", first.Callbacks());
    Check(first.WaitDelta(), "restart: request is in flight");
    const auto start = std::chrono::steady_clock::now();
    engine.Stop();
    Check(std::chrono::steady_clock::now() - start < std::chrono::seconds(6),
          "in-flight Stop is bounded");
    Check(first.done == 0 && first.errors == 0, "Stop is silent during destruction");
    Check(shared->Facts().empty() && shared->RecentEpisodes(10).empty(),
          "in-flight Stop does not commit a partial answer");
    Check(engine.Start(dir, assistant::Engine::Options{dir + L"\\dsh", 0}, &error),
          "restart: Start after in-flight Stop");
    engine.Ask(L"解释 recovered fixture", second.Callbacks());
    Check(second.Wait(), "restart: next question receives a terminal callback");
    engine.Stop();
    Check(second.done == 1 && second.errors == 0, "restart does not retain BUSY");
    Check(shared->Facts().empty() && shared->RecentEpisodes(10).size() == 1,
          "restarted success is visible in the original shared store");
}

void StopDuringHandshake(const std::wstring& dir) {
    Settings(dir, ExePath());
    Events events;
    assistant::Engine engine;
    std::wstring error;
    Check(engine.Start(dir, assistant::Engine::Options{dir + L"\\dsh", 0}, &error),
          "handshake: Start");
    engine.Ask(L"解释 handshake fixture", events.Callbacks());
    const auto marker = std::filesystem::path(dir) / L"pi-work" / L"handshake-entered.txt";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!std::filesystem::exists(marker) &&
           std::chrono::steady_clock::now() < deadline) {
        Sleep(10);
    }
    Check(std::filesystem::exists(marker), "handshake peer reached get_state");
    const auto start = std::chrono::steady_clock::now();
    engine.Stop();
    Check(std::chrono::steady_clock::now() - start < std::chrono::seconds(6),
          "Stop interrupts handshake instead of waiting the 20s deadline");
    Check(events.done == 0 && events.errors == 0, "handshake shutdown has no UI callback");
    Check(engine.memory().Facts().empty(), "handshake shutdown creates no memory");
}

void ReentrantRejection(const std::wstring& dir) {
    Settings(dir, ExePath());
    assistant::Engine engine;
    std::wstring error;
    Check(engine.Start(dir, assistant::Engine::Options{dir + L"\\dsh", 0}, &error),
          "reentrant rejection: Start");
    bool called = false;
    assistant::Engine::Callbacks cb;
    cb.onError = [&](std::wstring_view text) {
        engine.Cancel();  // takes engine.mu_: an under-lock callback deadlocks
        called = text == L"EMPTY QUESTION";
    };
    engine.Ask(L"", cb);
    Check(called, "empty-question callback runs outside the engine lock");
    engine.Stop();
    called = false;
    cb.onError = [&](std::wstring_view text) {
        engine.Cancel();
        called = text == L"ASSISTANT NOT STARTED";
    };
    engine.Ask(L"question after Stop", cb);
    Check(called, "stopped engine rejects Ask without launching a worker");
}

std::wstring GetEnv(const wchar_t* name) {
    wchar_t value[32768]{};
    const DWORD n = GetEnvironmentVariableW(name, value, 32768);
    return std::wstring(value, n < 32768 ? n : 0);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    g_argc = argc;
    g_argv = argv;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::wstring(argv[i]) == L"--mode" && std::wstring(argv[i + 1]) == L"rpc")
            return Peer();
    const auto root = std::filesystem::path(ExePath()).parent_path() /
                      (L"assistant-engine-fixture-" + std::to_wstring(GetCurrentProcessId()));
    const std::wstring profile = GetEnv(L"USERPROFILE"), home = GetEnv(L"HOME");
    std::filesystem::create_directories(root);
    SetEnvironmentVariableW(L"USERPROFILE", root.c_str());
    SetEnvironmentVariableW(L"HOME", root.c_str());

    RunRemoteCase((root / L"success").wstring(), L"解释 success fixture", false, false);
    RunRemoteCase((root / L"cancel").wstring(), L"解释 ABORT_FIXTURE", true, false);
    RunRemoteCase((root / L"error").wstring(), L"解释 ERROR_FIXTURE", false, true);
    RunRemoteCase((root / L"early-cancel").wstring(), L"解释 ABORT_FIXTURE", true, false, true);
    LocalCommands((root / L"local").wstring());
    BriefAndProjects(root.wstring());
    SessionContinuity(root.wstring());
    IdleStop(root.wstring());
    BudgetAndDisabled(root.wstring());
    Citations(root.wstring());
    SensitiveQuestion(root.wstring());
    ClearAllInvalidatesSession(root.wstring());
    ReentrantAskRejected(root.wstring());
    LocalSecrets(root.wstring());
    MissingSessionFile(root.wstring());
    ClearAllAcrossRestart(root.wstring());
    DoubleStopKeepsSession(root.wstring());
    StopAndRestart((root / L"restart").wstring());
    StopDuringHandshake((root / L"handshake-stop").wstring());
    ReentrantRejection((root / L"reentrant").wstring());

    SetEnvironmentVariableW(L"USERPROFILE", profile.empty() ? nullptr : profile.c_str());
    SetEnvironmentVariableW(L"HOME", home.empty() ? nullptr : home.c_str());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::printf("[assistant-engine-test] %s (%d failures)\n", failures ? "failed" : "ok", failures);
    return failures ? 1 : 0;
}
