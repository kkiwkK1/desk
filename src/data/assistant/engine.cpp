#include "data/assistant/engine.h"

#include "data/aimeter/accounts.h"
#include "widget/store.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace assistant {
namespace {

constexpr int kContextBudget = 6000;
constexpr const wchar_t* kPersona = L"persona.md";
constexpr const wchar_t* kSessionFile = L"assistant-session.ini";
constexpr const wchar_t* kImportFile = L"assistant-import.ini";
constexpr int kMaxImportedProjects = 8;
constexpr std::size_t kMinSecretChars = 6;  // shorter env values are not keys

// Thread-local owner of the engine worker that is currently running. Ask()
// refuses a re-entrant call from that thread *before* touching any state, so
// every std::thread stays joinable: there is no detach escape hatch and no
// callback can leave a bare 'this' running after Engine is destroyed.
thread_local const void* g_workerOwner = nullptr;

struct WorkerThreadScope {
    const void* previous;
    explicit WorkerThreadScope(const void* owner) : previous(g_workerOwner) {
        g_workerOwner = owner;
    }
    ~WorkerThreadScope() { g_workerOwner = previous; }
    WorkerThreadScope(const WorkerThreadScope&) = delete;
    WorkerThreadScope& operator=(const WorkerThreadScope&) = delete;
};

long long NowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER v{};
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return (long long)(v.QuadPart / 10000ULL) - 11644473600000LL;
}

std::wstring LocalDay(long long nowMs) {
    const long long ticks = (nowMs + 11644473600000LL) * 10000LL;
    FILETIME ft{};
    ft.dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFFLL);
    ft.dwHighDateTime = (DWORD)((unsigned long long)ticks >> 32);
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&ft, &utc) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        return L"1970-01-01";
    }
    wchar_t buf[16];
    swprintf(buf, 16, L"%04d-%02d-%02d", local.wYear, local.wMonth, local.wDay);
    return buf;
}

std::wstring Trim(const std::wstring& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    auto space = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f';
    };
    while (begin < end && space(text[begin])) ++begin;
    while (end > begin && space(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::wstring Lower(std::wstring text) {
    for (wchar_t& c : text) c = (wchar_t)std::towlower(c);
    return text;
}

std::wstring Hex64(unsigned long long value) {
    wchar_t buffer[24];
    swprintf(buffer, 24, L"%016llx", value);
    return buffer;
}

unsigned long long Fnv(const std::wstring& text, unsigned long long state) {
    for (wchar_t c : text) {
        state ^= (unsigned long long)c;
        state *= 1099511628211ULL;
    }
    return state;
}

// The memory lane owns MemoryStore::ResetRevision's exact counter type (an
// integral revision or already-textual token); the engine only needs a stable
// value to compare between runs, so both shapes are accepted here.
template <typename T>
std::wstring RevisionText(const T& value) {
    if constexpr (std::is_integral_v<T>) {
        return std::to_wstring(value);
    } else {
        return std::wstring(value);
    }
}

bool FileExists(const std::wstring& path) {
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    std::wstring partial;
    for (std::size_t i = 0; i < path.size(); ++i) {
        partial += path[i];
        const bool separator = path[i] == L'\\' || path[i] == L'/';
        if (!separator && i + 1 != path.size()) continue;
        std::wstring current = partial;
        while (!current.empty() && (current.back() == L'\\' || current.back() == L'/')) {
            current.pop_back();
        }
        if (current.empty()) continue;
        if (current.size() == 2 && current[1] == L':') continue;
        if (!CreateDirectoryW(current.c_str(), nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) return false;
        }
    }
    return true;
}

std::wstring ReadTextFile(const std::wstring& path) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return L"";
    std::string bytes;
    char buffer[8192];
    DWORD got = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        bytes.append(buffer, got);
        if (bytes.size() > (256u << 10)) break;  // persona-sized, not a data dump
    }
    CloseHandle(handle);
    if (bytes.empty()) return L"";
    if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF &&
        (unsigned char)bytes[1] == 0xFE) {
        return std::wstring(reinterpret_cast<const wchar_t*>(bytes.data() + 2),
                            (bytes.size() - 2) / 2);
    }
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                   (int)bytes.size(), nullptr, 0);
    UINT codePage = CP_UTF8;
    if (size <= 0) {
        codePage = CP_ACP;
        size = MultiByteToWideChar(codePage, 0, bytes.data(), (int)bytes.size(),
                                   nullptr, 0);
    }
    if (size <= 0) return L"";
    std::wstring out((std::size_t)size, L'\0');
    MultiByteToWideChar(codePage, 0, bytes.data(), (int)bytes.size(), out.data(), size);
    return out;
}

std::wstring Env(const wchar_t* name) {
    wchar_t buffer[32768];
    const DWORD length = GetEnvironmentVariableW(name, buffer,
                                                 (DWORD)std::size(buffer));
    if (length == 0 || length >= std::size(buffer)) return L"";
    return std::wstring(buffer, length);
}

std::wstring ExeDir() {
    wchar_t path[32768]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, 32768);
    if (n == 0) return L".";
    std::wstring text(path, n);
    const std::size_t slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : text.substr(0, slash);
}

void WriteUtf8File(const std::wstring& path, const std::string& text) {
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!out) return;
    out.write(text.data(), (std::streamsize)text.size());
}

void WriteDefaultPersona(const std::wstring& dir) {
    const std::wstring path = dir + L"\\" + kPersona;
    if (FileExists(path)) return;
    const char* text =
        "You are the Desktop Desk assistant on the user's Windows desktop.\n"
        "Rules:\n"
        "- Answer in Simplified Chinese, conclusion first, then at most one short reason.\n"
        "- Use ONLY the evidence in the provided context (DSH projects, sessions,\n"
        "  memory facts, recent episodes). If evidence is missing, say you cannot tell.\n"
        "- When a claim comes from memory, append [[mem:<id>]]; when it comes from a\n"
        "  DSH project/session/episode, append [[src:<id>]]. Never invent an id.\n"
        "- Keep the answer under ~200 Chinese characters unless asked for detail.\n"
        "- Never print API keys, passwords, tokens or credential file contents.\n"
        "- Do not run commands or edit files; you are read-only in this version.\n";
    WriteUtf8File(path, text);
}

bool ContainsId(const std::vector<std::wstring>& ids, const std::wstring& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// Removes real citation markers ([[mem:...]] / [[src:...]]) only; any other
// [[...]] text is ordinary prose and must survive verbatim. While streaming
// (finalText == false) an unterminated trailing marker is held back, so a
// marker split across two deltas never flashes as text; the final pass emits
// whatever never closed as literal text. The visible text only ever grows,
// which is what lets the engine diff it against what it already emitted.
std::wstring StripCitationTokens(const std::wstring& text, bool finalText) {
    std::wstring out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t open = text.find(L"[[", pos);
        if (open == std::wstring::npos) {
            out.append(text, pos, std::wstring::npos);
            break;
        }
        out.append(text, pos, open - pos);
        const std::size_t close = text.find(L"]]", open + 2);
        if (close == std::wstring::npos) {
            if (finalText) out.append(text, open, std::wstring::npos);
            break;  // streaming: hold the unfinished tail
        }
        const std::wstring token = text.substr(open + 2, close - open - 2);
        const bool citation = token.compare(0, 4, L"mem:") == 0 ||
                              token.compare(0, 4, L"src:") == 0;
        if (!citation) out.append(text, open, close + 2 - open);
        pos = close + 2;
    }
    return out;
}

// pi inherits the launcher's integrity level. Running desk.exe from a low /
// restricted workspace makes pi's mkdir of ~/.pi/agent/trust.json.lock fail
// with EPERM, which otherwise looks like a random "PI CRASH 1".
std::wstring TranslatePiError(const std::wstring& text) {
    if (text.find(L"pi still settling previous run") != std::wstring::npos) {
        return L"上一问仍在收尾，请稍后重试";
    }
    if (text.find(L"trust.json.lock") != std::wstring::npos ||
        text.find(L"EPERM") != std::wstring::npos) {
        return L"pi 无法写入信任文件（EPERM：低完整性/受限环境）。请以普通权限启动 "
               L"desk.exe（不要从受限/沙箱终端启动，也不要放在低完整性工作区），"
               L"或安装到 %LOCALAPPDATA%\\Programs 下再运行";
    }
    return text;
}

// Credential forwarding table for the providers M1 supports explicitly. An
// unknown provider with a key_ref fails closed instead of guessing an argv or
// environment fallback.
bool ProviderEnvName(const std::wstring& provider, std::wstring* name) {
    const std::wstring lower = Lower(provider);
    if (lower == L"command-code" || lower == L"commandcode" ||
        lower.find(L"commandcode") != std::wstring::npos) {
        *name = L"CMD_API_KEY";
        return true;
    }
    if (lower == L"deepseek" || lower.find(L"deepseek") != std::wstring::npos) {
        *name = L"DEEPSEEK_API_KEY";
        return true;
    }
    return false;
}

bool IsCommandCodeProvider(const std::wstring& provider) {
    const std::wstring lower = Lower(provider);
    return lower == L"command-code" || lower == L"commandcode" ||
           lower.find(L"commandcode") != std::wstring::npos;
}

// The installed official provider extension (Pi needs -e for a provider that
// lives outside its own package). assistant.ini provider_extension overrides.
std::wstring DefaultProviderExtension() {
    const std::wstring profile = Env(L"USERPROFILE");
    if (profile.empty()) return L"";
    return profile +
           L"\\.pi\\agent\\npm\\node_modules\\@commandcode\\pi-commandcode-provider"
           L"\\index.ts";
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    auto normalize = [](std::wstring text) {
        for (wchar_t& c : text) {
            if (c == L'/') c = L'\\';
            c = (wchar_t)std::towlower(c);
        }
        while (text.size() > 3 && text.back() == L'\\') text.pop_back();
        return text;
    };
    return normalize(a) == normalize(b);
}

// One line, at most 'limit' characters, whitespace collapsed.
std::wstring Condense(const std::wstring& text, std::size_t limit) {
    std::wstring out;
    bool space = false;
    for (wchar_t c : text) {
        const bool isSpace = c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
        if (isSpace) {
            space = !out.empty();
            continue;
        }
        if (space) {
            out += L' ';
            space = false;
        }
        out += c;
        if (out.size() >= limit) break;
    }
    if (out.size() > limit) out.resize(limit);
    return out;
}

// The blocker sentence of a cached conclusion. Empty when the text never
// mentions a blocker - the caller then says so instead of inventing one.
std::wstring ExtractBlocker(const std::wstring& text) {
    static const wchar_t* kKeys[] = {L"卡点", L"阻塞", L"卡住", L"blocked", L"blocker"};
    std::size_t best = std::wstring::npos;
    for (const wchar_t* key : kKeys) {
        const std::size_t at = Lower(text).find(key);
        if (at != std::wstring::npos && (best == std::wstring::npos || at < best)) {
            best = at;
        }
    }
    if (best == std::wstring::npos) return L"";
    const std::wstring key = Lower(text).substr(best, 2);
    std::size_t from = best + key.size();
    while (from < text.size() &&
           (text[from] == L' ' || text[from] == L':' || text[from] == L'：' ||
            text[from] == L'\t')) {
        ++from;
    }
    std::wstring fragment;
    for (std::size_t i = from; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'\n' || c == L'\r' || c == L'。') break;
        fragment += c;
        if (fragment.size() >= 60) break;
    }
    return Condense(fragment, 60);
}

} // namespace

Engine::Engine() = default;

Engine::~Engine() { Stop(); }

bool Engine::Start(const std::wstring& configDir, std::wstring* error) {
    return Start(configDir, Options{}, error);
}

bool Engine::Start(const std::wstring& configDir, const Options& options,
                   std::wstring* error) {
    if (idleThread_.joinable()) {
        if (error) *error = L"ENGINE ALREADY STARTED";
        return false;
    }
    configDir_ = configDir;
    personaPath_ = configDir_ + L"\\" + kPersona;
    EnsureDirectory(configDir_);
    WriteDefaultPersona(configDir_);
    LoadSettings();
    idleStopMs_ = options.idleStopMs > 0
                      ? options.idleStopMs
                      : (idleStopMinutes_ > 0 ? idleStopMinutes_ * 60 * 1000 : 0);
    dshRoot_ = options.dshRoot.empty() ? DefaultDshRoot() : options.dshRoot;

    std::wstring memoryError;
    auto memory = OpenSharedMemory(configDir_, &memoryError);
    if (!memory) {
        if (error) *error = memoryError.empty() ? L"MEMORY LOAD FAILED" : memoryError;
        return false;
    }
    context_.reset();
    memory_ = std::move(memory);
    context_ = std::make_unique<ContextBuilder>(configDir_, dshRoot_, *memory_);

    // No credential access or child process until the first remote Ask. Start
    // may follow Stop with a different config, so discard the inactive wrapper.
    pi_.reset();
    sessionPath_.clear();
    sessionDigest_.clear();
    sessionPolicy_.clear();
    sessionReset_.clear();
    // The epoch exists before loading so a stale session path can be dropped with
    // a consistent revision/epoch baseline.
    epoch_ = std::make_shared<std::atomic<unsigned long long>>(0);
    sessionEpoch_ = 0;
    LoadSessionState();
    LoadImportCheckpoint();

    // A weak counter: a notification that lands after Stop() cannot touch a
    // destroyed engine, and an in-flight answer can detect a memory change.
    std::weak_ptr<std::atomic<unsigned long long>> weakEpoch = epoch_;
    subscription_ = memory_->Subscribe([weakEpoch] {
        if (auto epoch = weakEpoch.lock()) {
            epoch->fetch_add(1, std::memory_order_relaxed);
        }
    });

    if (enabled_) ImportProjects();

    {
        std::lock_guard<std::mutex> lock(mu_);
        ResetRequestLocked();
        ++requestId_;  // a late completion from a previous run is now stale
        ready_ = true;
    }
    {
        std::lock_guard<std::mutex> lock(idleMu_);
        idleStop_ = false;
        activityGen_ = 0;
    }
    idleThread_ = std::thread([this] { IdleLoop(); });
    return true;
}

void Engine::ResetRequestLocked() {
    busy_ = false;
    settling_ = false;
    requestFailed_ = false;
    requestError_.clear();
    cancelRequested_ = false;
    localCommand_ = LocalCommand::None;
    callbacks_ = {};
    question_.clear();
    answer_.clear();
    emitted_ = 0;
    sources_.clear();
    inputTokens_ = outputTokens_ = askStartMs_ = 0;
    requestEpoch_ = 0;
}

void Engine::Stop() {
    if (g_workerOwner == this) {
        // A Stop() issued from an engine callback cannot join the worker it is
        // running on. Mark the engine closed and request the pi shutdown, but
        // leave the joins to the owner (UI/destructor) on another thread: every
        // thread object stays joinable, nothing is detached.
        std::shared_ptr<PiClient> running;
        {
            std::lock_guard<std::mutex> lock(mu_);
            ready_ = false;
            ResetRequestLocked();
            ++requestId_;
            running = pi_;
        }
        if (running) running->RequestStop();
        {
            std::lock_guard<std::mutex> lock(idleMu_);
            idleStop_ = true;
        }
        idleCv_.notify_all();
        return;
    }
    std::shared_ptr<PiClient> client;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ready_ = false;
        // Shutdown is silent: the caller can be the card destructor.
        ResetRequestLocked();
        ++requestId_;
        client = std::move(pi_);
        pi_.reset();
    }
    // Interrupt a get_state wait without racing two join/Stop operations.
    // This wrapper is discarded by the next successful Engine::Start.
    if (client) client->RequestStop();
    {
        std::lock_guard<std::mutex> lock(idleMu_);
        idleStop_ = true;
    }
    idleCv_.notify_all();
    if (idleThread_.joinable()) idleThread_.join();
    if (worker_.joinable()) worker_.join();
    if (client) client->Stop();
    // Do not lose an invalidation that happened after the last answer but
    // before the next Ask: persist it now (path + digests only). This needs the
    // live epoch baseline: Stop is called twice in practice (the card
    // destructor, then the Engine destructor), and after the first call the
    // epoch is released, so a second evaluation would compare a stale saved
    // epoch against 0 and delete a perfectly valid session.
    if (epoch_ && !sessionPath_.empty() &&
        (CurrentEpoch() != sessionEpoch_ || CurrentReset() != sessionReset_)) {
        ClearSessionState();
    }
    if (memory_ && subscription_ != 0) memory_->Unsubscribe(subscription_);
    subscription_ = 0;
    epoch_.reset();
}

void Engine::LoadSettings() {
    wg::Store store;
    const std::wstring path = configDir_ + L"\\assistant.ini";
    store.Open(path);
    enabled_ = store.GetInt(L"enabled", 1) != 0;
    contextBudget_ = (int)store.GetInt(L"context_tokens", kContextBudget);
    if (contextBudget_ < 500) contextBudget_ = 500;
    idleStopMinutes_ = (int)store.GetInt(L"idle_stop_minutes", 10);
    if (idleStopMinutes_ < 0) idleStopMinutes_ = 0;
    dailyTokenLimit_ = store.GetInt(L"daily_token_limit", 0);
    if (dailyTokenLimit_ < 0) dailyTokenLimit_ = 0;
    piCommand_ = store.Get(L"pi_command", L"");
    // M1 pi runs with zero tools. The key stays in assistant.ini for forward
    // compatibility, but a stale read/edit whitelist must never be forwarded.
    tools_ = L"";
    provider_ = store.Get(L"provider", L"command-code");
    model_ = store.Get(L"model", L"deepseek/deepseek-v4.1-flash-fast");
    keyRef_ = store.Get(L"key_ref", L"COMMANDCODE_API_KEY_2");
    providerExtension_ = store.Get(L"provider_extension", L"");

    bool changed = false;
    if (!store.Has(L"enabled")) {
        store.SetInt(L"enabled", enabled_ ? 1 : 0);
        store.SetInt(L"context_tokens", contextBudget_);
        store.SetInt(L"idle_stop_minutes", idleStopMinutes_);
        store.SetInt(L"daily_token_limit", dailyTokenLimit_);
        store.Set(L"tools", tools_);
        changed = true;
    }
    if (store.Has(L"tools") && !store.Get(L"tools", L"").empty()) {
        store.Set(L"tools", L"");
        changed = true;
    }
    if (!store.Has(L"daily_token_limit")) {
        store.SetInt(L"daily_token_limit", dailyTokenLimit_);
        changed = true;
    }
    if (!store.Has(L"provider_extension")) {
        store.Set(L"provider_extension", providerExtension_);
        changed = true;
    }
    // Existing M1 installs only ever got the DeepSeek default; add the
    // CommandCode/ACC2 route once so "insufficient balance" does not come back
    // after an upgrade. Users can still blank provider/model to defer to pi.
    if (!store.Has(L"provider")) {
        store.Set(L"provider", provider_);
        store.Set(L"model", model_);
        store.Set(L"key_ref", keyRef_);
        changed = true;
    }
    if (changed) store.Save();
}

void Engine::NoteActivity() {
    {
        std::lock_guard<std::mutex> lock(idleMu_);
        ++activityGen_;
    }
    idleCv_.notify_all();
}

void Engine::IdleLoop() {
    std::unique_lock<std::mutex> lock(idleMu_);
    unsigned long long seen = activityGen_;
    while (!idleStop_) {
        if (idleStopMs_ <= 0) {
            idleCv_.wait(lock, [this] { return idleStop_; });
            break;
        }
        const bool changed = idleCv_.wait_for(
            lock, std::chrono::milliseconds(idleStopMs_),
            [this, seen] { return idleStop_ || activityGen_ != seen; });
        if (idleStop_) break;
        if (changed) {
            seen = activityGen_;
            continue;
        }
        lock.unlock();
        {
            // Serialize idle Stop with startup/Ask. The UI only marks busy;
            // it never waits on this lifecycle lock.
            std::lock_guard<std::mutex> lifecycle(piMu_);
            std::shared_ptr<PiClient> client;
            {
                std::lock_guard<std::mutex> busyLock(mu_);
                if (!busy_) client = pi_;
            }
            if (client && client->Running()) client->Stop();
        }
        lock.lock();
        seen = activityGen_;
    }
}

void Engine::Cancel() {
    std::shared_ptr<PiClient> client;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!ready_ || !busy_ || settling_) return;
        if (localCommand_ != LocalCommand::None) return;  // already finishing
        cancelRequested_ = true;
        client = pi_;
    }
    if (client) client->Cancel();
}

std::wstring Engine::Brief() {
    const std::vector<DshProject> projects = ReadDshProjects(dshRoot_, 3);
    if (projects.empty()) return L"NO DSH PROJECTS YET";
    const std::vector<DshSession> sessions = ReadDshSessions(dshRoot_, 8);
    std::wstring brief;
    for (const DshProject& project : projects) {
        if (MemoryStore::IsSensitive(project.path) ||
            MemoryStore::IsSensitive(project.title)) {
            continue;
        }
        if (!brief.empty()) brief += L"\n";
        const std::wstring title = project.title.empty() ? project.path : project.title;
        brief += L"项目：" + title;
        if (project.updatedMs > 0) {
            const long long ageH = (NowMs() - project.updatedMs) / 3600000LL;
            wchar_t age[24];
            if (ageH < 1) swprintf(age, 24, L" now");
            else if (ageH < 48) swprintf(age, 24, L" %lldh", ageH);
            else swprintf(age, 24, L" %lldd", ageH / 24);
            brief += age;
        }
        // The newest cached session for this project's own directory: a
        // different project's conclusion is never borrowed.
        const DshSession* match = nullptr;
        for (const DshSession& session : sessions) {
            if (!SamePath(session.cwd, project.path)) continue;
            match = &session;
            break;
        }
        const bool usable = match && !match->lastResponse.empty() &&
                            !MemoryStore::IsSensitive(match->lastResponse) &&
                            !MemoryStore::IsSensitive(match->title);
        if (!usable) {
            brief += L"\n最近结论：未记录\n卡点：未记录";
            continue;
        }
        brief += L"\n最近结论：" + Condense(match->lastResponse, 80);
        const std::wstring blocker = ExtractBlocker(match->lastResponse);
        brief += L"\n卡点：" + (blocker.empty() ? std::wstring(L"未记录") : blocker);
    }
    return brief;
}

Engine::ParsedCommand Engine::ParseLocalCommand(const std::wstring& question) {
    ParsedCommand parsed;
    std::wstring text = Trim(question);
    if (text.empty()) return parsed;
    const std::wstring lower = Lower(text);

    auto strip = [&](const wchar_t* prefix) -> bool {
        const std::wstring head(prefix);
        const std::wstring lowerHead = Lower(head);
        if (lower.compare(0, lowerHead.size(), lowerHead) != 0) return false;
        text.erase(0, head.size());
        text = Trim(text);
        while (!text.empty() && (text.front() == L':' || text.front() == L'：' ||
                                 text.front() == L',' || text.front() == L'，' ||
                                 text.front() == L'、')) {
            text.erase(0, 1);
            text = Trim(text);
        }
        return true;
    };

    // Clear first: "清空记忆" must not be read as a remembered statement.
    static const wchar_t* kClear[] = {L"清空全部记忆", L"清空所有记忆", L"清空记忆",
                                      L"clear all memory", L"clear memory"};
    for (const wchar_t* prefix : kClear) {
        if (strip(prefix)) {
            parsed.kind = LocalCommand::ClearAll;
            return parsed;
        }
    }
    static const wchar_t* kRemember[] = {L"记住", L"remember", L"记下"};
    for (const wchar_t* prefix : kRemember) {
        if (strip(prefix)) {
            parsed.kind = LocalCommand::Remember;
            parsed.text = Trim(text);
            return parsed;
        }
    }
    static const wchar_t* kForget[] = {L"忘掉", L"忘记", L"forget"};
    for (const wchar_t* prefix : kForget) {
        if (strip(prefix)) {
            parsed.kind = LocalCommand::Forget;
            parsed.text = Trim(text);
            return parsed;
        }
    }
    return parsed;
}

std::wstring Engine::RememberKind(const std::wstring& text) {
    // "记住 X 的目标是 Y" is a milestone; a generic goal becomes a goal, and
    // everything else stays a preference.
    if (text.find(L"的目标是") != std::wstring::npos ||
        Lower(text).find(L"目标为") != std::wstring::npos) {
        return L"milestone";
    }
    if (text.compare(0, 2, L"目标") == 0 || Lower(text).compare(0, 4, L"goal") == 0) {
        return L"goal";
    }
    return L"preference";
}

void Engine::Ask(const std::wstring& question, Callbacks callbacks) {
    const ParsedCommand parsed = ParseLocalCommand(question);
    // Callbacks arrive on the engine worker (local/budget answers) or on pi's
    // reader thread. The UI contract is to marshal them back to the UI thread;
    // a direct submit from the engine worker is refused here, so nothing is
    // detached and the worker thread object stays joinable.
    const bool reentrant = g_workerOwner == this;
    std::wstring rejection;
    long long id = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!ready_) rejection = L"ASSISTANT NOT STARTED";
        else if (reentrant) {
            rejection = L"REENTRANT ASK FROM AGENT CALLBACK: dispatch to the UI thread";
        } else if (busy_) rejection = L"BUSY: ANOTHER QUESTION IS RUNNING";
        else if (Trim(question).empty()) rejection = L"EMPTY QUESTION";
        else if (!enabled_) rejection = L"ASSISTANT DISABLED (assistant.ini enabled=0)";
        if (rejection.empty()) {
            busy_ = true;
            settling_ = false;
            requestFailed_ = false;
            requestError_.clear();
            cancelRequested_ = false;
            callbacks_ = std::move(callbacks);
            question_ = question;
            answer_.clear();
            emitted_ = 0;
            sources_.clear();
            inputTokens_ = 0;
            outputTokens_ = 0;
            askStartMs_ = NowMs();
            localCommand_ = parsed.kind;
            requestEpoch_ = epoch_ ? (long long)epoch_->load(std::memory_order_relaxed) : 0;
            id = ++requestId_;
        }
    }
    if (!rejection.empty()) {
        if (callbacks.onError) callbacks.onError(rejection);
        return;
    }
    NoteActivity();

    // Ask() never reaches this point on the worker thread (rejected above), so
    // this join is always a cross-thread join.
    if (worker_.joinable()) worker_.join();

    const std::wstring trimmed = Trim(question);
    worker_ = std::thread([this, id, trimmed, parsed] {
        WorkerThreadScope workerScope(this);
        std::lock_guard<std::mutex> lifecycle(piMu_);
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!RequestActiveLocked(id)) return;
        }
        if (parsed.kind != LocalCommand::None) {
            RunLocal(id, parsed.kind, parsed.text);
        } else {
            RunRemote(id, trimmed);
        }
    });
}

bool Engine::RequestActiveLocked(long long requestId) const {
    return ready_ && busy_ && requestId_ == requestId;
}

void Engine::PublishLocal(long long requestId, const std::wstring& delta, bool ok,
                          const std::wstring& error) {
    Callbacks cb;
    long long elapsed = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!RequestActiveLocked(requestId)) return;
        settling_ = true;
        cb = callbacks_;
        callbacks_ = {};
        elapsed = std::max(0LL, NowMs() - askStartMs_);
        busy_ = false;
        settling_ = false;
        localCommand_ = LocalCommand::None;
    }
    NoteActivity();
    if (!ok) {
        if (cb.onError) {
            cb.onError(error.empty() ? L"MEMORY COMMAND FAILED" : error);
        }
        return;
    }
    if (cb.onDelta && !delta.empty()) cb.onDelta(delta);
    if (cb.onDone) cb.onDone(L"", 0, 0, elapsed);
}

void Engine::FailRequest(long long requestId, const std::wstring& error) {
    PublishLocal(requestId, L"", false, error);
}

void Engine::RunLocal(long long requestId, LocalCommand kind,
                      const std::wstring& text) {
    // A local memory mutation must not be polluted by the old pi session: stop
    // the wrapper first (we hold piMu_), then drop it and forget the saved
    // session, so no stale callback can answer this request and the next remote
    // Ask starts from a clean session.
    std::shared_ptr<PiClient> victim;
    {
        std::lock_guard<std::mutex> lock(mu_);
        victim = std::move(pi_);
        pi_.reset();
    }
    if (victim) {
        victim->RequestStop();
        victim->Stop();
    }
    ClearSessionState();

    // The resolved provider credential must never be stored through a local
    // command either, even when it has no recognisable "sk-" shape. A missing
    // or unresolvable key reference produces an empty literal here, so ordinary
    // local memory keeps working (no Pi, no credential read failure path).
    const std::wstring secret = EffectiveKeyLiteral();

    std::wstring error;
    std::wstring delta;
    bool ok = false;
    switch (kind) {
    case LocalCommand::Remember: {
        if (text.empty()) {
            FailRequest(requestId, L"记住：请说明要记住什么");
            return;
        }
        if (!secret.empty() && text.find(secret) != std::wstring::npos) {
            FailRequest(requestId, L"内容含当前渠道凭据，已拒绝记住");
            return;
        }
        Fact fact;
        fact.kind = RememberKind(text);
        fact.text = text;
        fact.sourceRef = L"user";
        fact.confidence = 90;
        ok = memory_->AddFact(fact, &error);
        if (ok) {
            const std::wstring kindLabel = fact.kind == L"milestone" ? L"里程碑"
                                            : fact.kind == L"goal" ? L"目标"
                                                                   : L"偏好";
            delta = L"已记住（" + kindLabel + L"）：" + text;
        }
        break;
    }
    case LocalCommand::Forget: {
        if (text.empty()) {
            FailRequest(requestId, L"忘掉：请说明要忘掉什么");
            return;
        }
        const std::wstring needle = Lower(text);
        std::vector<std::wstring> ids;
        for (const Fact& fact : memory_->Facts()) {
            if (Lower(fact.text).find(needle) != std::wstring::npos) {
                ids.push_back(fact.id);
            }
        }
        if (ids.empty()) {
            // Deliberately no payload echo: a forget request may name a secret.
            FailRequest(requestId, L"没有匹配的记忆。");
            return;
        }
        for (const std::wstring& id : ids) {
            if (!memory_->DeleteFact(id, &error)) {
                FailRequest(requestId, error.empty() ? L"MEMORY DELETE FAILED" : error);
                return;
            }
        }
        ok = true;
        // The confirmation never repeats what was removed (the local forget is
        // the way to delete a leaked secret, and it is not gated on the key).
        delta = L"已忘掉 " + std::to_wstring(ids.size()) + L" 条记忆。";
        break;
    }
    case LocalCommand::ClearAll: {
        ok = memory_->ClearAll(&error);
        if (ok) delta = L"已清空全部记忆（事实与问答记录）。";
        break;
    }
    case LocalCommand::None: return;
    }
    PublishLocal(requestId, delta, ok, error);
}

void Engine::RunRemote(long long requestId, const std::wstring& question) {
    // 1) The user's own question is never sent when it looks like credential
    // material - filtering DSH/memory first is not enough.
    if (MemoryStore::IsSensitive(question)) {
        FailRequest(requestId,
                    L"问题包含敏感内容（凭据/私钥/敏感路径），已拒绝发送");
        return;
    }
    // 2) The concrete credential we would hand to pi (resolved ref or the
    // provider env var) must never appear in the question, the persona
    // policy file or the assembled context. Persona is trusted policy text, so
    // only this literal is rejected - no generic "password" word scanning.
    const std::wstring secret = EffectiveKeyLiteral();
    if (!secret.empty() && question.find(secret) != std::wstring::npos) {
        FailRequest(requestId, L"问题含当前渠道凭据，已拒绝发送");
        return;
    }
    if (!secret.empty() && ReadTextFile(personaPath_).find(secret) != std::wstring::npos) {
        FailRequest(requestId, L"persona.md 含当前渠道凭据，已拒绝发送");
        return;
    }
    // 3) Daily budget: degrade to a local BRIEF answer without launching pi.
    if (BudgetExhausted()) {
        wchar_t header[128];
        swprintf(header, 128, L"今日 token 预算已用完（%lld/%lld）。本地 BRIEF：\n",
                 DailyTokensUsed(), dailyTokenLimit_);
        PublishLocal(requestId, std::wstring(header) + Brief(), true, L"");
        return;
    }

    // 4) Session continuity: a fact/episode/policy change - or a memory reset
    // revision advanced by a ClearAll - invalidates the pi session so the model
    // cannot answer from a memory the user has since edited or cleared. The
    // revision and epoch also cover a ClearAll that changed nothing on disk (an
    // empty store still notifies and still advances the revision).
    const std::wstring digest = FactsDigest();
    const std::wstring policy = PolicyDigest();
    if (!sessionPath_.empty() &&
        (digest != sessionDigest_ || policy != sessionPolicy_ ||
         CurrentReset() != sessionReset_ || CurrentEpoch() != sessionEpoch_)) {
        std::shared_ptr<PiClient> victim;
        {
            std::lock_guard<std::mutex> lock(mu_);
            victim = std::move(pi_);
            pi_.reset();
        }
        if (victim) {
            victim->RequestStop();
            victim->Stop();
        }
        ClearSessionState();
    }

    // 5) Explicit provider/extension resolution, fail closed.
    std::wstring providerExtension;
    std::wstring keyEnv;
    if (!provider_.empty()) {
        if (!ProviderEnvName(provider_, &keyEnv)) {
            keyEnv.clear();
            if (!keyRef_.empty()) {
                FailRequest(requestId,
                            L"未知 provider（" + provider_ +
                                L"）：没有凭据转发规则，拒绝启动 pi（fail closed）");
                return;
            }
        }
        if (IsCommandCodeProvider(provider_)) {
            providerExtension = !providerExtension_.empty()
                                    ? providerExtension_
                                    : DefaultProviderExtension();
            if (!FileExists(providerExtension)) {
                FailRequest(requestId,
                            L"找不到官方 provider 扩展：" + providerExtension +
                                L"（安装 @commandcode/pi-commandcode-provider，"
                                L"或在 assistant.ini 设 provider_extension）");
                return;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!RequestActiveLocked(requestId)) return;
    }
    if (!pi_) {
        const std::wstring agentDir = configDir_ + L"\\pi-agent";
        const std::wstring workDir = configDir_ + L"\\pi-work";
        if (!EnsureDirectory(agentDir) || !EnsureDirectory(workDir)) {
            FailRequest(requestId, L"无法创建 pi 私有配置目录");
            return;
        }
        PiClientOptions options;
        options.configDir = configDir_;
        options.workDir = workDir;
        options.sessionPath = sessionPath_;
        options.personaPath = personaPath_;
        options.tools.clear();  // M1 runtime is zero-tool (PiClient enforces)
        options.piCommand = piCommand_;
        options.providerExtension = providerExtension;
        // Credentials travel only in the child environment, never argv.
        options.childEnv.push_back({L"PI_CODING_AGENT_DIR", agentDir});
        if (!keyEnv.empty() && !keyRef_.empty()) {
            const std::wstring key = aimeter::ReadDshRef(keyRef_);
            if (key.empty()) {
                FailRequest(requestId,
                            L"key_ref 无法解析：" + keyRef_ + L"（凭据缺失，拒绝启动）");
                return;
            }
            options.childEnv.push_back({keyEnv, key});
        }
        if (!provider_.empty()) {
            options.extraArgs.push_back(L"--provider");
            options.extraArgs.push_back(provider_);
        }
        if (!model_.empty()) {
            options.extraArgs.push_back(L"--model");
            options.extraArgs.push_back(model_);
        }
        auto client = std::make_shared<PiClient>(options);
        client->SetEventHandler([this](const PiEvent& event) { HandleEvent(event); });
        // Cancel snapshots the pointer under mu_; idle accesses it only
        // under piMu_. A shared_ptr keeps it alive while other paths discard it.
        std::lock_guard<std::mutex> lock(mu_);
        if (!RequestActiveLocked(requestId)) return;
        pi_ = client;
    }

    std::shared_ptr<PiClient> client;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!RequestActiveLocked(requestId)) return;
        client = pi_;
    }
    if (!client) {
        FailRequest(requestId, L"PI CLIENT UNAVAILABLE");
        return;
    }
    std::wstring startError;
    if (!client->Running() && !client->Start(&startError)) {
        FailRequest(requestId, TranslatePiError(startError.empty() ? L"PI START FAILED"
                                                                  : startError));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!RequestActiveLocked(requestId)) return;
    }
    const ContextBundle bundle = context_->Build(NowMs(), contextBudget_, question);
    if (!secret.empty() && bundle.prompt.find(secret) != std::wstring::npos) {
        FailRequest(requestId, L"上下文含当前渠道凭据，已拒绝发送");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!RequestActiveLocked(requestId)) return;
        sources_ = bundle.sourceIds;
    }
    const std::wstring prompt =
        bundle.prompt + L"\n\nUSER QUESTION:\n" + question + L"\n";
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cancelled = cancelRequested_;
    }
    if (cancelled) {
        PiEvent done;
        done.kind = PiEvent::Kind::Done;
        done.aborted = true;
        HandleEvent(done);
        return;
    }
    std::wstring askError;
    if (client->Ask(prompt, &askError) == 0) {
        FailRequest(requestId, TranslatePiError(askError.empty() ? L"PI NOT READY"
                                                                 : askError));
        return;
    }
    // Cancel may have raced the gap between the preflight check and writing the
    // prompt. Abort again after it is in flight.
    {
        std::lock_guard<std::mutex> lock(mu_);
        cancelled = cancelRequested_;
    }
    if (cancelled) client->Cancel();
}

void Engine::HandleEvent(const PiEvent& event) {
    switch (event.kind) {
    case PiEvent::Kind::TextDelta: {
        Callbacks cb;
        std::wstring delta;
        {
            std::lock_guard<std::mutex> lock(mu_);
            // A local request owns the callbacks while an old pi run may still
            // be draining: its events must never reach the new request.
            if (!busy_ || settling_ || localCommand_ != LocalCommand::None) break;
            answer_.append(event.text);
            const std::wstring visible = StripCitationTokens(answer_, false);
            if (visible.size() > emitted_) {
                delta = visible.substr(emitted_);
                emitted_ = visible.size();
            }
            cb = callbacks_;
        }
        if (!delta.empty() && cb.onDelta) cb.onDelta(delta);
        break;
    }
    case PiEvent::Kind::Usage: {
        std::lock_guard<std::mutex> lock(mu_);
        if (!busy_ || settling_ || localCommand_ != LocalCommand::None) break;
        inputTokens_ = std::max(inputTokens_, event.inputTokens);
        outputTokens_ = std::max(outputTokens_, event.outputTokens);
        break;
    }
    case PiEvent::Kind::Error: {
        std::wstring text = event.text.empty() ? std::wstring(L"PI ERROR") : event.text;
        if (text.find(L"Insufficient Balance") != std::wstring::npos ||
            text.find(L"402") != std::wstring::npos) {
            text = L"模型余额不足 (HTTP 402)：请在 assistant.ini 设 provider/model "
                   L"指向有余额的渠道（pi --list-models 查看），或给当前渠道充值";
        }
        text = TranslatePiError(text);
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!busy_ || settling_ || localCommand_ != LocalCommand::None) break;
            requestFailed_ = true;
            requestError_ = std::move(text);
        }
        // Publish one terminal error after Done (or Start/Ask failure), not an
        // early callback that lets the UI race a still-active RPC request.
        break;
    }
    case PiEvent::Kind::Done: {
        Callbacks cb;
        std::wstring citation;
        std::wstring tail;
        std::wstring answer;
        std::wstring question;
        std::wstring failureText;
        bool failed = false;
        bool aborted = false;
        bool memoryChanged = false;
        std::shared_ptr<PiClient> client;
        long long inTokens = 0;
        long long outTokens = 0;
        long long elapsed = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!busy_ || settling_ || localCommand_ != LocalCommand::None) break;
            settling_ = true;
            failed = requestFailed_;
            failureText = requestError_;
            aborted = event.aborted || cancelRequested_;
            cb = callbacks_;
            callbacks_ = {};
            question = question_;
            answer = StripCitationTokens(answer_, false);
            const std::wstring finalVisible = StripCitationTokens(answer_, true);
            if (finalVisible.size() > emitted_) {
                tail = finalVisible.substr(emitted_);
                emitted_ = finalVisible.size();
            }
            answer = finalVisible;
            citation = BuildCitation(answer_, sources_);
            inTokens = inputTokens_;
            outTokens = outputTokens_;
            elapsed = std::max(0LL, NowMs() - askStartMs_);
            memoryChanged =
                epoch_ && (long long)epoch_->load(std::memory_order_relaxed) != requestEpoch_;
            client = pi_;
        }
        // A memory change while the answer streamed means the model may have
        // quoted a fact the user has since edited/forgotten: the answer is not
        // trusted and nothing is persisted.
        if (!failed && !aborted && memoryChanged) {
            failed = true;
            failureText = L"记忆已更新：上一答案未保存，请重新提问";
        }
        // The literal tail of the prose (text that never closed into a marker)
        // is published before the terminal callback so the card shows the whole
        // answer. Nothing is persisted for a failed/aborted request.
        if (!failed && !aborted && !tail.empty() && cb.onDelta) cb.onDelta(tail);
        // A settled event also terminates failed/aborted requests. Never
        // interpret it as successful permission to remember or forget facts.
        // Memory commands are local and deterministic, handled in Ask().
        if (!failed && !aborted) {
            if (!answer.empty()) {
                memory_->AppendEpisode(question, answer, citation, nullptr);
                RecordCost(inTokens, outTokens, elapsed);
            }
            // Remember the pi session for the next remote Ask: path + digests
            // only, never a credential. A normal episode append does not notify
            // subscribers, so it never forces this rotation.
            if (client) {
                sessionPath_ = client->SessionPath();
                sessionDigest_ = FactsDigest();
                sessionPolicy_ = PolicyDigest();
                sessionEpoch_ = CurrentEpoch();
                sessionReset_ = CurrentReset();
                SaveSessionState();
            }
        }
        NoteActivity();
        {
            std::lock_guard<std::mutex> lock(mu_);
            busy_ = false;
            settling_ = false;
            localCommand_ = LocalCommand::None;
        }
        if (failed) {
            if (cb.onError) cb.onError(failureText.empty() ? L"PI ERROR" : failureText);
        } else if (aborted) {
            if (cb.onError) cb.onError(L"CANCELLED");
        } else if (cb.onDone) {
            cb.onDone(citation, inTokens, outTokens, elapsed);
        }
        break;
    }
    case PiEvent::Kind::Tool:
        break;
    }
}

std::wstring Engine::BuildCitation(const std::wstring& answer,
                                   const std::vector<std::wstring>& sources) const {
    std::vector<std::wstring> found;
    std::size_t pos = 0;
    while (pos < answer.size()) {
        const std::size_t open = answer.find(L"[[", pos);
        if (open == std::wstring::npos) break;
        const std::size_t close = answer.find(L"]]", open + 2);
        if (close == std::wstring::npos) break;
        std::wstring token = answer.substr(open + 2, close - open - 2);
        pos = close + 2;
        std::wstring id;
        if (token.compare(0, 4, L"mem:") == 0) id = token.substr(4);
        else if (token.compare(0, 4, L"src:") == 0) id = token.substr(4);
        else continue;
        while (!id.empty() && (id.front() == L' ' || id.front() == L'\t')) id.erase(0, 1);
        while (!id.empty() && (id.back() == L' ' || id.back() == L'\t')) id.pop_back();
        if (id.empty() || !ContainsId(sources, id)) continue;
        if (std::find(found.begin(), found.end(), id) == found.end()) found.push_back(id);
    }
    std::wstring citation;
    for (const std::wstring& id : found) {
        if (!citation.empty()) citation += L", ";
        citation += id;
    }
    return citation;
}

void Engine::LoadImportCheckpoint() {
    importedProjects_.clear();
    wg::Store store;
    if (!store.Open(configDir_ + L"\\" + kImportFile)) return;
    std::wstring list = store.Get(L"imported", L"");
    std::size_t pos = 0;
    while (pos <= list.size() && !list.empty()) {
        const std::size_t comma = list.find(L',', pos);
        std::wstring id = Trim(list.substr(pos, comma == std::wstring::npos
                                                      ? std::wstring::npos
                                                      : comma - pos));
        if (!id.empty()) importedProjects_.push_back(std::move(id));
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
}

void Engine::SaveImportCheckpoint() const {
    wg::Store store;
    store.Open(configDir_ + L"\\" + kImportFile);
    std::wstring list;
    for (const std::wstring& id : importedProjects_) {
        if (!list.empty()) list += L",";
        list += id;
    }
    store.Set(L"imported", list);
    store.Save();
}

void Engine::ImportProjects() {
    if (!memory_ || dshRoot_.empty()) return;
    const std::vector<DshProject> projects =
        ReadDshProjects(dshRoot_, kMaxImportedProjects);
    if (projects.empty()) return;
    const std::vector<Fact> existing = memory_->AllFacts();
    bool checkpointChanged = false;
    for (const DshProject& project : projects) {
        if (MemoryStore::IsSensitive(project.path) ||
            MemoryStore::IsSensitive(project.title)) {
            continue;
        }
        const std::wstring source = L"dsh-project-" + project.id;
        std::wstring text = project.title.empty() ? project.path : project.title;
        if (!project.title.empty() && !project.path.empty()) {
            text += L" · " + project.path;
        }
        if (text.empty()) continue;

        const bool known =
            std::find(importedProjects_.begin(), importedProjects_.end(), project.id) !=
            importedProjects_.end();
        // The checkpoint is what keeps a forgotten/cleared project from being
        // re-created on the next Start.
        if (known) continue;

        const Fact* active = nullptr;
        bool tombstone = false;
        for (const Fact& fact : existing) {
            if (fact.sourceRef != source) continue;
            if (fact.status == L"active") {
                if (!active) active = &fact;
            } else {
                tombstone = true;
            }
        }
        bool stored = true;
        if (active) {
            // Keep exactly one fact per project; an unchanged title is a no-op
            // so every Start does not bump updatedMs (which would invalidate
            // episodes that cite this project fact).
            const std::wstring current = Trim(active->text);
            if (current != Trim(text)) stored = memory_->UpdateFact(active->id, text, nullptr);
        } else if (!tombstone) {
            Fact fact;
            fact.kind = L"project";
            fact.text = text;
            fact.sourceRef = source;
            fact.confidence = 70;
            stored = memory_->AddFact(fact, nullptr);
        }
        if (!stored) continue;  // retry on the next Start
        importedProjects_.push_back(project.id);
        checkpointChanged = true;
    }
    if (checkpointChanged) SaveImportCheckpoint();
}

void Engine::LoadSessionState() {
    wg::Store store;
    if (!store.Open(configDir_ + L"\\" + kSessionFile)) return;
    sessionPath_ = store.Get(L"session_path", L"");
    sessionDigest_ = store.Get(L"facts_digest", L"");
    sessionPolicy_ = store.Get(L"policy_digest", L"");
    sessionReset_ = store.Get(L"memory_reset", L"");
    // Resuming into a deleted pi session file would fail on every Ask: verify it
    // first and fall back to a fresh session instead of a permanent start error.
    const std::filesystem::path saved(sessionPath_);
    if (!sessionPath_.empty() &&
        (!saved.is_absolute() || !FileExists(sessionPath_))) {
        ClearSessionState();
    }
}

void Engine::SaveSessionState() const {
    if (configDir_.empty()) return;
    wg::Store store;
    store.Open(configDir_ + L"\\" + kSessionFile);
    store.Set(L"session_path", sessionPath_);
    store.Set(L"facts_digest", sessionDigest_);
    store.Set(L"policy_digest", sessionPolicy_);
    store.Set(L"memory_reset", sessionReset_);
    store.Save();
}

void Engine::ClearSessionState() {
    sessionPath_.clear();
    sessionDigest_.clear();
    sessionPolicy_.clear();
    sessionEpoch_ = CurrentEpoch();
    sessionReset_ = CurrentReset();
    if (configDir_.empty()) return;
    DeleteFileW((configDir_ + L"\\" + kSessionFile).c_str());
}

long long Engine::CurrentEpoch() const {
    return epoch_ ? (long long)epoch_->load(std::memory_order_relaxed) : 0;
}

std::wstring Engine::CurrentReset() const {
    if (!memory_) return L"";
    return RevisionText(memory_->ResetRevision());
}

std::wstring Engine::FactsDigest() const {
    if (!memory_) return L"";
    std::vector<Fact> facts = memory_->AllFacts();
    std::sort(facts.begin(), facts.end(),
              [](const Fact& a, const Fact& b) { return a.id < b.id; });
    unsigned long long hash = 1469598103934665603ULL;
    for (const Fact& fact : facts) {
        // Content only: timestamps are excluded, so re-saving an unchanged
        // fact does not invalidate the session, while any edit/delete/clear
        // (a tombstone keeps its status) does.
        hash = Fnv(fact.id, hash);
        hash = Fnv(fact.kind, hash);
        hash = Fnv(fact.text, hash);
        hash = Fnv(fact.status, hash);
        hash = Fnv(fact.pinned ? L"1" : L"0", hash);
    }
    // Recent episodes are part of what the model may have answered from, so
    // their stable ids/content are part of the digest too. Done appends the
    // episode *before* saving the digest, so the next question matches and a
    // normal turn never rotates the session.
    const std::vector<Episode> episodes = memory_->RecentEpisodes(0);
    for (const Episode& episode : episodes) {
        hash = Fnv(episode.id, hash);
        hash = Fnv(episode.question, hash);
        hash = Fnv(episode.answer, hash);
        hash = Fnv(episode.sources, hash);
    }
    return Hex64(hash);
}

std::wstring Engine::PolicyDigest() const {
    unsigned long long hash = 1469598103934665603ULL;
    hash = Fnv(L"m1-zero-tools", hash);
    hash = Fnv(provider_, hash);
    hash = Fnv(model_, hash);
    hash = Fnv(providerExtension_, hash);
    hash = Fnv(piCommand_, hash);
    return Hex64(hash);
}

long long Engine::DailyTokensUsed() const {
    if (configDir_.empty()) return 0;
    wg::Store store;
    if (!store.Open(configDir_ + L"\\assistant-cost.ini")) return 0;
    wchar_t key[64];
    swprintf(key, 64, L"day.%ls.tokens", LocalDay(NowMs()).c_str());
    return store.GetInt(key, 0);
}

bool Engine::BudgetExhausted() const {
    if (dailyTokenLimit_ <= 0) return false;
    return DailyTokensUsed() >= dailyTokenLimit_;
}

std::wstring Engine::EffectiveKeyLiteral() const {
    if (provider_.empty()) return L"";
    std::wstring envName;
    if (!ProviderEnvName(provider_, &envName)) return L"";
    std::wstring key;
    if (!keyRef_.empty()) key = aimeter::ReadDshRef(keyRef_);
    if (key.empty()) key = Env(envName.c_str());
    return key.size() >= kMinSecretChars ? key : L"";
}

void Engine::RecordCost(long long inputTokens, long long outputTokens, long long elapsedMs) {
    if (configDir_.empty()) return;
    wg::Store store;
    const std::wstring path = configDir_ + L"\\assistant-cost.ini";
    store.Open(path);
    const std::wstring day = LocalDay(NowMs());
    wchar_t key[64];
    swprintf(key, 64, L"day.%ls.tokens", day.c_str());
    store.SetInt(key, store.GetInt(key, 0) + inputTokens + outputTokens);
    swprintf(key, 64, L"day.%ls.calls", day.c_str());
    store.SetInt(key, store.GetInt(key, 0) + 1);
    swprintf(key, 64, L"day.%ls.ms", day.c_str());
    store.SetInt(key, store.GetInt(key, 0) + elapsedMs);
    store.Save();
}

} // namespace assistant
