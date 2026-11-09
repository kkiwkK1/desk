// M1 assistant engine: question -> local command or DSH/memory context -> pi
// RPC -> streaming answer -> citation verification -> episode/cost persistence.
//
// Two request kinds share one lifecycle (ready/busy/settling):
//   * remote: context build + pi prompt + streaming + citation + persistence;
//   * local:  explicit memory commands (remember / forget / clear all) run
//     without pi, network, balance or cost accounting.
//
// Threading: Ask() returns immediately and does the work on a worker thread.
// Callbacks run on that worker (or on pi's reader thread for deltas); the
// card's mutators are thread-safe by contract. Stop() is silent.
#pragma once

#include "data/assistant/context.h"
#include "data/assistant/memory.h"
#include "data/assistant/pi_client.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace assistant {

class Engine {
public:
    struct Callbacks {
        std::function<void(std::wstring_view)> onDelta;
        // citation is "" when the model cited nothing that exists; tokens and
        // elapsed are for the card's footer.
        std::function<void(const std::wstring& citation, long long inputTokens,
                           long long outputTokens, long long elapsedMs)> onDone;
        // Terminal failure/cancel only; no onDone follows this callback.
        std::function<void(std::wstring_view)> onError;
    };

    // Test/QA seam. Production passes nothing ({}): the DSH root comes from the
    // environment and the idle timeout from assistant.ini (10 minutes).
    struct Options {
        std::wstring dshRoot;  // empty -> DefaultDshRoot()
        // >0 overrides idle_stop_minutes with an exact millisecond timeout, so
        // regressions never wait the real 10 minutes.
        int idleStopMs = 0;
    };

    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Start/Stop are serialized by the lifecycle owner (UI); do not call them
    // from a Pi event callback. Stop is silent and interrupts pending startup.
    // Callbacks must be marshalled back to the UI thread: an Ask() issued from
    // an engine callback is refused with a visible error (no thread is detached
    // or self-joined) and Start/Stop stay owned by the lifecycle thread.
    // Loads memory, imports DSH projects once (checkpointed), restores the
    // saved session and writes the default persona when missing. Does not start
    // pi yet (the first remote Ask does, so an idle board costs nothing).
    bool Start(const std::wstring& configDir, std::wstring* error = nullptr);
    bool Start(const std::wstring& configDir, const Options& options,
               std::wstring* error = nullptr);
    void Stop();

    // Empty question or a second Ask while one is running reports an error.
    // "记住/remember X", "忘掉/forget X" and "清空全部记忆/clear all memory"
    // are handled locally and deterministically (no pi, no network). A local
    // remember refuses the current provider credential literal; a local forget
    // is never gated (deleting a leaked secret must work) and never echoes the
    // payload it removed.
    void Ask(const std::wstring& question, Callbacks callbacks);
    void Cancel();

    // BRIEF block: real DSH projects with their latest project-cache
    // conclusion and blocker ("卡点：未记录" when the cache has none).
    std::wstring Brief();
    // Available after a successful Start, also after Stop for inspection.
    MemoryStore& memory() { return *memory_; }

    // Effective idle timeout in ms (0 = disabled). Exposed for regressions.
    int idle_stop_ms() const { return idleStopMs_; }

private:
    enum class LocalCommand { None, Remember, Forget, ClearAll };
    struct ParsedCommand {
        LocalCommand kind = LocalCommand::None;
        std::wstring text;
    };

    static ParsedCommand ParseLocalCommand(const std::wstring& question);
    static std::wstring RememberKind(const std::wstring& text);

    void HandleEvent(const PiEvent& event);
    void RunLocal(long long requestId, LocalCommand kind, const std::wstring& text);
    void RunRemote(long long requestId, const std::wstring& question);
    void PublishLocal(long long requestId, const std::wstring& delta, bool ok,
                      const std::wstring& error);
    void FailRequest(long long requestId, const std::wstring& error);
    bool RequestActiveLocked(long long requestId) const;

    void IdleLoop();
    void NoteActivity();
    void LoadSettings();
    void ResetRequestLocked();

    // Project facts imported from DSH once per project id (assistant-import.ini
    // checkpoint), skipping anything the user already forgot or cleared.
    void ImportProjects();
    void LoadImportCheckpoint();
    void SaveImportCheckpoint() const;

    // Session continuity (assistant-session.ini): the pi session path plus the
    // fact/policy digests it belongs to. Only a path and hashes are stored.
    void LoadSessionState();
    void SaveSessionState() const;
    void ClearSessionState();
    long long CurrentEpoch() const;
    std::wstring CurrentReset() const;
    std::wstring FactsDigest() const;
    std::wstring PolicyDigest() const;

    // Daily token budget (assistant-cost.ini day.<date>.tokens).
    long long DailyTokensUsed() const;
    bool BudgetExhausted() const;

    // The concrete credential value we would hand to pi, if any. Used to fail
    // closed when it shows up in a question/persona/context; never logged.
    std::wstring EffectiveKeyLiteral() const;

    std::wstring BuildCitation(const std::wstring& answer,
                               const std::vector<std::wstring>& sources) const;
    void RecordCost(long long inputTokens, long long outputTokens, long long elapsedMs);

    bool enabled_ = true;
    int contextBudget_ = 6000;
    int idleStopMinutes_ = 10;
    int idleStopMs_ = 10 * 60 * 1000;
    long long dailyTokenLimit_ = 0;
    std::wstring piCommand_;
    std::wstring tools_;  // kept for compatibility; M1 always runs zero-tool
    std::wstring provider_ = L"command-code";
    std::wstring model_ = L"deepseek/deepseek-v4.1-flash-fast";
    std::wstring keyRef_ = L"COMMANDCODE_API_KEY_2";
    std::wstring providerExtension_;

    std::wstring configDir_;
    std::wstring personaPath_;
    std::wstring dshRoot_;
    std::shared_ptr<MemoryStore> memory_;
    std::unique_ptr<ContextBuilder> context_;
    // shared_ptr so Cancel/idle/worker can hold a reference while another path
    // is discarding the wrapper (a local memory mutation drops it on purpose).
    std::shared_ptr<PiClient> pi_;
    std::mutex piMu_;  // worker startup/Ask versus idle Stop; never held by UI

    std::mutex mu_;
    Callbacks callbacks_{};
    std::wstring question_;
    std::wstring answer_;
    std::size_t emitted_ = 0;
    std::vector<std::wstring> sources_;
    long long askStartMs_ = 0;
    long long requestId_ = 0;  // identity of the request that owns callbacks_
    long long requestEpoch_ = 0;
    bool ready_ = false;  // lifecycle owner calls Start/Stop; protected by mu_
    bool busy_ = false;
    bool settling_ = false;  // RPC ended; persistence still owns the request
    bool requestFailed_ = false;
    std::wstring requestError_;
    bool cancelRequested_ = false;
    LocalCommand localCommand_ = LocalCommand::None;
    long long inputTokens_ = 0;
    long long outputTokens_ = 0;
    std::thread worker_;

    // Memory change epoch: the engine only holds a weak reference to the shared
    // counter (never 'this'), so a late notification cannot touch a dead engine.
    std::shared_ptr<std::atomic<unsigned long long>> epoch_;
    uint64_t subscription_ = 0;

    // assistant-session.ini
    std::wstring sessionPath_;
    std::wstring sessionDigest_;
    std::wstring sessionPolicy_;
    // Memory epoch the saved session belongs to. Any memory change after that
    // (including a ClearAll of an already empty store, which still notifies)
    // invalidates the session even when the fact digest happens to match.
    long long sessionEpoch_ = 0;
    // Persisted memory reset revision (MemoryStore::ResetRevision) captured when
    // the session was saved. It survives restarts, so a ClearAll performed while
    // the engine was stopped cannot be missed when the digest is unchanged.
    std::wstring sessionReset_;

    // assistant-import.ini
    std::vector<std::wstring> importedProjects_;

    // Idle stop: after the last answer, stop the pi child to give its memory
    // back; the next Ask restarts it and resumes the session.
    std::thread idleThread_;
    std::mutex idleMu_;
    std::condition_variable idleCv_;
    unsigned long long activityGen_ = 0;
    bool idleStop_ = false;
};

} // namespace assistant
