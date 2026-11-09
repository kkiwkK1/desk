// Long-term assistant memory (M1.1).
//
// Persistence is UTF-8 JSON Lines in the assistant config directory
// (see docs/assistant-m1-plan.md §3):
//
//   assistant-episodes.jsonl   append-only: one question/answer turn per line
//   assistant-facts.jsonl      rewritten atomically (temp file + MoveFileEx)
//   assistant-memory-reset.txt one reset id; a durable "the user cleared
//                              memory" witness that survives a restart even
//                              when both data files were already empty
//
// Facts carry a source reference so answers can cite them ([[mem:<id>]]) and
// the MEMORY card can show where a memory came from. Sensitive material (SSH /
// credential paths, API keys) is rejected before it can reach either file, and
// facts are capped at kFactCap entries.
//
// Thread-safe: the engine appends/updates from its worker thread while the
// MEMORY card reads from the UI thread.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace assistant {

// One durable memory fact (docs/assistant-m1-plan.md §2/§3).
struct Fact {
    std::wstring id;          // "mem-<hex>", stable; cited as [[mem:<id>]]
    std::wstring kind;        // goal / preference / constraint / milestone / project / fact
    std::wstring text;        // the remembered statement (single line)
    std::wstring sourceRef;   // episode id / DSH session id / workspace path
    int confidence = 80;      // 0..100; a user correction can raise it
    std::wstring status = L"active";  // "active" | "deleted" (soft delete)
    long long createdMs = 0;
    long long updatedMs = 0;
    long long expiresMs = 0;  // 0 = never
    bool pinned = false;      // MEMORY card pin; pinned facts list first
};

// One persisted question/answer turn (append-only episode log).
struct Episode {
    std::wstring id;          // "ep-<hex>"
    long long createdMs = 0;
    std::wstring question;
    std::wstring answer;
    std::wstring sources;     // comma-separated source ids cited in the answer
};

// Hard cap on active facts; the weakest are dropped first when it is exceeded.
constexpr int kFactCap = 2000;
// Facts not touched for this long lose half their retrieval score (90 days).
constexpr long long kFactDecayMs = 90LL * 24 * 60 * 60 * 1000;

class MemoryStore {
public:
    // Load reads assistant-facts.jsonl, the tail of the episode log and the
    // persisted reset id. A missing directory or file is not an error (first
    // run). Returns false when a file existed but could not be read/parsed,
    // including the reset marker: an unreadable reset id must fail closed
    // rather than look like "never cleared", which would let an old Pi session
    // survive a restart.
    // (OpenSharedMemory never hands out a store whose Load failed: it returns
    // nullptr instead, so shared callers cannot write over an unread file.)
    bool Load(const std::wstring& configDir, std::wstring* error);

    // --- Episodes (append-only) -------------------------------------------
    // Appends one JSONL line to assistant-episodes.jsonl, creating the config
    // directory when needed.
    bool AppendEpisode(const std::wstring& question, const std::wstring& answer,
                       const std::wstring& sources, std::wstring* error);
    // Newest last; at most 'limit' entries (limit <= 0 -> all loaded ones).
    std::vector<Episode> RecentEpisodes(int limit) const;

    // --- Facts -------------------------------------------------------------
    // Inserts or merges. Among *active* facts the same kind+text
    // (case-insensitive) updates the existing fact instead of duplicating it.
    // A soft-deleted fact is never revived: remembering the same kind+text
    // again creates a new active fact with its own id, while the old id stays a
    // "deleted" tombstone in AllFacts(). Rejects sensitive material
    // (kFactSensitive), empty text (kFactEmpty) and enforces kFactCap.
    bool AddFact(const Fact& fact, std::wstring* error);
    // Corrects the text of an existing fact; bumps confidence and updatedMs.
    bool UpdateFact(const std::wstring& id, const std::wstring& text,
                    std::wstring* error);
    // Forgets a fact: soft delete ("deleted"), so it leaves Facts()/Query()
    // immediately and stays inspectable in AllFacts() for diagnostics.
    bool DeleteFact(const std::wstring& id, std::wstring* error);
    // Pins or unpins a fact (MEMORY card "Pin fact"). Pinned facts sort first
    // in Facts(). Pinning leaves updatedMs alone on purpose: it is a display
    // flag, not a content touch, so it must not refresh the 90-day recency /
    // decay that Query() uses (use UpdateFact to re-touch a fact).
    // An unknown or soft-deleted id reports kFactNotFound. When the fact is
    // already in the requested state this returns true without rewriting the
    // file and without notifying subscribers.
    bool SetPinned(const std::wstring& id, bool pinned, std::wstring* error = nullptr);

    // Forgets *everything*: the active facts, their soft-deleted tombstones and
    // the whole episode log (assistant-facts.jsonl and
    // assistant-episodes.jsonl are each rewritten empty). Backs the MEMORY
    // card's "Clear all memory" and the engine's local "清空" command.
    //
    // Order matters, because the engine folds ResetRevision() into its session
    // digest: (1) a new reset id is written atomically first and the in-memory
    // revision advances as soon as that write succeeds; only then (2) the facts
    // file and (3) the episode file are emptied. The reset id is the durable
    // "a clear happened" witness, so a clear still invalidates an old Pi
    // session when facts and episodes were already empty, and it survives a
    // restart - a digest over the data files alone could not tell that case
    // apart from "nothing ever happened".
    //
    // A failure is never reported as success: the in-memory lists are cleared
    // only after both data rewrites returned, and the call reports
    // kFactWriteFailed. *error is cleared on entry, so a successful clear never
    // returns a message left over from an earlier call. If the *revision* write
    // fails nothing else is touched and the revision stays as it was. If the
    // revision was written but a data
    // rewrite then fails, the revision has already advanced - a deliberately
    // conservative invalidation (the old session stays dropped) - while the
    // facts/episodes keep their old rollback rules (facts are restored from
    // the snapshot when the episode rewrite fails) and no subscriber is
    // notified; the caller still sees false, so no data clear is ever faked.
    //
    // There is deliberately no transaction across the three files: a crash
    // between rewrites can leave them out of step - metadata ahead of data is
    // expected and harmless (the next clear retries), so this is ordered
    // best-effort cleanup, not atomicity.
    //
    // On success subscribers are notified exactly once, outside the data lock;
    // a failed clear notifies nobody (see the change-notification notes below).
    // That notification is unconditional: it also fires when facts and episodes
    // were already empty, because it doubles as the user's explicit "forget
    // this conversation" barrier - the engine drops any old Pi session (which
    // may contain cancelled or failed turns) on that signal, so an early return
    // for "no data changed" would leave that stale session alive.
    bool ClearAll(std::wstring* error = nullptr);

    // The persisted id of the last successful clear ("" when the user has
    // never cleared). Load() reads it from assistant-memory-reset.txt; ClearAll
    // writes a fresh one before touching the data files. It is a copy taken
    // under the store lock, so callers (the engine's session digest) can use it
    // on any thread. Unchanged by AddFact/UpdateFact/DeleteFact/SetPinned - a
    // data edit changes the facts digest instead.
    std::wstring ResetRevision() const;

    // Active facts only, pinned first, then most recently updated first. What
    // the MEMORY card lists by default; rows are addressed by id, never by
    // index, because a pin/update reorders this list.
    std::vector<Fact> Facts() const;
    // Includes soft-deleted facts (tests / diagnostics).
    std::vector<Fact> AllFacts() const;
    // Ranked retrieval for the context builder and the MEMORY card search:
    // keyword hits on text/kind/sourceRef, recency bonus, 90-day decay.
    // nowMs = 0 -> current time; limit <= 0 -> every matching fact.
    std::vector<Fact> Query(std::wstring_view text, int limit,
                            long long nowMs = 0) const;

    // Sensitive-path / credential sniffing shared with the context builder.
    // True for .ssh, .credentials, *.pem, id_rsa/id_ed25519, PRIVATE KEY,
    // sk-/Bearer tokens and *_API_KEY / credential names.
    static bool IsSensitive(std::wstring_view text);

    const std::wstring& config_dir() const { return configDir_; }

    // --- change notification ----------------------------------------------
    // Fires after a fact change was durably written: AddFact (insert and
    // merge), UpdateFact, DeleteFact, SetPinned and ClearAll. A change whose
    // atomic rewrite failed is rolled back and never notifies, so a subscriber
    // that repaints from Facts() cannot show a fact the file does not have.
    //
    // Handlers run on the mutating thread, *after* the store's data lock is
    // released, and each handler is isolated: a handler that throws is
    // swallowed (the fact stays stored) and the remaining handlers still run.
    // A handler may call Facts() / AllFacts() / Query() and Subscribe /
    // Unsubscribe without deadlocking. AppendEpisode does not notify (the
    // MEMORY card lists facts, not episodes).
    using ChangeHandler = std::function<void()>;

    // Registers a handler and returns its token; an empty handler returns 0 and
    // registers nothing, every other call gets a distinct non-zero token.
    uint64_t Subscribe(ChangeHandler handler);
    // Drops a token so it receives no *future* notifications. It deliberately
    // does not wait for a notification already in flight: the handler list is
    // snapshotted before it is called, so a handler can still run once after
    // Unsubscribe returns. Callers (the MEMORY card) must therefore capture
    // shared/weak state - e.g. a std::weak_ptr to the card's state - rather
    // than a raw 'this', so a late callback cannot touch a destroyed card.
    void Unsubscribe(uint64_t token);

private:
    bool LoadFacts(std::wstring* error);
    bool LoadEpisodes();
    bool LoadResetRevision(std::wstring* error);
    bool SaveFacts(std::wstring* error);
    // SaveFacts + rollback: a failed atomic rewrite restores 'before' so the
    // in-memory facts never claim a change the file on disk does not have.
    bool SaveFactsOrRollback(const std::vector<Fact>& before, std::wstring* error);
    void TrimFactsToCap();
    // Copies the subscriber list and runs it; never called with mu_ held.
    void NotifyChanged();

    std::wstring FactsPath() const;
    std::wstring EpisodesPath() const;
    std::wstring ResetPath() const;

    std::wstring configDir_;
    std::vector<Fact> facts_;
    std::vector<Episode> recentEpisodes_;
    // Last persisted clear id; guarded by mu_ like the two lists above.
    std::wstring resetRevision_;
    mutable std::mutex mu_;  // data lock; never held while notifying
    std::mutex subscribersMu_;  // leaf lock: subscribers_ and nextToken_
    std::vector<std::pair<uint64_t, ChangeHandler>> subscribers_;
    uint64_t nextToken_ = 1;
};

// One MemoryStore per config directory, shared by the engine worker and the
// MEMORY card so both see the same facts and the same change notifications.
//
// The registry holds only weak_ptr. The returned shared_ptr owns the store: as
// soon as every caller releases it the instance is gone and the next call
// creates (and Loads) a fresh one. The key is the directory made absolute
// (GetFullPathNameW when it succeeds, otherwise the text as given), '/' folded
// to '\', trailing separators stripped (except at a drive root, so "C:\" keeps
// its separator and never shares a key with the drive-relative "C:") and the
// whole path lower-cased - so "C:\data\memory" and "c:/DATA/MEMORY/" map to
// one entry. Load() keeps the calling spelling in config_dir() the first time.
//
// Load runs at most once per live store and runs under the registry lock, so
// two threads cannot create two stores for one directory. An already loaded
// store is never reloaded behind its callers' backs, and every caller gets the
// same pointer, so Engine and the MEMORY card can compare pointers.
//
// A store whose Load failed is never handed out and never cached: the call
// returns nullptr with *error set (kFactLoadFailed, or the parse failure), so
// nullptr means "memory is unavailable" and callers must not treat it as a
// healthy store. Nothing was written; after the file is fixed the next call
// loads normally and is cached from then on.
//
// An empty directory cannot be shared and also returns nullptr, with
// kMemoryDirEmpty.
std::shared_ptr<MemoryStore> OpenSharedMemory(const std::wstring& configDir,
                                              std::wstring* error = nullptr);

// Error strings handed back through std::wstring* error (pointer-stable).
extern const wchar_t* const kFactSensitive;
extern const wchar_t* const kFactEmpty;
extern const wchar_t* const kFactNotFound;
extern const wchar_t* const kFactWriteFailed;
extern const wchar_t* const kFactLoadFailed;
extern const wchar_t* const kMemoryDirEmpty;

} // namespace assistant
