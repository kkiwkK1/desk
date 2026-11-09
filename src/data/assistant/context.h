// DSH context + prompt assembly for the assistant (M1.1).
//
// Sources (docs/assistant-m1-plan.md §2/§3):
//   ~/.dsh/storages/workspace.json                          project title/path/updatedAt
//   ~/.dsh/storages/session_projcache/sessions/*.json       last turnOutline response
//   MemoryStore                                             durable facts + recent episodes
//
// Only the plain-JSON projection cache is read; the zstd session bodies under
// ~/.dsh/sessions are deliberately left untouched. Everything that reaches the
// prompt is filtered for sensitive paths/credentials, and the send log records
// only ids, file names and sizes - never prompt/answer content.
//
// Recent episodes are re-injected under the same character budget but fail
// closed: an episode is dropped when its answer cites a deleted/unknown memory
// id, cites a fact that was edited after the answer, or contains the text of a
// forgotten fact. That keeps "forget" from coming back through the episode log.
#pragma once

#include "data/assistant/memory.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace assistant {

// One DSH project from ~/.dsh/storages/workspace.json.
struct DshProject {
    std::wstring id;          // workspace uuid
    std::wstring title;
    std::wstring path;
    long long updatedMs = 0;  // updatedAt parsed to Unix ms; 0 when unknown
};

// One DSH session conclusion from the project cache.
struct DshSession {
    std::wstring id;            // session uuid taken from the cache file name
    std::wstring title;         // rows.title when set (may be empty)
    std::wstring cwd;           // identity.cwd
    long long updatedMs = 0;    // cache file write time = last activity
    std::wstring lastResponse;  // last non-empty turnOutline turn response
};

// %USERPROFILE%\.dsh; empty-string fallback when USERPROFILE is unset.
// An isolated test instance (non-empty DESK_TEST_INSTANCE) never resolves to
// the live profile: DESK_TEST_DSH_ROOT overrides the root when set, otherwise
// the root is <exe dir>\test-dsh, which does not exist (empty DSH).
std::wstring DefaultDshRoot();

// Plain scans of the DSH projection cache. Missing files/directories yield an
// empty result (never a failure). Newest first, at most 'limit' entries.
std::vector<DshProject> ReadDshProjects(const std::wstring& dshRoot, int limit);
std::vector<DshSession> ReadDshSessions(const std::wstring& dshRoot, int limit);

struct ContextBundle {
    std::wstring prompt;                 // complete context injected into pi
    std::vector<std::wstring> sourceIds; // ids the answer may cite
};
class ContextBuilder {
public:
    // configDir receives assistant-send.log; dshRoot empty -> DefaultDshRoot().
    // 'memory' must outlive the builder (the engine owns both).
    ContextBuilder(std::wstring configDir, std::wstring dshRoot,
                   const MemoryStore& memory);

    // Assembles the prompt for one question. budgetTokens <= 0 -> 6000 tokens,
    // with 1 token ~= 2 characters (plan §7). The prompt always fits the budget.
    ContextBundle Build(long long nowMs, int budgetTokens = 6000,
                        std::wstring_view question = {});

    const std::wstring& send_log_path() const { return sendLogPath_; }

private:
    bool AppendSendLog(long long nowMs, const ContextBundle& bundle,
                       int promptChars, int budgetTokens) const;

    std::wstring configDir_;
    std::wstring dshRoot_;
    const MemoryStore& memory_;
    std::wstring sendLogPath_;
};

} // namespace assistant
