// Native-only proposal/file lifecycle core. Not wired to menus or models yet.
// A trusted host supplies an absolute local directory, never a spec/model path.
#pragma once
#include "data/cardspec/cardspec.h"
#include <functional>
#include <string>
#include <string_view>

namespace cardspec::store {

inline constexpr wchar_t kActiveFile[] = L"cardspec.json";
inline constexpr wchar_t kBackupFile[] = L"cardspec.json.bak";
inline constexpr wchar_t kTempFile[] = L"cardspec.json.tmp";

class Candidate {
public:
    bool valid() const { return !canonical_.empty(); }
    const Spec& spec() const { return spec_; }
    const std::string& canonical() const { return canonical_; }
private:
    Spec spec_;
    std::string canonical_;
    friend bool Prepare(std::string_view, Candidate*, std::wstring*);
};

// Raw bytes preserve BOM/spacing for exact restoration; SHA-256 is lowercase hex.
// Missing: exists=false, bytes/digest empty. Empty existing files have SHA256("").
struct Snapshot {
    bool exists = false;
    std::string bytes;
    std::string digest;
};

enum class Result {
    Ok, InvalidCandidate, InvalidDirectory, Refused, TooLarge, Busy,
    InvalidBaseline, Conflict, NoBackup, InvalidBackup, AlreadyMatches,
    IoError, Raced, ChangedDuringWrite
};

enum class TargetEffect { None, Replaced, Unknown };
struct Outcome {
    Result result = Result::IoError;
    TargetEffect effect = TargetEffect::None; // this operation, not external writers
    Snapshot observed;
    std::wstring message;                    // static, no paths or payload
    bool ok() const { return result == Result::Ok; }
};

struct Hooks {
    // Test-only native seam, after preflight/temp flush and before promotion.
    // No schema/config/model field can supply this callback. It is called once.
    std::function<void()> beforePromote;
};

// v1 inline only. No I/O; failure leaves *out unchanged; success clears error.
bool Prepare(std::string_view utf8, Candidate* out, std::wstring* error = nullptr);

// Pure reads of fixed leaves, bounded by kMaxSpecBytes. Missing is Ok + !exists.
// Read-only ordinary files can be sampled. Failure leaves *out unchanged;
// success clears error. Observed metadata changes during reading are Raced.
// Reject relative/UNC/device/remote/reparse directories and nonregular/reparse/
// multi-hardlink leaves. Final directory and leaves are checked, not a sandbox
// against malicious replacement of an ancestor directory by the same user.
Result ReadActive(const std::wstring& trustedDirectory, Snapshot* out,
                  std::wstring* error = nullptr);
Result ReadBackup(const std::wstring& trustedDirectory, Snapshot* out,
                  std::wstring* error = nullptr);

// All writes are opt-in native calls. Existing active files must also be valid
// v1 inline. A detected baseline conflict performs no target/backup/temp writes.
// One cooperating named mutex serializes operations in this Windows session.
// Existing active: ReplaceFileW atomically promotes the flushed temp and saves
// the replaced bytes to the single backup. Missing active: no-replace rename.
// A pre-existing temp is Busy, never truncated/deleted. Reject unsafe backup
// even for creation; no ini, network, exec, or other source paths are involved.
Outcome Apply(const std::wstring& trustedDirectory, const Candidate& candidate,
              const Snapshot& expectedCurrent, const Hooks& hooks = {});

// Restore the exact bytes of a valid v1 backup; active must also be valid v1.
// Require both current and backup
// baselines, then atomically replace active and swap the former active into bak.
// With no backup (including first-ever creation), return NoBackup: no implicit
// deletion-style undo. If backup exists but active is missing, return Refused;
// this first stage only swaps an existing active file, not missing-file recovery.
// This is a single-slot restore, not persistent history.
Outcome Rollback(const std::wstring& trustedDirectory,
                 const Snapshot& expectedCurrent, const Snapshot& expectedBackup,
                 const Hooks& hooks = {});

// NOT a strong filesystem CAS or power-loss transaction: noncooperating writers
// can change directory entries between validation and promotion. Re-read active
// and backup afterwards; report Raced/ChangedDuringWrite or Unknown effect when
// appropriate, never auto-rollback over another writer's update. Unknown temp
// state is preserved rather than blindly deleting a possibly replaced file.

} // namespace cardspec::store
