// Headless tests for the M1.1 DSH context builder: workspace.json parsing,
// projcache turnOutline parsing (plain JSON, no zstd), sensitive exclusion,
// memory retrieval, token budget and the send log. Fixtures live under the
// test exe directory (build\) and imitate - but never contain - real DSH data.
#include "data/assistant/context.h"
#include "data/assistant/memory.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[assistant-context-test] FAIL %s\n", message);
        ++failures;
    }
}

constexpr long long kNow = 1794000000000LL;  // fixed clock
constexpr long long kDayMs = 24LL * 60 * 60 * 1000;

std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0) return L".";
    std::wstring text(path, n);
    const std::size_t slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : text.substr(0, slash);
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

bool WriteUtf8(const std::wstring& path, const std::wstring& text) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const std::string bytes = WideToUtf8(text);
    DWORD wrote = 0;
    const BOOL ok = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()),
                              &wrote, nullptr);
    CloseHandle(handle);
    return ok && wrote == bytes.size();
}

bool SetFileMtime(const std::wstring& path, long long ms) {
    HANDLE handle = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    ULARGE_INTEGER value{};
    value.QuadPart = static_cast<unsigned long long>(ms + 11644473600000LL) * 10000ULL;
    FILETIME ft{};
    ft.dwLowDateTime = value.LowPart;
    ft.dwHighDateTime = value.HighPart;
    const BOOL ok = SetFileTime(handle, nullptr, nullptr, &ft);
    CloseHandle(handle);
    return ok != FALSE;
}

void DeleteTree(const std::wstring& path) {
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring child = path + L"\\" + name;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                DeleteTree(child);
            } else {
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    RemoveDirectoryW(path.c_str());
}

bool ReadFileText(const std::wstring& path, std::wstring* out) {
    out->clear();
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    std::string bytes;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        bytes.append(buffer, got);
    }
    CloseHandle(handle);
    if (bytes.empty()) return true;
    const int size = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                         static_cast<int>(bytes.size()), nullptr, 0);
    if (size <= 0) return false;
    out->resize(static_cast<std::size_t>(size));
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                        out->data(), size);
    return true;
}

bool Contains(const std::wstring& haystack, const wchar_t* needle) {
    return haystack.find(needle) != std::wstring::npos;
}

bool ContainsId(const std::vector<std::wstring>& ids, const std::wstring& id) {
    for (const std::wstring& value : ids) {
        if (value == id) return true;
    }
    return false;
}

std::wstring GetEnvText(const wchar_t* name) {
    wchar_t value[32768]{};
    const DWORD n = GetEnvironmentVariableW(name, value, 32768);
    return std::wstring(value, n < 32768 ? n : 0);
}

} // namespace

int wmain() {
    using namespace assistant;

    const std::wstring dir = ExeDir() + L"\\assistant-test-context";
    const std::wstring dsh = dir + L"\\dsh";
    const std::wstring cfg = dir + L"\\cfg";
    const std::wstring sessionsDir = dsh + L"\\storages\\session_projcache\\sessions";
    DeleteTree(dir);
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW(dsh.c_str(), nullptr);
    CreateDirectoryW((dsh + L"\\storages").c_str(), nullptr);
    CreateDirectoryW((dsh + L"\\storages\\session_projcache").c_str(), nullptr);
    CreateDirectoryW(sessionsDir.c_str(), nullptr);
    CreateDirectoryW(cfg.c_str(), nullptr);

    // --- fixtures (fake project/session shapes, no real user data) --------
    const std::wstring workspace =
        L"{\n"
        L"  \"unit\": { \"name\": \"workspace\", \"version\": 2 },\n"
        L"  \"global\": { \"initialized\": true, \"defaultWorkspaceId\": \"ws-a\" },\n"
        L"  \"tables\": {\n"
        L"    \"workspaces\": {\n"
        L"      \"ws-a\": {\n"
        L"        \"path\": \"M:\\\\studies\\\\auto-control\",\n"
        L"        \"title\": \"自动控制原理\",\n"
        L"        \"sessionIds\": [\"session-AAA\"],\n"
        L"        \"createdAt\": \"2026-10-20T05:22:23.461Z\",\n"
        L"        \"updatedAt\": \"2026-11-06T17:15:00.751Z\"\n"
        L"      },\n"
        L"      \"ws-b\": {\n"
        L"        \"path\": \"M:\\\\workspace\\\\win11\",\n"
        L"        \"title\": \"win11桌面组件\",\n"
        L"        \"sessionIds\": [],\n"
        L"        \"createdAt\": \"2026-10-20T05:22:23.461Z\",\n"
        L"        \"updatedAt\": \"2026-11-05T10:00:00.000Z\"\n"
        L"      },\n"
        L"      \"ws-ssh\": {\n"
        L"        \"path\": \"C:\\\\Users\\\\me\\\\.ssh\",\n"
        L"        \"title\": \"secret keys\",\n"
        L"        \"sessionIds\": [],\n"
        L"        \"updatedAt\": \"2026-11-07T10:00:00.000Z\"\n"
        L"      }\n"
        L"    }\n"
        L"  }\n"
        L"}\n";
    Check(WriteUtf8(dsh + L"\\storages\\workspace.json", workspace),
          "workspace fixture written");

    const std::wstring sessionAaa =
        L"{\n"
        L"  \"version\": 7,\n"
        L"  \"record\": {\n"
        L"    \"identity\": { \"formatVersion\": 4, \"createdAt\": 1793965380355, "
        L"\"cwd\": \"M:\\\\studies\\\\auto-control\" },\n"
        L"    \"rows\": {\n"
        L"      \"title\": { \"ver\": 1, \"seq\": 2, \"val\": \"自动控制原理会话\" },\n"
        L"      \"turnOutline\": {\n"
        L"        \"ver\": 2,\n"
        L"        \"seq\": 2,\n"
        L"        \"val\": {\n"
        L"          \"turns\": [\n"
        L"            { \"turn\": 1, \"seq\": 11, \"prompt\": \"第一问\", "
        L"\"response\": \"第一轮结论\" },\n"
        L"            { \"turn\": 2, \"seq\": 19, \"prompt\": \"第二问\", "
        L"\"response\": \"最后结论：M1 记忆已完成\" },\n"
        L"            { \"turn\": 3, \"seq\": 25, \"prompt\": \"第三问\", "
        L"\"response\": \"\" }\n"
        L"          ],\n"
        L"          \"draft\": \"\"\n"
        L"        }\n"
        L"      }\n"
        L"    }\n"
        L"  }\n"
        L"}\n";
    Check(WriteUtf8(sessionsDir + L"\\session-AAA.json", sessionAaa),
          "session AAA fixture written");

    const std::wstring sessionBbb =
        L"{\n"
        L"  \"record\": {\n"
        L"    \"identity\": { \"cwd\": \"M:\\\\studies\\\\auto-control\" },\n"
        L"    \"rows\": {\n"
        L"      \"title\": { \"val\": \"敏感会话\" },\n"
        L"      \"turnOutline\": { \"val\": { \"turns\": [\n"
        L"        { \"turn\": 1, \"prompt\": \"key?\", "
        L"\"response\": \"用 sk-abcdef123456 试了一下\" }\n"
        L"      ], \"draft\": \"\" } }\n"
        L"    }\n"
        L"  }\n"
        L"}\n";
    Check(WriteUtf8(sessionsDir + L"\\session-BBB.json", sessionBbb),
          "session BBB fixture written");

    const std::wstring sessionCcc =
        L"{\n"
        L"  \"record\": {\n"
        L"    \"identity\": { \"cwd\": \"M:\\\\studies\\\\auto-control\" },\n"
        L"    \"rows\": {\n"
        L"      \"title\": { \"val\": \"空会话\" },\n"
        L"      \"turnOutline\": { \"val\": { \"turns\": [], \"draft\": \"\" } }\n"
        L"    }\n"
        L"  }\n"
        L"}\n";
    Check(WriteUtf8(sessionsDir + L"\\session-CCC.json", sessionCcc),
          "session CCC fixture written");

    Check(SetFileMtime(dsh + L"\\storages\\workspace.json", kNow - 2 * kDayMs),
          "workspace mtime");
    Check(SetFileMtime(sessionsDir + L"\\session-AAA.json", kNow - 2 * kDayMs),
          "session AAA mtime");
    Check(SetFileMtime(sessionsDir + L"\\session-BBB.json", kNow - kDayMs),
          "session BBB mtime");
    Check(SetFileMtime(sessionsDir + L"\\session-CCC.json", kNow - 3 * kDayMs),
          "session CCC mtime");

    // --- workspace parsing -------------------------------------------------
    const std::vector<DshProject> projects = ReadDshProjects(dsh, 3);
    Check(projects.size() == 3, "all three workspaces parse");
    if (projects.size() == 3) {
        Check(projects[0].id == L"ws-ssh", "workspaces are newest first");
        Check(projects[1].id == L"ws-a", "second workspace is ws-a");
        Check(projects[1].title == L"自动控制原理", "workspace title parses");
        Check(projects[1].path == L"M:\\studies\\auto-control", "workspace path parses");
        const long long delta = projects[1].updatedMs - projects[2].updatedMs;
        Check(delta == 112500751LL, "ISO updatedAt parses to the right instant");
    }
    Check(ReadDshProjects(dsh + L"\\missing", 3).empty(),
          "missing workspace.json yields no projects");
    Check(ReadDshProjects(dsh, 1).size() == 1, "project limit is honoured");

    // --- session parsing ---------------------------------------------------
    const std::vector<DshSession> sessions = ReadDshSessions(dsh, 3);
    Check(sessions.size() == 2, "only sessions with a non-empty response parse");
    if (sessions.size() == 2) {
        Check(sessions[0].id == L"session-BBB", "sessions are newest first");
        Check(sessions[1].id == L"session-AAA", "second session is AAA");
        Check(sessions[1].title == L"自动控制原理会话", "session title parses");
        Check(sessions[1].cwd == L"M:\\studies\\auto-control", "session cwd parses");
        Check(sessions[1].lastResponse == L"最后结论：M1 记忆已完成",
              "last non-empty turnOutline response wins");
        Check(sessions[1].updatedMs == kNow - 2 * kDayMs, "session mtime is used");
    }
    Check(ReadDshSessions(dsh + L"\\missing", 3).empty(),
          "missing projcache yields no sessions");

    // --- context assembly --------------------------------------------------
    std::wstring error;
    MemoryStore memory;
    Check(memory.Load(cfg, &error), "memory store loads");
    Fact goal;
    goal.kind = L"goal";
    goal.text = L"目标：完成 M1 助理";
    goal.sourceRef = L"dsh-project-ws-a";
    goal.confidence = 85;
    goal.createdMs = kNow - kDayMs;
    goal.updatedMs = kNow - kDayMs;
    Check(memory.AddFact(goal, &error), "seed a fact for context");
    const std::wstring factId = memory.Facts().front().id;

    ContextBuilder builder(cfg, dsh, memory);
    const ContextBundle bundle = builder.Build(kNow, 6000, L"M1 助理目标");
    Check(!bundle.prompt.empty(), "prompt is not empty");
    Check(bundle.prompt.size() <= 6000u * 2u, "prompt fits a 6000-token budget");
    Check(Contains(bundle.prompt, L"记忆事实"), "prompt has a memory section");
    Check(Contains(bundle.prompt, L"目标：完成 M1 助理"), "relevant fact is injected");
    Check(Contains(bundle.prompt, L"最后结论：M1 记忆已完成"),
          "last DSH session response is injected");
    Check(Contains(bundle.prompt, L"自动控制原理"), "active project is injected");
    Check(!Contains(bundle.prompt, L".ssh"), "sensitive project path is excluded");
    Check(!Contains(bundle.prompt, L"secret keys"), "sensitive project title is excluded");
    Check(!Contains(bundle.prompt, L"sk-abcdef123456"),
          "sensitive session response is excluded");

    Check(ContainsId(bundle.sourceIds, factId), "fact id is a citable source");
    Check(ContainsId(bundle.sourceIds, L"dsh-session-session-AAA"),
          "session id is a citable source");
    Check(!ContainsId(bundle.sourceIds, L"dsh-session-session-BBB"),
          "sensitive session is not citable");
    Check(ContainsId(bundle.sourceIds, L"dsh-project-ws-a"),
          "project id is a citable source");
    Check(!ContainsId(bundle.sourceIds, L"dsh-project-ws-ssh"),
          "sensitive project is not citable");

    // --- send log ----------------------------------------------------------
    std::wstring logText;
    Check(ReadFileText(builder.send_log_path(), &logText), "send log is written");
    Check(Contains(logText, L"budget=6000"), "send log records the budget");
    Check(Contains(logText, factId.c_str()), "send log records referenced ids");
    Check(!Contains(logText, L"目标：完成 M1 助理"),
          "send log never records prompt/fact text");
    Check(!Contains(logText, L"sk-abcdef123456"), "send log never records secrets");

    // --- budget ------------------------------------------------------------
    const ContextBundle small = builder.Build(kNow, 300, L"M1 助理目标");
    Check(small.prompt.size() <= 300u * 2u, "small budget is respected");
    const ContextBundle tiny = builder.Build(kNow, 40, L"M1 助理目标");
    Check(tiny.prompt.size() <= 40u * 2u, "tiny budget is respected");

    // --- recent episodes ---------------------------------------------------
    // A separate config dir so the episode timestamps (real clock) line up with
    // the facts they cite (also real clock, assigned by the store).
    {
        const std::wstring cfgEpisodes = dir + L"\\cfg-episodes";
        CreateDirectoryW(cfgEpisodes.c_str(), nullptr);
        MemoryStore episodes;
        Check(episodes.Load(cfgEpisodes, &error), "episode store loads");
        Fact alpha;
        alpha.kind = L"goal";
        alpha.text = L"alpha 目标：完成 episode 验收";
        alpha.sourceRef = L"user";
        Check(episodes.AddFact(alpha, &error), "episode fact alpha is stored");
        const std::wstring alphaId = episodes.Facts().front().id;
        Check(episodes.AppendEpisode(
                  L"alpha 进展怎样",
                  L"alpha 已完成一半 [[mem:" + alphaId + L"]]", alphaId, &error),
              "episode citing alpha is stored");
        const std::wstring episodeId = episodes.RecentEpisodes(1).front().id;
        ContextBuilder episodeBuilder(cfgEpisodes, dsh, episodes);
        const ContextBundle withEpisode = episodeBuilder.Build(kNow, 6000, L"无关问题");
        Check(Contains(withEpisode.prompt, L"近期问答"),
              "recent episodes enter the prompt");
        Check(Contains(withEpisode.prompt, L"alpha 已完成一半"),
              "a live episode answer is injected");
        Check(ContainsId(withEpisode.sourceIds, episodeId),
              "an episode id is a citable source");
        std::wstring episodeLog;
        Check(ReadFileText(episodeBuilder.send_log_path(), &episodeLog),
              "episode send log is written");
        Check(Contains(episodeLog, episodeId.c_str()),
              "send log records the episode id");
        Check(!Contains(episodeLog, L"alpha 已完成一半"),
              "send log never records episode text");

        // Edited after the answer: the stored answer may quote the old text, so
        // it must not come back through the episode log.
        Sleep(20);
        Check(episodes.UpdateFact(alphaId, L"alpha 目标改成别的", &error),
              "episode fact is edited");
        const ContextBundle afterEdit = episodeBuilder.Build(kNow, 6000, L"无关问题");
        Check(!Contains(afterEdit.prompt, L"alpha 已完成一半"),
              "an episode citing an edited fact is dropped");
        Check(!ContainsId(afterEdit.sourceIds, episodeId),
              "an episode citing an edited fact is not citable");

        // Forgotten: the id is a tombstone and the old answer must not return.
        Check(episodes.DeleteFact(alphaId, &error), "episode fact is forgotten");
        const ContextBundle afterForget = episodeBuilder.Build(kNow, 6000, L"无关问题");
        Check(!Contains(afterForget.prompt, L"alpha 已完成一半"),
              "an episode citing a deleted id is dropped");
        Check(!ContainsId(afterForget.sourceIds, episodeId),
              "an episode citing a deleted id is not citable");
    }
    {
        // Deleted fact text must not be re-injected through an episode that
        // merely repeats it, even without a citation token.
        const std::wstring cfgText = dir + L"\\cfg-episode-text";
        CreateDirectoryW(cfgText.c_str(), nullptr);
        MemoryStore store;
        Check(store.Load(cfgText, &error), "episode-text store loads");
        Fact beta;
        beta.kind = L"preference";
        beta.text = L"偏好用深色主题界面";
        Check(store.AddFact(beta, &error), "episode-text fact is stored");
        const std::wstring betaId = store.Facts().front().id;
        Check(store.AppendEpisode(L"主题选哪个", L"结论：偏好用深色主题界面", L"", &error),
              "episode repeating the fact text is stored");
        ContextBuilder textBuilder(cfgText, dsh, store);
        Check(Contains(textBuilder.Build(kNow, 6000, L"无关").prompt,
                       L"结论：偏好用深色主题界面"),
              "the episode is injected while the fact is active");
        Check(store.DeleteFact(betaId, &error), "episode-text fact is forgotten");
        const ContextBundle purged = textBuilder.Build(kNow, 6000, L"无关");
        Check(!Contains(purged.prompt, L"结论：偏好用深色主题界面"),
              "forgotten fact text never returns through an episode");
    }

    // --- sensitive facts already on disk never reach prompt/send log ------
    {
        const std::wstring cfg2 = dir + L"\\cfg2";
        CreateDirectoryW(cfg2.c_str(), nullptr);
        // A hand-edited or legacy facts file can hold material AddFact would
        // reject today; Build() must still keep it out of the prompt, the
        // citation list and assistant-send.log.
        const std::wstring legacy =
            L"{\"id\":\"mem-legacy01\",\"kind\":\"fact\",\"text\":\"key file at "
            L"C:\\\\Users\\\\me\\\\keys\\\\server.pem\",\"source\":\"\","
            L"\"confidence\":80,\"status\":\"active\",\"created\":1793000000000,"
            L"\"updated\":1793000000000,\"expires\":0}\n";
        Check(WriteUtf8(cfg2 + L"\\assistant-facts.jsonl", legacy),
              "legacy sensitive fact fixture written");
        MemoryStore legacyStore;
        Check(legacyStore.Load(cfg2, &error), "legacy store loads");
        Check(legacyStore.Facts().size() == 1, "legacy fact loads into the store");
        ContextBuilder legacyBuilder(cfg2, dsh, legacyStore);
        const ContextBundle legacyBundle =
            legacyBuilder.Build(kNow, 6000, L"server.pem key");
        Check(!Contains(legacyBundle.prompt, L"server.pem"),
              "a sensitive legacy fact is excluded from the prompt");
        Check(!ContainsId(legacyBundle.sourceIds, L"mem-legacy01"),
              "a sensitive legacy fact is not citable");
        std::wstring legacyLog;
        Check(ReadFileText(legacyBuilder.send_log_path(), &legacyLog),
              "legacy send log is written");
        Check(!Contains(legacyLog, L"server.pem"),
              "the send log never records a sensitive fact");
    }

    // --- DefaultDshRoot: isolated test instance ---------------------------
    {
        const std::wstring previousInstance = GetEnvText(L"DESK_TEST_INSTANCE");
        const std::wstring previousRoot = GetEnvText(L"DESK_TEST_DSH_ROOT");
        const std::wstring previousProfile = GetEnvText(L"USERPROFILE");
        SetEnvironmentVariableW(L"USERPROFILE", dir.c_str());
        SetEnvironmentVariableW(L"DESK_TEST_INSTANCE", L"context-unit");
        SetEnvironmentVariableW(L"DESK_TEST_DSH_ROOT", dsh.c_str());
        Check(DefaultDshRoot() == dsh, "DESK_TEST_DSH_ROOT overrides the test DSH root");
        SetEnvironmentVariableW(L"DESK_TEST_DSH_ROOT", nullptr);
        const std::wstring isolated = DefaultDshRoot();
        Check(isolated != dir + L"\\.dsh",
              "a test instance without an override never reads the profile DSH");
        Check(ReadDshProjects(isolated, 3).empty(), "the isolated root has no DSH data");
        SetEnvironmentVariableW(L"DESK_TEST_INSTANCE", nullptr);
        Check(DefaultDshRoot() == dir + L"\\.dsh",
              "without a test instance the profile root is unchanged");
        SetEnvironmentVariableW(L"DESK_TEST_INSTANCE",
                                previousInstance.empty() ? nullptr
                                                         : previousInstance.c_str());
        SetEnvironmentVariableW(L"DESK_TEST_DSH_ROOT",
                                previousRoot.empty() ? nullptr : previousRoot.c_str());
        SetEnvironmentVariableW(L"USERPROFILE",
                                previousProfile.empty() ? nullptr : previousProfile.c_str());
    }

    // --- defaults ----------------------------------------------------------
    const std::wstring defaultRoot = DefaultDshRoot();
    const bool testInstance = !GetEnvText(L"DESK_TEST_INSTANCE").empty();
    Check(defaultRoot.empty() || testInstance ||
              (defaultRoot.size() > 5 &&
               defaultRoot.compare(defaultRoot.size() - 5, 5, L"\\.dsh") == 0),
          "DefaultDshRoot points at a .dsh directory");

    DeleteTree(dir);
    std::printf("[assistant-context-test] %s\n", failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
