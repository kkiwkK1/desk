// Headless tests for the M1.1 memory store: episodes, facts CRUD, dedupe,
// correction, forgetting, persistence, the 2000-fact cap, sensitive rejection
// and Query ranking/decay. No window, no network: all fixtures live under the
// test exe directory (build\).
#include "data/assistant/memory.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[assistant-memory-test] FAIL %s\n", message);
        ++failures;
    }
}

constexpr long long kNow = 1794000000000LL;          // fixed clock
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

std::wstring ReadUtf8(const std::wstring& path) {
    HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::wstring();
    std::string bytes;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        bytes.append(buffer, got);
    }
    CloseHandle(handle);
    if (bytes.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                         static_cast<int>(bytes.size()), nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                        out.data(), size);
    return out;
}

// A facts line written before pinning existed (no "pinned" key).
std::wstring LegacyFactLine(const wchar_t* id, const wchar_t* text) {
    std::wstring line = L"{\"id\":\"";
    line += id;
    line += L"\",\"kind\":\"fact\",\"text\":\"";
    line += text;
    line += L"\",\"source\":\"\",\"confidence\":80,\"status\":\"active\","
            L"\"created\":1700000000000,\"updated\":1700000000000,\"expires\":0}\n";
    return line;
}

std::wstring Upper(const std::wstring& text) {
    std::wstring out = text;
    for (wchar_t& c : out) {
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    }
    return out;
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

assistant::Fact MakeFact(const wchar_t* kind, const wchar_t* text, long long updated,
                         const wchar_t* source = L"") {
    assistant::Fact fact;
    fact.kind = kind;
    fact.text = text;
    fact.sourceRef = source;
    fact.confidence = 80;
    fact.createdMs = updated;
    fact.updatedMs = updated;
    return fact;
}

std::wstring Number(long long value) {
    return std::to_wstring(value);
}

} // namespace

int wmain() {
    using namespace assistant;

    const std::wstring dir = ExeDir() + L"\\assistant-test-memory";
    DeleteTree(dir);
    CreateDirectoryW(dir.c_str(), nullptr);

    std::wstring error;
    MemoryStore store;
    Check(store.Load(dir, &error), "Load with no files is not an error");
    Check(error.empty(), "first run reports no error");
    Check(store.config_dir() == dir, "Load keeps the config directory");

    // --- episodes ---------------------------------------------------------
    Check(store.AppendEpisode(L"问：M1 目标是什么？", L"答：记忆 + 上下文 + 引用",
                              L"dsh-project-ws-a", &error),
          "append episode 1");
    Check(store.AppendEpisode(L"问：回答用什么语言？", L"答：中文",
                              L"", &error),
          "append episode 2");
    Check(!store.AppendEpisode(L"看一下 .ssh/id_rsa 的内容", L"ok", L"", &error),
          "episode with a sensitive path is rejected");
    Check(!store.AppendEpisode(L"普通问题", L"key 是 sk-abcdef123456", L"", &error),
          "episode with an API key is rejected");

    const std::vector<Episode> episodes = store.RecentEpisodes(10);
    Check(episodes.size() == 2, "two episodes are kept in memory");
    if (episodes.size() == 2) {
        Check(episodes.front().question == L"问：M1 目标是什么？",
              "episode order is append order");
        Check(episodes.back().sources.empty(), "episode sources round-trip");
        Check(episodes.front().id.rfind(L"ep-", 0) == 0, "episode id has the ep- prefix");
    }
    {
        MemoryStore reloaded;
        Check(reloaded.Load(dir, &error), "reload after episodes");
        Check(reloaded.RecentEpisodes(10).size() == 2,
              "episodes are persisted to assistant-episodes.jsonl");
    }

    // --- fact CRUD + persistence -----------------------------------------
    Fact goal = MakeFact(L"goal", L"目标：完成 M1 助理", kNow - 1000,
                         L"dsh-project-ws-a");
    goal.confidence = 70;
    Check(store.AddFact(goal, &error), "add a goal fact");
    std::vector<Fact> facts = store.Facts();
    Check(facts.size() == 1, "one active fact");
    if (facts.size() != 1) return 1;
    const std::wstring goalId = facts.front().id;
    Check(goalId.rfind(L"mem-", 0) == 0, "fact id has the mem- prefix");
    Check(facts.front().text == L"目标：完成 M1 助理", "fact text round-trips");
    Check(facts.front().kind == L"goal", "fact kind round-trips");
    Check(facts.front().sourceRef == L"dsh-project-ws-a", "fact source round-trips");
    Check(facts.front().createdMs == kNow - 1000, "explicit created timestamp is kept");

    // Same kind + text merges instead of duplicating.
    Fact duplicate = goal;
    duplicate.confidence = 90;
    Check(store.AddFact(duplicate, &error), "duplicate add is accepted");
    Check(store.Facts().size() == 1, "same kind+text is deduplicated");
    Check(store.Facts().front().confidence == 90, "dedupe raises confidence to the max");

    // Same text with a different kind stays a second fact.
    Check(store.AddFact(MakeFact(L"milestone", L"目标：完成 M1 助理", kNow - 500), &error),
          "same text with a different kind is a separate fact");
    Check(store.Facts().size() == 2, "different kinds are not deduplicated");

    // Persistence: a fresh store sees the same facts.
    {
        MemoryStore reloaded;
        Check(reloaded.Load(dir, &error), "reload facts");
        const std::vector<Fact> reloadedFacts = reloaded.Facts();
        Check(reloadedFacts.size() == 2, "facts persist across Load");
        bool found = false;
        for (const Fact& fact : reloadedFacts) {
            if (fact.id == goalId && fact.text == L"目标：完成 M1 助理") found = true;
        }
        Check(found, "persisted fact keeps id and text");
    }

    // --- correction and forgetting ---------------------------------------
    Check(store.UpdateFact(goalId, L"目标：完成 M1 记忆与上下文", &error),
          "correct a fact");
    {
        const std::vector<Fact> after = store.Facts();
        bool corrected = false;
        for (const Fact& fact : after) {
            if (fact.id == goalId) {
                corrected = fact.text == L"目标：完成 M1 记忆与上下文" &&
                            fact.confidence > 90;
            }
        }
        Check(corrected, "correction updates the text and raises confidence");
    }
    Check(!store.UpdateFact(L"mem-does-not-exist", L"x", &error),
          "correcting an unknown id fails");

    Check(store.DeleteFact(goalId, &error), "forget a fact");
    Check(store.Facts().size() == 1, "forgotten fact leaves Facts()");
    bool tombstone = false;
    for (const Fact& fact : store.AllFacts()) {
        if (fact.id == goalId && fact.status == L"deleted") tombstone = true;
    }
    Check(tombstone, "forget is a soft delete visible in AllFacts()");
    {
        MemoryStore reloaded;
        Check(reloaded.Load(dir, &error), "reload after forget");
        for (const Fact& fact : reloaded.Facts()) {
            Check(fact.id != goalId, "forgotten fact stays hidden after reload");
        }
    }
    Check(store.DeleteFact(L"mem-does-not-exist", &error) == false,
          "forgetting an unknown id fails");

    // --- sensitive rejection ---------------------------------------------
    Check(!store.AddFact(MakeFact(L"fact", L"路径：C:\\Users\\me\\.ssh\\id_rsa", kNow), &error),
          "fact with a sensitive path is rejected");
    Check(!store.AddFact(MakeFact(L"fact", L"token sk-abcdef123456", kNow), &error),
          "fact with an API key is rejected");
    Check(!store.AddFact(MakeFact(L"fact", L"PRIVATE KEY 内容", kNow), &error),
          "fact with a private key is rejected");
    Check(MemoryStore::IsSensitive(L"C:\\Users\\me\\.credentials.yaml"),
          "credential path is sensitive");
    Check(!MemoryStore::IsSensitive(L"用户偏好中文回答"),
          "plain Chinese text is not sensitive");

    // "sk-" is a token prefix, not a substring (pre-existing filter debt): the
    // MEMORY/assistant integration seeds sourceRef "desk-render-test", where
    // de[sk-]render used to be rejected as an API key.
    Check(!MemoryStore::IsSensitive(L"desk-render-test"),
          "desk- is an ordinary word, not an API key");
    Check(!MemoryStore::IsSensitive(L"task-run risk-free disk-usage"),
          "task-/risk-/disk- are ordinary words");
    Check(!MemoryStore::IsSensitive(L"seed source desk-render-test"),
          "a desk-render seed source is not sensitive");
    Check(!MemoryStore::IsSensitive(L"DESK-RENDER-TEST"),
          "the desk- exemption is case-insensitive too");
    Check(MemoryStore::IsSensitive(L"sk-abcdef123456"),
          "a token that starts with sk- is sensitive");
    Check(MemoryStore::IsSensitive(L"sk-ant-api03-xyz"),
          "an Anthropic-shaped key is sensitive");
    Check(MemoryStore::IsSensitive(L"sk-"), "a bare sk- prefix is sensitive");
    Check(MemoryStore::IsSensitive(L"SK-ABCDEF"),
          "the sk- check is case-insensitive");
    Check(MemoryStore::IsSensitive(L"key is sk-abcdef123456"),
          "sk- after whitespace is sensitive");
    Check(MemoryStore::IsSensitive(L"token=\"sk-abcdef123456\""),
          "sk- after a quote is sensitive");
    Check(MemoryStore::IsSensitive(L"token:\"sk-abcdef123456\""),
          "sk- after a colon and a quote is sensitive");
    Check(MemoryStore::IsSensitive(L"api=sk-abcdef123456"),
          "sk- after an equals sign is sensitive");
    Check(MemoryStore::IsSensitive(L"密钥sk-abcdef123456"),
          "CJK directly before sk- is still sensitive");

    // End to end: the desk-render shape may be stored, the key shape may not.
    Check(store.AddFact(MakeFact(L"fact", L"MEMORY 卡 seed 来自 desk-render-test", kNow,
                                 L"desk-render-test"),
                        &error),
          "a desk-render sourceRef and text are stored");
    bool deskStored = false;
    for (const Fact& fact : store.Facts()) {
        if (fact.sourceRef == L"desk-render-test") deskStored = true;
    }
    Check(deskStored, "the desk-render fact keeps its source on disk");
    Check(store.AppendEpisode(L"渲染测试怎么 seed？", L"用 desk-render-test",
                              L"desk-render-test", &error),
          "a desk-render episode is accepted");
    Check(!store.AddFact(MakeFact(L"fact", L"sk-abcdef123456", kNow), &error),
          "a bare API key is still rejected by AddFact");
    Check(!store.AppendEpisode(L"question", L"key sk-abcdef123456", L"", &error),
          "a real API key in an episode is still rejected");

    // --- cap ---------------------------------------------------------------
    {
        const std::wstring capDir = dir + L"\\cap";
        CreateDirectoryW(capDir.c_str(), nullptr);
        std::wstring lines;
        for (int i = 0; i < kFactCap; ++i) {
            lines += L"{\"id\":\"mem-bulk-" + Number(i) +
                     L"\",\"kind\":\"bulk\",\"text\":\"批量事实 " + Number(i) +
                     L"\",\"source\":\"\",\"confidence\":80,\"status\":\"active\","
                     L"\"created\":1700000000000,\"updated\":1700000000000,"
                     L"\"expires\":0}\n";
        }
        Check(WriteUtf8(capDir + L"\\assistant-facts.jsonl", lines),
              "cap fixture written");
        MemoryStore capped;
        Check(capped.Load(capDir, &error), "load a full facts file");
        Check(capped.Facts().size() == static_cast<std::size_t>(kFactCap),
              "cap fixture loads exactly kFactCap facts");
        Check(capped.AddFact(MakeFact(L"fact", L"新事实", kNow), &error),
              "one more fact is accepted");
        Check(capped.Facts().size() <= static_cast<std::size_t>(kFactCap),
              "fact count stays within kFactCap");
        Check(capped.AllFacts().size() <= static_cast<std::size_t>(kFactCap),
              "the file itself stays within kFactCap");
    }

    // --- Query ranking and 90-day decay ----------------------------------
    {
        const std::wstring queryDir = dir + L"\\query";
        CreateDirectoryW(queryDir.c_str(), nullptr);
        MemoryStore queryStore;
        Check(queryStore.Load(queryDir, &error), "query store loads");
        Check(queryStore.AddFact(MakeFact(L"preference", L"用户偏好深色主题", kNow - kDayMs), &error),
              "query fact A");
        Check(queryStore.AddFact(MakeFact(L"goal", L"旧目标：整理桌面组件", kNow - 100 * kDayMs), &error),
              "query fact B (stale)");
        Check(queryStore.AddFact(MakeFact(L"goal", L"新目标：完成 M1 助理", kNow - kDayMs), &error),
              "query fact C (fresh)");
        Check(queryStore.AddFact(MakeFact(L"preference", L"用户偏好中文回答", kNow - kDayMs), &error),
              "query fact D");

        const std::vector<Fact> goals = queryStore.Query(L"目标", 10, kNow);
        Check(goals.size() == 2, "query finds both goal facts");
        if (goals.size() == 2) {
            Check(goals.front().text == L"新目标：完成 M1 助理",
                  "the fresh fact outranks the 90-day-old fact");
        }
        const std::vector<Fact> prefs = queryStore.Query(L"偏好", 10, kNow);
        Check(prefs.size() == 2, "query finds both preference facts");
        Check(queryStore.Query(L"目标", 1, kNow).size() == 1, "query honours limit");
        Check(queryStore.Query(L"", 2, kNow).size() == 2, "empty query lists active facts");
        Check(queryStore.Query(L"没有这个词", 10, kNow).empty(),
              "unmatched query returns nothing");

        // A fact that has not been touched for 90+ days loses half its weight:
        // same keywords, fresh fact first, stale fact second.
        const std::vector<Fact> ranked = queryStore.Query(L"用户偏好", 10, kNow);
        Check(ranked.size() == 2, "both preference facts match");
    }

    // --- 90-day decay halves the score (memory.h / plan §3) --------------
    {
        const std::wstring decayDir = dir + L"\\decay";
        CreateDirectoryW(decayDir.c_str(), nullptr);
        MemoryStore decayStore;
        Check(decayStore.Load(decayDir, &error), "decay store loads");
        // Same confidence, different ages: the stale fact matches both query
        // tokens ("主题", "偏好"), the fresh one only "偏好". Only the 90-day
        // demotion keeps the fresh fact on top; adding half a score instead
        // (the old bug) inverted the ranking.
        Check(decayStore.AddFact(MakeFact(L"preference", L"深色主题 用户偏好",
                                          kNow - 100 * kDayMs), &error),
              "stale decay fact is stored");
        Check(decayStore.AddFact(MakeFact(L"fact", L"用户偏好中文回答",
                                          kNow - kDayMs), &error),
              "fresh decay fact is stored");
        const std::vector<Fact> decayed = decayStore.Query(L"主题 偏好", 10, kNow);
        Check(decayed.size() == 2, "both decay facts match");
        if (decayed.size() == 2) {
            Check(decayed.front().text == L"用户偏好中文回答",
                  "a 90-day-old fact is demoted, not promoted");
        }
    }

    // --- a failed write must not leave phantom facts in memory ------------
    {
        const std::wstring roDir = dir + L"\\readonly";
        CreateDirectoryW(roDir.c_str(), nullptr);
        MemoryStore roStore;
        Check(roStore.Load(roDir, &error), "read-only store loads");
        Check(roStore.AddFact(MakeFact(L"fact", L"已经保存的事实", kNow), &error),
              "baseline fact is written");
        const std::wstring savedId = roStore.Facts().front().id;
        const std::wstring factsFile = roDir + L"\\assistant-facts.jsonl";
        // An exclusive handle makes the atomic replace (temp + MoveFileEx)
        // fail without FILE_SHARE_DELETE, i.e. a write error on a healthy
        // looking path - the same shape as a locked/AV-scanned file.
        HANDLE factsLock = CreateFileW(factsFile.c_str(), GENERIC_READ, 0, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(factsLock != INVALID_HANDLE_VALUE, "facts file can be locked exclusively");
        if (factsLock == INVALID_HANDLE_VALUE) return 1;

        std::wstring writeError;
        Check(!roStore.AddFact(MakeFact(L"fact", L"写不进去的事实", kNow), &writeError),
              "add fails while the facts file is locked");
        Check(writeError == kFactWriteFailed, "the failed add reports a write error");
        Check(roStore.Facts().size() == 1,
              "a failed add leaves no phantom fact in memory");

        Check(!roStore.UpdateFact(savedId, L"改不进去的内容", &writeError),
              "update fails while the facts file is locked");
        bool textKept = false;
        for (const Fact& fact : roStore.Facts()) {
            if (fact.id == savedId && fact.text == L"已经保存的事实") textKept = true;
        }
        Check(textKept, "a failed update keeps the previous text in memory");

        Check(!roStore.DeleteFact(savedId, &writeError),
              "delete fails while the facts file is locked");
        bool stillActive = false;
        for (const Fact& fact : roStore.Facts()) {
            if (fact.id == savedId) stillActive = true;
        }
        Check(stillActive, "a failed delete keeps the fact active in memory");

        CloseHandle(factsLock);
        MemoryStore reloaded;
        Check(reloaded.Load(roDir, &error), "reload after the failed-write episode");
        const std::vector<Fact> afterReload = reloaded.Facts();
        Check(afterReload.size() == 1,
              "memory and assistant-facts.jsonl agree after a failed write");
        if (afterReload.size() == 1) {
            Check(afterReload.front().id == savedId &&
                      afterReload.front().text == L"已经保存的事实",
                  "the persisted fact is unchanged after the failed writes");
        }
    }

    // --- shared store registry (T06a) -------------------------------------
    {
        const std::wstring sharedDir = dir + L"\\shared";
        CreateDirectoryW(sharedDir.c_str(), nullptr);
        std::wstring sharedError;
        std::shared_ptr<MemoryStore> first = OpenSharedMemory(sharedDir, &sharedError);
        Check(first != nullptr, "OpenSharedMemory returns a store");
        Check(sharedError.empty(), "opening a fresh shared directory reports no error");
        if (first == nullptr) return 1;
        Check(first->config_dir() == sharedDir,
              "the shared store keeps the first requested directory spelling");

        std::shared_ptr<MemoryStore> sameDir = OpenSharedMemory(sharedDir + L"\\");
        Check(sameDir.get() == first.get(),
              "a trailing separator maps to the same shared store");
        std::shared_ptr<MemoryStore> sameCase =
            OpenSharedMemory(Upper(sharedDir) + L"/");
        Check(sameCase.get() == first.get(),
              "a different case and slash spelling maps to the same shared store");

        int sharedNotifies = 0;
        const uint64_t sharedToken =
            first->Subscribe([&sharedNotifies] { ++sharedNotifies; });
        Check(sharedToken != 0, "the shared store accepts a subscriber");
        Check(sameCase->AddFact(MakeFact(L"fact", L"共享事实", kNow), &sharedError),
              "a fact added through another spelling is stored");
        Check(sharedNotifies == 1,
              "both spellings share one store, so one subscriber list");
        first->Unsubscribe(sharedToken);

        std::shared_ptr<MemoryStore> otherDir =
            OpenSharedMemory(dir + L"\\shared-other");
        Check(otherDir.get() != first.get(), "a different directory gets its own store");

        std::wstring emptyError;
        Check(OpenSharedMemory(L"", &emptyError) == nullptr,
              "an empty directory cannot be shared");
        Check(emptyError == kMemoryDirEmpty, "an empty directory reports kMemoryDirEmpty");

        // The registry holds only weak_ptr: dropping every caller releases the
        // store, and the next call builds a fresh one from the files on disk.
        std::weak_ptr<MemoryStore> watched = first;
        first.reset();
        sameDir.reset();
        sameCase.reset();
        otherDir.reset();
        Check(watched.expired(), "the registry does not keep a store alive");
        std::shared_ptr<MemoryStore> reopened = OpenSharedMemory(sharedDir, &sharedError);
        Check(reopened != nullptr, "reopening after expiry creates a store");
        bool reloadedShared = false;
        if (reopened != nullptr) {
            for (const Fact& fact : reopened->Facts()) {
                if (fact.text == L"共享事实") reloadedShared = true;
            }
        }
        Check(reloadedShared, "the reopened store reloads the persisted facts");
        Check(OpenSharedMemory(sharedDir).get() == reopened.get(),
              "a loaded store is reused without reloading");
    }

    // --- a failed Load hands out nothing and is not cached -----------------
    {
        const std::wstring brokenDir = dir + L"\\broken";
        CreateDirectoryW(brokenDir.c_str(), nullptr);
        const std::wstring brokenFacts = brokenDir + L"\\assistant-facts.jsonl";
        Check(WriteUtf8(brokenFacts, LegacyFactLine(L"mem-legacy", L"遗留事实") +
                                         L"这不是 JSON\n"),
              "a facts file with one broken line is written");

        std::wstring brokenError;
        std::shared_ptr<MemoryStore> broken = OpenSharedMemory(brokenDir, &brokenError);
        Check(broken == nullptr,
              "a failed Load returns nullptr instead of a half-loaded store");
        Check(!brokenError.empty(), "a failed Load is reported through error");
        std::shared_ptr<MemoryStore> brokenRetry = OpenSharedMemory(brokenDir, &brokenError);
        Check(brokenRetry == nullptr, "a failed Load is not cached as a usable store");
        Check(!brokenError.empty(), "the retry still reports the load error");

        // Nothing was written: the unreadable file is exactly as it was.
        Check(ReadUtf8(brokenFacts).find(L"mem-legacy") != std::wstring::npos &&
                  ReadUtf8(brokenFacts).find(L"这不是 JSON") != std::wstring::npos,
              "a failed Load does not rewrite the facts file");

        Check(WriteUtf8(brokenFacts, LegacyFactLine(L"mem-fixed", L"修好的事实")),
              "the facts file is repaired");
        std::shared_ptr<MemoryStore> repaired = OpenSharedMemory(brokenDir, &brokenError);
        Check(repaired != nullptr, "the retry succeeds after the file is fixed");
        Check(brokenError.empty(), "the successful retry reports no error");
        bool sawFixed = false;
        if (repaired != nullptr) {
            for (const Fact& fact : repaired->Facts()) {
                if (fact.id == L"mem-fixed" && !fact.pinned) sawFixed = true;
            }
        }
        Check(sawFixed, "the repaired store reads the facts from disk");
        Check(OpenSharedMemory(brokenDir, &brokenError).get() == repaired.get(),
              "a store that loaded once is reused without reloading");

        // An unreadable file is a Load failure too: still no store, and the
        // directory loads normally once it can be read.
        const std::wstring lockDir = dir + L"\\broken-lock";
        CreateDirectoryW(lockDir.c_str(), nullptr);
        const std::wstring lockFacts = lockDir + L"\\assistant-facts.jsonl";
        Check(WriteUtf8(lockFacts, LegacyFactLine(L"mem-locked", L"锁住的事实")),
              "the locked-load fixture is written");
        HANDLE loadLock = CreateFileW(lockFacts.c_str(), GENERIC_READ, 0, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(loadLock != INVALID_HANDLE_VALUE, "the fixture facts file can be locked");
        if (loadLock != INVALID_HANDLE_VALUE) {
            std::wstring lockError;
            Check(OpenSharedMemory(lockDir, &lockError) == nullptr,
                  "an unreadable facts file makes OpenSharedMemory return nullptr");
            Check(!lockError.empty(), "the unreadable file reports an error");
            CloseHandle(loadLock);
            std::shared_ptr<MemoryStore> unlocked = OpenSharedMemory(lockDir, &lockError);
            Check(unlocked != nullptr, "the directory loads once the file is readable");
            Check(lockError.empty(), "the successful retry reports no error");
            bool sawLocked = false;
            if (unlocked != nullptr) {
                for (const Fact& fact : unlocked->Facts()) {
                    if (fact.id == L"mem-locked") sawLocked = true;
                }
            }
            Check(sawLocked, "the store loaded after unlocking keeps its facts");
        }
    }

    // --- pin persistence, ordering and legacy files (T06a) ----------------
    {
        const std::wstring pinDir = dir + L"\\pin";
        CreateDirectoryW(pinDir.c_str(), nullptr);
        const std::wstring pinFacts = pinDir + L"\\assistant-facts.jsonl";
        Check(WriteUtf8(pinFacts, LegacyFactLine(L"mem-legacy", L"遗留事实")),
              "legacy facts fixture written");

        MemoryStore pinStore;
        Check(pinStore.Load(pinDir, &error), "legacy facts load");
        const std::vector<Fact> legacy = pinStore.Facts();
        Check(legacy.size() == 1, "the legacy fact is loaded");
        Check(legacy.size() == 1 && legacy.front().id == L"mem-legacy",
              "the legacy fact keeps its id");
        Check(legacy.size() == 1 && !legacy.front().pinned,
              "a fact without a pinned field defaults to false");

        // Deliberately older than the legacy fact: only the pin can put it on
        // top of Facts().
        Fact pinnedFact = MakeFact(L"goal", L"置顶目标", kNow - 3000 * kDayMs);
        Check(pinStore.AddFact(pinnedFact, &error), "the pin fixture is stored");
        std::wstring pinnedId;
        for (const Fact& fact : pinStore.Facts()) {
            if (fact.text == L"置顶目标") pinnedId = fact.id;
        }
        Check(!pinnedId.empty(), "the pin fixture has an id");

        Check(pinStore.SetPinned(pinnedId, true, &error), "a fact can be pinned");
        Check(ReadUtf8(pinFacts).find(L"\"pinned\":true") != std::wstring::npos,
              "a pin is written to the facts file");
        const std::vector<Fact> ordered = pinStore.Facts();
        Check(ordered.size() == 2, "both pin facts are active");
        Check(ordered.size() == 2 && ordered.front().id == pinnedId,
              "a pinned fact sorts before a newer unpinned fact");
        Check(ordered.size() == 2 && ordered.back().id == L"mem-legacy",
              "unpinned facts keep the newest-first order");

        // A re-observed fact must not silently unpin what the user pinned.
        Fact observedAgain = MakeFact(L"goal", L"置顶目标", kNow);
        observedAgain.pinned = false;
        Check(pinStore.AddFact(observedAgain, &error), "re-observing a pinned fact merges");
        bool stillPinned = false;
        for (const Fact& fact : pinStore.Facts()) {
            if (fact.id == pinnedId) stillPinned = fact.pinned;
        }
        Check(stillPinned, "a merge keeps the user's pin");

        {
            MemoryStore pinReloaded;
            Check(pinReloaded.Load(pinDir, &error), "reload pinned facts");
            bool reloadedPin = false;
            for (const Fact& fact : pinReloaded.Facts()) {
                if (fact.id == pinnedId) reloadedPin = fact.pinned;
            }
            Check(reloadedPin, "the pin survives a reload");
        }

        // A failed write must leave the pin alone, in memory and on disk.
        HANDLE pinLock = CreateFileW(pinFacts.c_str(), GENERIC_READ, 0, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(pinLock != INVALID_HANDLE_VALUE, "the pinned facts file can be locked");
        std::wstring pinError;
        if (pinLock != INVALID_HANDLE_VALUE) {
            Check(!pinStore.SetPinned(pinnedId, false, &pinError),
                  "unpinning fails while the facts file is locked");
            Check(pinError == kFactWriteFailed, "the failed pin reports a write error");
            bool pinKept = false;
            for (const Fact& fact : pinStore.Facts()) {
                if (fact.id == pinnedId) pinKept = fact.pinned;
            }
            Check(pinKept, "a failed pin keeps the in-memory pin");
            CloseHandle(pinLock);
        }
        {
            MemoryStore pinDisk;
            Check(pinDisk.Load(pinDir, &error), "reload after the failed pin");
            bool diskPinKept = false;
            for (const Fact& fact : pinDisk.Facts()) {
                if (fact.id == pinnedId) diskPinKept = fact.pinned;
            }
            Check(diskPinKept, "a failed pin leaves the facts file unchanged");
        }

        Check(pinStore.SetPinned(pinnedId, false, &error), "a fact can be unpinned");
        Check(ReadUtf8(pinFacts).find(L"\"pinned\":true") == std::wstring::npos,
              "unpinning clears the pinned flag in the file");

        Check(!pinStore.SetPinned(L"mem-nope", true, &pinError) &&
                  pinError == kFactNotFound,
              "pinning an unknown id fails");
        Check(pinStore.DeleteFact(pinnedId, &error), "the pin fixture can be forgotten");
        Check(!pinStore.SetPinned(pinnedId, true, &pinError) &&
                  pinError == kFactNotFound,
              "a forgotten fact cannot be pinned");
    }

    // --- change notification (T06a) ---------------------------------------
    {
        const std::wstring notifyDir = dir + L"\\notify";
        CreateDirectoryW(notifyDir.c_str(), nullptr);
        const std::wstring notifyFacts = notifyDir + L"\\assistant-facts.jsonl";
        MemoryStore notifyStore;
        Check(notifyStore.Load(notifyDir, &error), "the notify store loads");

        int notified = 0;
        const uint64_t token = notifyStore.Subscribe([&notified] { ++notified; });
        Check(token != 0, "Subscribe returns a non-zero token");
        Check(notifyStore.Subscribe(MemoryStore::ChangeHandler()) == 0,
              "an empty handler registers nothing");

        Check(notifyStore.AddFact(MakeFact(L"fact", L"通知事实", kNow), &error),
              "an add notifies");
        Check(notified == 1, "AddFact notifies exactly once");
        std::wstring notifyId;
        for (const Fact& fact : notifyStore.Facts()) {
            if (fact.text == L"通知事实") notifyId = fact.id;
        }
        Check(!notifyId.empty(), "the notified fact has an id");
        Check(notifyStore.UpdateFact(notifyId, L"通知事实二", &error),
              "an update notifies");
        Check(notified == 2, "UpdateFact notifies exactly once");
        Check(notifyStore.DeleteFact(notifyId, &error), "a delete notifies");
        Check(notified == 3, "DeleteFact notifies exactly once");
        Check(notifyStore.DeleteFact(notifyId, &error),
              "deleting an already forgotten fact still succeeds");
        Check(notified == 3, "an idempotent delete does not notify");

        Check(notifyStore.AddFact(MakeFact(L"fact", L"置顶通知事实", kNow), &error),
              "the pin notification fixture is stored");
        Check(notified == 4, "the pin fixture add notifies");
        std::wstring pinNotifyId;
        for (const Fact& fact : notifyStore.Facts()) {
            if (fact.text == L"置顶通知事实") pinNotifyId = fact.id;
        }
        Check(notifyStore.SetPinned(pinNotifyId, true, &error), "pinning notifies");
        Check(notified == 5, "SetPinned notifies exactly once");
        Check(notifyStore.SetPinned(pinNotifyId, true, &error),
              "pinning an already pinned fact succeeds");
        Check(notified == 5, "an idempotent pin neither rewrites nor notifies");
        Check(notifyStore.SetPinned(pinNotifyId, false, &error), "unpinning notifies");
        Check(notified == 6, "unpinning notifies exactly once");

        // A failed write rolls back and stays silent.
        HANDLE notifyLock = CreateFileW(notifyFacts.c_str(), GENERIC_READ, 0, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(notifyLock != INVALID_HANDLE_VALUE, "the notify facts file can be locked");
        if (notifyLock != INVALID_HANDLE_VALUE) {
            std::wstring writeError;
            Check(!notifyStore.AddFact(MakeFact(L"fact", L"写盘失败的事实", kNow),
                                       &writeError),
                  "an add fails while the facts file is locked");
            Check(notified == 6, "a failed add does not notify");
            Check(!notifyStore.SetPinned(pinNotifyId, true, &writeError),
                  "a pin fails while the facts file is locked");
            Check(notified == 6, "a failed pin does not notify");
            CloseHandle(notifyLock);
        }

        // Unsubscribe drops future notifications only.
        notifyStore.Unsubscribe(token);
        Check(notifyStore.AddFact(MakeFact(L"fact", L"取消订阅后的事实", kNow), &error),
              "the store still works after Unsubscribe");
        Check(notified == 6, "an unsubscribed token is not called");

        // A handler may read the store (no lock is held) and must see the
        // change already on disk: notify only after a real write.
        int handlerFactCount = -1;
        bool handlerSawDisk = false;
        const uint64_t readingToken = notifyStore.Subscribe([&] {
            handlerFactCount = static_cast<int>(notifyStore.Facts().size());
            handlerSawDisk = ReadUtf8(notifyFacts).find(L"回调事实") != std::wstring::npos;
        });
        Check(readingToken != 0, "the reading handler is registered");
        Check(notifyStore.AddFact(MakeFact(L"fact", L"回调事实", kNow), &error),
              "an add with a reading handler succeeds");
        Check(handlerFactCount > 0, "a handler can call Facts() without deadlocking");
        Check(handlerSawDisk, "the notification fires after the fact is on disk");
        notifyStore.Unsubscribe(readingToken);

        // A throwing subscriber must not undo the write or starve the rest.
        int later = 0;
        const uint64_t throwToken = notifyStore.Subscribe(
            [] { throw std::runtime_error("subscriber boom"); });
        const uint64_t laterToken = notifyStore.Subscribe([&later] { ++later; });
        Check(throwToken != 0 && laterToken != 0, "both subscribers are registered");
        Check(notifyStore.AddFact(MakeFact(L"fact", L"异常订阅者事实", kNow), &error),
              "a throwing subscriber does not fail the write");
        Check(later == 1, "subscribers after a throwing one still run");
        bool storedAfterThrow = false;
        for (const Fact& fact : notifyStore.Facts()) {
            if (fact.text == L"异常订阅者事实") storedAfterThrow = true;
        }
        Check(storedAfterThrow, "the fact survives a throwing subscriber");
        notifyStore.Unsubscribe(throwToken);
        notifyStore.Unsubscribe(laterToken);

        // Unsubscribing from inside a handler (re-entrant) must not deadlock.
        int selfCalls = 0;
        uint64_t selfToken = 0;
        std::function<void()> selfRemoving;
        selfRemoving = [&] {
            ++selfCalls;
            notifyStore.Unsubscribe(selfToken);
        };
        selfToken = notifyStore.Subscribe(selfRemoving);
        Check(notifyStore.AddFact(MakeFact(L"fact", L"自卸载订阅者事实", kNow), &error),
              "a self-unsubscribing handler is allowed");
        Check(selfCalls == 1, "a handler can unsubscribe itself");
        Check(notifyStore.AddFact(MakeFact(L"fact", L"自卸载后的事实", kNow), &error),
              "the store keeps working after the handler removed itself");
        Check(selfCalls == 1, "the removed handler is not called again");

        // In-flight notifications: Unsubscribe does not wait, so a handler may
        // still run once with its token gone. This only proves the store stays
        // consistent under Subscribe/AddFact/Unsubscribe churn; the UI's
        // weak-state rule is documented in memory.h.
        std::atomic<int> raceCalls{0};
        std::thread producer([&] {
            for (int i = 0; i < 20; ++i) {
                const std::wstring text = L"并发事实 " + Number(i);
                notifyStore.AddFact(MakeFact(L"fact", text.c_str(), kNow + i), nullptr);
            }
        });
        for (int i = 0; i < 200; ++i) {
            const uint64_t raceToken = notifyStore.Subscribe([&raceCalls] { ++raceCalls; });
            notifyStore.Unsubscribe(raceToken);
        }
        producer.join();
        Check(notifyStore.Facts().size() >= 20,
              "the store stays consistent while subscribers churn");
    }

    // --- forgetting then remembering the same text again -------------------
    {
        const std::wstring reviveDir = dir + L"\\revive";
        CreateDirectoryW(reviveDir.c_str(), nullptr);
        MemoryStore reviveStore;
        Check(reviveStore.Load(reviveDir, &error), "the revive store loads");

        int reviveNotifies = 0;
        reviveStore.Subscribe([&reviveNotifies] { ++reviveNotifies; });

        Check(reviveStore.AddFact(MakeFact(L"preference", L"用户偏好深色主题", kNow), &error),
              "the revive fixture is stored");
        Check(reviveNotifies == 1, "the revive fixture add notifies once");
        std::wstring forgottenId;
        for (const Fact& fact : reviveStore.Facts()) {
            if (fact.text == L"用户偏好深色主题") forgottenId = fact.id;
        }
        Check(!forgottenId.empty(), "the revive fixture has an id");

        Check(reviveStore.DeleteFact(forgottenId, &error), "the fixture is forgotten");
        Check(reviveNotifies == 2, "forgetting notifies once");
        Check(reviveStore.Facts().empty(), "the forgotten fact leaves Facts()");

        // Remembering the same kind+text must create a new active fact; the
        // soft-deleted tombstone stays inspectable under its old id.
        Check(reviveStore.AddFact(MakeFact(L"preference", L"用户偏好深色主题", kNow), &error),
              "remembering the same text again is accepted");
        Check(reviveNotifies == 3, "re-remembering notifies once");
        const std::vector<Fact> revived = reviveStore.Facts();
        Check(revived.size() == 1, "the re-remembered fact is active on its own");
        if (revived.size() == 1) {
            Check(revived.front().id != forgottenId, "re-remembering gets a new id");
            Check(revived.front().status == L"active", "the new fact is active");
            Check(revived.front().text == L"用户偏好深色主题", "the new fact keeps the text");
        }
        Check(reviveStore.AllFacts().size() == 2,
              "the tombstone and the new fact both exist");
        bool tombstoneKept = false;
        for (const Fact& fact : reviveStore.AllFacts()) {
            if (fact.id == forgottenId && fact.status == L"deleted") tombstoneKept = true;
        }
        Check(tombstoneKept, "the forgotten id stays a deleted tombstone");

        // The same shape is what is on disk.
        {
            MemoryStore reviveReload;
            Check(reviveReload.Load(reviveDir, &error), "reload after the re-remember");
            Check(reviveReload.Facts().size() == 1,
                  "only the new fact is active after reload");
            bool reloadedTombstone = false;
            for (const Fact& fact : reviveReload.AllFacts()) {
                if (fact.id == forgottenId && fact.status == L"deleted") {
                    reloadedTombstone = true;
                }
            }
            Check(reloadedTombstone, "the tombstone survives the reload");
        }
    }

    // --- ClearAll: facts + episodes, tombstones, notification (M1-U) -------
    // The MEMORY card's "Clear all memory" and the engine's local "清空" both
    // go through this one call. It is deliberately *not* a two-file
    // transaction, so the tests below pin down what it does promise: success
    // clears both files and the in-memory lists, one notification, a failed
    // rewrite keeps memory intact and never claims success.
    {
        const std::wstring clearDir = dir + L"\\clear";
        CreateDirectoryW(clearDir.c_str(), nullptr);
        const std::wstring clearFacts = clearDir + L"\\assistant-facts.jsonl";
        const std::wstring clearEpisodes = clearDir + L"\\assistant-episodes.jsonl";
        std::wstring clearError;
        MemoryStore clearStore;
        Check(clearStore.Load(clearDir, &clearError), "the clear store loads");

        int clearNotifies = 0;
        clearStore.Subscribe([&clearNotifies] { ++clearNotifies; });

        Check(clearStore.AddFact(MakeFact(L"goal", L"清空事实一", kNow), &clearError),
              "clear fixture fact one is stored");
        Check(clearStore.AddFact(MakeFact(L"pref", L"清空事实二", kNow - 1000), &clearError),
              "clear fixture fact two is stored");
        Check(clearStore.AddFact(MakeFact(L"fact", L"将要遗忘的事实", kNow - 2000), &clearError),
              "clear fixture tombstone fact is stored");
        std::wstring tombstoneId;
        for (const Fact& fact : clearStore.Facts()) {
            if (fact.text == L"将要遗忘的事实") tombstoneId = fact.id;
        }
        Check(!tombstoneId.empty(), "the clear fixture has a tombstone id");
        Check(clearStore.DeleteFact(tombstoneId, &clearError),
              "the clear fixture tombstone is forgotten");
        Check(clearStore.AllFacts().size() == 3,
              "the fixture has two active facts and one tombstone");
        Check(clearStore.AppendEpisode(L"清空前的问题", L"清空前的回答", L"", &clearError),
              "clear fixture episode one is stored");
        Check(clearStore.AppendEpisode(L"清空前的第二问", L"清空前的第二答", L"", &clearError),
              "clear fixture episode two is stored");
        Check(clearStore.RecentEpisodes(0).size() == 2, "the fixture has two episodes");
        const int notifiesBeforeClear = clearNotifies;

        Check(clearStore.ClearAll(&clearError), "ClearAll succeeds on a populated store");
        Check(clearError.empty(), "a successful clear reports no error");
        Check(clearStore.Facts().empty(), "ClearAll empties Facts()");
        Check(clearStore.AllFacts().empty(),
              "ClearAll drops the soft-deleted tombstone too, not only active facts");
        Check(clearStore.RecentEpisodes(0).empty(), "ClearAll empties the episode log");
        Check(clearNotifies == notifiesBeforeClear + 1,
              "ClearAll notifies subscribers exactly once");
        Check(ReadUtf8(clearFacts).empty(), "the facts file is empty after the clear");
        Check(ReadUtf8(clearEpisodes).empty(), "the episode file is empty after the clear");

        // Reload from disk: nothing comes back, not even the tombstone.
        {
            MemoryStore clearReloaded;
            Check(clearReloaded.Load(clearDir, &clearError), "reload after ClearAll");
            Check(clearReloaded.AllFacts().empty(),
                  "a reloaded store has no facts and no tombstones");
            Check(clearReloaded.RecentEpisodes(0).empty(),
                  "a reloaded store has no episodes");
            Check(clearReloaded.Facts().empty() && clearReloaded.AllFacts().empty(),
                  "Facts() and AllFacts() agree after the reload");
        }

        // The store stays usable, and a cleared tombstone cannot come back:
        // re-remembering its text is a plain insert with a fresh id, and there
        // is no stale tombstone left to inspect in AllFacts().
        Check(clearStore.AddFact(MakeFact(L"fact", L"将要遗忘的事实", kNow + 1000), &clearError),
              "the store accepts a fact after a clear");
        Check(clearStore.Facts().size() == 1, "the post-clear fact is the only active one");
        Check(clearStore.AllFacts().size() == 1,
              "the cleared tombstone was removed, so nothing is merged or revived");
        if (clearStore.Facts().size() == 1) {
            Check(clearStore.Facts().front().id != tombstoneId,
                  "the post-clear fact gets a fresh id");
        }
        Check(clearStore.AppendEpisode(L"清空后的问题", L"清空后的回答", L"", &clearError),
              "episodes can be appended after a clear");
        {
            MemoryStore afterClear;
            Check(afterClear.Load(clearDir, &clearError), "reload after the post-clear writes");
            Check(afterClear.Facts().size() == 1 && afterClear.AllFacts().size() == 1,
                  "only the post-clear fact survives the reload");
            Check(afterClear.RecentEpisodes(0).size() == 1,
                  "only the post-clear episode survives the reload");
        }

        // Clearing again is still a successful clear, and the notification is
        // also the engine's "forget the old Pi session" signal - a session may
        // contain cancelled or failed turns that are not visible in
        // facts/episodes - so there is deliberately no "no data changed" early
        // return. First the honest non-empty case: the post-clear writes above
        // left one fact and one episode, they must be gone afterwards.
        const int beforeSecond = clearNotifies;
        Check(clearStore.Facts().size() == 1 && clearStore.RecentEpisodes(0).size() == 1,
              "the second-clear fixture has one fact and one episode again");
        Check(clearStore.ClearAll(&clearError), "a second ClearAll succeeds");
        Check(clearStore.Facts().empty() && clearStore.AllFacts().empty(),
              "the second clear removes the re-added fact");
        Check(clearStore.RecentEpisodes(0).empty(),
              "the second clear removes the re-added episode");
        Check(clearNotifies == beforeSecond + 1,
              "the second clear notifies exactly once");

        // And now the explicitly empty case: a clear of an already-empty store
        // still notifies exactly once (same session-barrier rule), while the
        // data stays empty.
        const int beforeThird = clearNotifies;
        Check(clearStore.Facts().empty() && clearStore.RecentEpisodes(0).empty(),
              "the store is empty before the already-empty clear");
        Check(clearStore.ClearAll(&clearError), "an already-empty ClearAll succeeds");
        Check(clearStore.Facts().empty() && clearStore.RecentEpisodes(0).empty(),
              "the already-empty clear leaves the store empty");
        Check(clearNotifies == beforeThird + 1,
              "an already-empty clear still notifies exactly once");

        // A first-run directory has no files at all: clearing must still work
        // instead of failing on the missing path, and it must still notify -
        // even though there was nothing to clear - so the engine can drop a Pi
        // session that only lived in memory.
        const std::wstring virginDir = dir + L"\\clear-virgin";
        CreateDirectoryW(virginDir.c_str(), nullptr);
        MemoryStore virgin;
        Check(virgin.Load(virginDir, &clearError), "the virgin clear store loads");
        int virginNotifies = 0;
        virgin.Subscribe([&virginNotifies] { ++virginNotifies; });
        Check(virginNotifies == 0, "loading does not notify");
        Check(virgin.ClearAll(&clearError), "ClearAll succeeds without existing files");
        Check(virginNotifies == 1,
              "an already-empty ClearAll still notifies exactly once (session barrier)");
        Check(virgin.Facts().empty() && virgin.RecentEpisodes(0).empty(),
              "an already-empty ClearAll leaves memory empty");
        Check(ReadUtf8(virginDir + L"\\assistant-facts.jsonl").empty() &&
                  ReadUtf8(virginDir + L"\\assistant-episodes.jsonl").empty(),
              "a first-run clear writes empty files");
    }

    // --- ClearAll failure: no false success, memory + disk kept, no notify --
    {
        const std::wstring failDir = dir + L"\\clear-fail";
        CreateDirectoryW(failDir.c_str(), nullptr);
        const std::wstring failFacts = failDir + L"\\assistant-facts.jsonl";
        const std::wstring failEpisodes = failDir + L"\\assistant-episodes.jsonl";
        std::wstring failError;
        MemoryStore failStore;
        Check(failStore.Load(failDir, &failError), "the failing clear store loads");
        int failNotifies = 0;
        failStore.Subscribe([&failNotifies] { ++failNotifies; });
        Check(failStore.AddFact(MakeFact(L"goal", L"清空失败事实一", kNow), &failError),
              "failing-clear fact one is stored");
        Check(failStore.AddFact(MakeFact(L"pref", L"清空失败事实二", kNow - 1000), &failError),
              "failing-clear fact two is stored");
        Check(failStore.AppendEpisode(L"清空失败的问", L"清空失败的答", L"", &failError),
              "failing-clear episode is stored");
        const int notifiesBeforeFailure = failNotifies;

        // The facts file is locked: the very first rewrite cannot replace it.
        HANDLE factsLock = CreateFileW(failFacts.c_str(), GENERIC_READ, 0, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(factsLock != INVALID_HANDLE_VALUE, "the facts file can be locked for ClearAll");
        if (factsLock != INVALID_HANDLE_VALUE) {
            Check(!failStore.ClearAll(&failError), "ClearAll fails while the facts file is locked");
            Check(failError == kFactWriteFailed, "the failed clear reports a write error");
            Check(failStore.Facts().size() == 2, "a failed clear keeps the facts in memory");
            Check(failStore.RecentEpisodes(0).size() == 1,
                  "a failed clear keeps the episodes in memory");
            Check(failNotifies == notifiesBeforeFailure, "a failed clear does not notify");
            CloseHandle(factsLock);
        }
        // Read after unlocking: the exclusive handle above also blocks a shared
        // read, so reading inside the block would only prove the lock works.
        Check(ReadUtf8(failFacts).find(L"清空失败事实一") != std::wstring::npos,
              "a failed clear leaves the facts file unchanged");
        Check(ReadUtf8(failEpisodes).find(L"清空失败的问") != std::wstring::npos,
              "a failed clear leaves the episode file unchanged");

        // Only the episode file is locked: the facts file has already been
        // truncated, which is the ordered-partial path. The store must restore
        // the facts file from the untouched snapshot, keep memory, and still
        // report the failure instead of pretending the clear worked.
        HANDLE episodesLock = CreateFileW(failEpisodes.c_str(), GENERIC_READ, 0, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(episodesLock != INVALID_HANDLE_VALUE,
              "the episode file can be locked for ClearAll");
        if (episodesLock != INVALID_HANDLE_VALUE) {
            Check(!failStore.ClearAll(&failError),
                  "ClearAll fails while the episode file is locked");
            Check(failError == kFactWriteFailed, "the partial clear reports a write error");
            Check(failStore.Facts().size() == 2 && failStore.RecentEpisodes(0).size() == 1,
                  "the partial clear keeps memory intact");
            Check(failNotifies == notifiesBeforeFailure, "the partial clear does not notify");
            CloseHandle(episodesLock);
        }
        const std::wstring restored = ReadUtf8(failFacts);
        Check(restored.find(L"清空失败事实一") != std::wstring::npos &&
                  restored.find(L"清空失败事实二") != std::wstring::npos,
              "the already-truncated facts file is restored from the snapshot");
        Check(ReadUtf8(failEpisodes).find(L"清空失败的问") != std::wstring::npos,
              "the locked episode file keeps its episodes");

        // Healthy again: the same call now succeeds, notifies once and the disk
        // agrees with the in-memory result.
        Check(failStore.ClearAll(&failError), "ClearAll succeeds once the locks are gone");
        Check(failError.empty(), "the recovered clear reports no error");
        Check(failNotifies == notifiesBeforeFailure + 1,
              "the recovered clear notifies exactly once");
        {
            MemoryStore failReloaded;
            Check(failReloaded.Load(failDir, &failError), "reload after the recovered clear");
            Check(failReloaded.AllFacts().empty() && failReloaded.RecentEpisodes(0).empty(),
                  "a recovered clear leaves both files empty");
        }
    }

    // --- reset revision: durable clear witness across restarts (M1-U) ------
    // The engine folds ResetRevision() into its session digest. A clear must
    // invalidate an old Pi session even when facts *and* episodes were already
    // empty (the session itself is then the only history) and even after a
    // restart, which a digest over the data files alone cannot express.
    {
        const std::wstring revDir = dir + L"\\clear-revision";
        CreateDirectoryW(revDir.c_str(), nullptr);
        const std::wstring revFile = revDir + L"\\assistant-memory-reset.txt";
        std::wstring revError;
        MemoryStore revStore;
        Check(revStore.Load(revDir, &revError), "the revision store loads");
        Check(revStore.ResetRevision().empty(),
              "a first run has no reset revision (a missing file is empty)");

        // An already-empty clear still advances the revision: this is exactly
        // the "only the old Pi session has history" case.
        Check(revStore.ClearAll(&revError), "the first empty clear succeeds");
        const std::wstring firstRevision = revStore.ResetRevision();
        Check(!firstRevision.empty(), "a clear persists a reset revision");
        Check(ReadUtf8(revFile).find(firstRevision) != std::wstring::npos,
              "the reset revision is written to assistant-memory-reset.txt");

        Check(revStore.ClearAll(&revError), "the second empty clear succeeds");
        const std::wstring secondRevision = revStore.ResetRevision();
        Check(!secondRevision.empty() && secondRevision != firstRevision,
              "every clear advances the revision, even with no facts/episodes");
        {
            MemoryStore revReloaded;
            Check(revReloaded.Load(revDir, &revError), "reload after the empty clears");
            Check(revReloaded.ResetRevision() == secondRevision,
                  "the reset revision survives a reload (cross-store persistence)");
            Check(revReloaded.ResetRevision() != firstRevision,
                  "a restart reads the newest revision, not the first one");
        }

        // A failed *data* clear must not fake success, but the revision it
        // already wrote is a conservative invalidation: it may have advanced,
        // the facts stay, and nobody is notified.
        const std::wstring revisionBeforeEdit = revStore.ResetRevision();
        Check(revStore.AddFact(MakeFact(L"goal", L"重设代号事实", kNow), &revError),
              "the revision failure fixture fact is stored");
        Check(revStore.ResetRevision() == revisionBeforeEdit,
              "a data edit does not change the reset revision (only a clear does)");
        int revNotifies = 0;
        revStore.Subscribe([&revNotifies] { ++revNotifies; });
        const int notifiesBeforeDataFailure = revNotifies;
        const std::wstring revisionBeforeDataFailure = revStore.ResetRevision();
        const std::wstring revFacts = revDir + L"\\assistant-facts.jsonl";
        HANDLE dataLock = CreateFileW(revFacts.c_str(), GENERIC_READ, 0, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(dataLock != INVALID_HANDLE_VALUE,
              "the revision fixture facts file can be locked");
        if (dataLock != INVALID_HANDLE_VALUE) {
            Check(!revStore.ClearAll(&revError),
                  "a clear fails while the facts file is locked");
            Check(revError == kFactWriteFailed,
                  "the partially applied clear reports a write error");
            Check(revStore.Facts().size() == 1,
                  "the failed data clear keeps the fact in memory");
            Check(revNotifies == notifiesBeforeDataFailure,
                  "the failed data clear does not notify");
            Check(revStore.ResetRevision() != revisionBeforeDataFailure,
                  "the already-written revision advances (conservative session invalidation)");
            CloseHandle(dataLock);
        }
        {
            MemoryStore afterDataFailure;
            Check(afterDataFailure.Load(revDir, &revError),
                  "reload after the failed data clear");
            Check(afterDataFailure.Facts().size() == 1,
                  "the data file still holds the fact (no false success)");
            Check(afterDataFailure.ResetRevision() == revStore.ResetRevision(),
                  "the advanced revision is what a restart reads");
        }

        // If even the revision cannot be written, the clear is rejected before
        // any data is touched: revision unchanged, facts kept, no notification.
        const std::wstring stableRevision = revStore.ResetRevision();
        HANDLE revLock = CreateFileW(revFile.c_str(), GENERIC_READ, 0, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(revLock != INVALID_HANDLE_VALUE, "the reset file can be locked");
        if (revLock != INVALID_HANDLE_VALUE) {
            const int notifiesBeforeResetFailure = revNotifies;
            Check(!revStore.ClearAll(&revError),
                  "a clear fails while the reset file is locked");
            Check(revError == kFactWriteFailed,
                  "the blocked revision write reports a write error");
            Check(revStore.ResetRevision() == stableRevision,
                  "a rejected clear leaves the revision unchanged");
            Check(revStore.Facts().size() == 1,
                  "a rejected revision write keeps the facts");
            Check(revNotifies == notifiesBeforeResetFailure,
                  "a rejected revision write does not notify");
            CloseHandle(revLock);
        }

        // An unreadable reset file must be visible: Load fails (and the shared
        // opener returns nullptr) instead of silently reporting "never
        // cleared", which would let an old session survive a restart.
        HANDLE readLock = CreateFileW(revFile.c_str(), GENERIC_READ, 0, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(readLock != INVALID_HANDLE_VALUE,
              "the reset file can be locked for the read case");
        if (readLock != INVALID_HANDLE_VALUE) {
            MemoryStore unreadable;
            std::wstring readError;
            Check(!unreadable.Load(revDir, &readError),
                  "an unreadable reset file makes Load fail instead of dropping the revision");
            Check(!readError.empty(), "the unreadable reset file is reported through error");
            std::wstring sharedError;
            Check(OpenSharedMemory(revDir, &sharedError) == nullptr,
                  "an unreadable reset file makes OpenSharedMemory fail closed");
            CloseHandle(readLock);
            MemoryStore repaired;
            Check(repaired.Load(revDir, &revError),
                  "the directory loads once the reset file is readable again");
            Check(repaired.ResetRevision() == stableRevision,
                  "the repaired load keeps the persisted revision");
        }
    }

    DeleteTree(dir);
    std::printf("[assistant-memory-test] %s\n", failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
