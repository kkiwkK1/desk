// Multi-account CommandCode data for the Account card (M0a).
//
// Scope: account configuration (aimeter.ini), credential resolution, one
// GET /alpha/billing/credits per account and the aggregated board snapshot.
//
// Secrets stay in memory: this module never writes a key to disk or to the
// log - only source names ("DSH REF" / "CRED") are logged or rendered.
// Rendering consumes AccountBoardData; daily history lives in history.h.
//
// Threading: FetchAccounts blocks and may be called from the refresh worker
// only. Everything else is pure or touches wg::Store, which is thread-safe.
#pragma once

#include "widget/store.h"

#include <string>
#include <vector>

namespace aimeter {

// Where a key comes from. Credential-manager targets are
// AiMeter/CommandCode/<label>; account 1 additionally honours the legacy
// AiMeter/CommandCode entry.
enum class Source { Dsh, Credman };

const wchar_t* SourceKey(Source source);    // "dsh" / "credman" (ini form)
const wchar_t* SourceTag(Source source);    // "DSH REF" / "CRED" (UI form)
Source ParseSource(const std::wstring& text);  // unknown -> Source::Dsh

// Account count is capped so one refresh cannot spawn an unbounded fan-out.
constexpr int kMaxAccounts = 4;
// wg::HttpGet already uses an 8s receive timeout; kept named here so the
// contract ("each account gets 8s") is visible from this header.
constexpr int kFetchTimeoutMs = 8000;
// Error codes stored in AccountSnapshot::error.
inline constexpr wchar_t kErrNet[] = L"NET ERR";    // transport failure
inline constexpr wchar_t kErrBadKey[] = L"BAD KEY";  // 401/403
inline constexpr wchar_t kErrNoData[] = L"NO DATA";  // 200 but fields missing
inline constexpr wchar_t kErrNoKey[] = L"NO KEY";    // no credential resolved

// One configured account, as stored in aimeter.ini:
//   accounts=N
//   account.<i>.label=ACC1
//   account.<i>.keyref=COMMANDCODE_API_KEY
//   account.<i>.source=dsh
//   account.<i>.monthly_limit=          (empty / 0 = unset)
struct AccountConfig {
    int index = 0;                 // 1..N, also the history key index
    std::wstring label;            // default "ACC1", "ACC2" (ASCII: card glyphs)
    std::wstring keyRef;           // DSH ref name, e.g. COMMANDCODE_API_KEY
    Source source = Source::Dsh;
    // Optional manual monthly cap, same unit as monthlyCredits; 0 = unset.
    double monthlyLimit = 0.0;
};

struct QuotaWindow {
    double used = 0.0;
    double cap = 0.0;
    long long resetAtMs = 0;       // Unix epoch ms; 0 = no window
    bool exceeded = false;

    bool present() const { return cap > 0.0 || used > 0.0 || resetAtMs > 0; }
    double percent() const { return cap > 0.0 ? used * 100.0 / cap : 0.0; }
};

// One account's last fetch result. asOfMs is 0 until the first success, and a
// failed account keeps its previous asOfMs (so IsStale reports its age).
struct AccountSnapshot {
    std::wstring label;
    std::wstring keyRef;
    Source source = Source::Dsh;
    bool ok = false;
    std::wstring error;            // NET ERR / BAD KEY / ERR <n> / NO DATA / NO KEY
    QuotaWindow fiveHour;
    QuotaWindow weekly;
    double monthlyCredits = 0.0;
    bool belowThreshold = false;
    double creditThreshold = 0.0;
    long long sandboxUsedMinutes = 0;
    long long sandboxLimitMinutes = 0;
    long long sandboxResetMs = 0;
    double sandboxOverageUsd = 0.0;
    long long asOfMs = 0;
    double monthlyLimit = 0.0;     // copied from AccountConfig, 0 = unset
};

struct AccountBoardData {
    std::vector<AccountSnapshot> accounts;
    double tightestPct = 0.0;      // max(used/cap) over windows with cap > 0
    double monthlyCreditsTotal = 0.0;
    bool anyOk = false;
};

// --- Configuration -------------------------------------------------------
// True when aimeter.ini already has an explicit accounts= key (no import).
bool HasAccountConfig(const wg::Store& config);
// Reads accounts/account.<i>.* without importing or writing anything.
std::vector<AccountConfig> ReadAccounts(const wg::Store& config);
// Pure import path: refs COMMANDCODE_API_KEY, COMMANDCODE_API_KEY_2, ... in
// the given credentials.yaml text -> labelled accounts (cap kMaxAccounts).
std::vector<AccountConfig> ImportAccounts(const std::string& credentialsYaml);
// Reads config; when accounts= is missing, imports the DSH refs once and
// writes them back (idempotent: the written keys stop every later import).
std::vector<AccountConfig> LoadAccounts(wg::Store& config);
// Writes accounts= and account.<i>.* ; keys for removed accounts are dropped.
void SaveAccounts(wg::Store& config, const std::vector<AccountConfig>& accounts);

// --- DSH credential file (~/.dsh/.credentials.yaml) ---------------------
// Ref names only (never values): COMMANDCODE_API_KEY, _2, _3, ... sorted.
std::vector<std::wstring> DshCcRefNames(const std::string& credentialsYaml);
std::vector<std::wstring> LoadDshRefNames();
// Secret value for a ref, or empty when absent/unreadable. Never logged.
std::wstring ReadDshRef(const std::wstring& refName);

// --- Credentials and fetch ----------------------------------------------
std::wstring CredentialTarget(const std::wstring& label);  // AiMeter/CommandCode/<label>
// Resolution order: by source (dsh ref / credman target), then the other one,
// then - account 1 only - the legacy AiMeter/CommandCode entry.
std::wstring ResolveAccountKey(const AccountConfig& account, bool legacyFallback = true);
// One blocking fetch. key must come from ResolveAccountKey; never logged.
AccountSnapshot FetchAccount(const AccountConfig& account, const std::wstring& key,
                             long long nowMs);
// Concurrent fetch of every account: at most 4 workers, 8s each, failures
// isolated per account (never abort the whole refresh).
AccountBoardData FetchAccounts(const std::vector<AccountConfig>& accounts, long long nowMs);

// --- Parsing and aggregation (pure) -------------------------------------
// Parses a 200 body from /alpha/billing/credits. Extended from
// commandcode.cpp: adds resetAt, belowThreshold/creditThreshold, sandboxMinutes.
AccountSnapshot ParseAccountBody(const std::string& body, long long nowMs);
// Aggregates snapshots that have any data (asOfMs > 0): tightest percentage
// and monthly credit total. Failed accounts without data are skipped, so one
// broken key cannot blank the card.
AccountBoardData Aggregate(std::vector<AccountSnapshot> snapshots);
// Keeps the last good numbers of a failed account: previous values + fresh
// error + previous.asOfMs (UI marks it STALE and the age keeps growing).
// When the previous snapshot never succeeded (asOfMs == 0), fresh is returned.
AccountSnapshot CarryForward(const AccountSnapshot& fresh, const AccountSnapshot& previous);

// --- UI helpers (pure) ---------------------------------------------------
bool AllAccountsOk(const AccountBoardData& data);
// asOfMs <= 0 counts as stale: nothing has ever succeeded.
bool IsStale(long long asOfMs, long long nowMs, long long staleAfterMs = 600000);
std::wstring AgeText(long long ageMs);                     // "12M", "3H 12M", "5D 6H"
bool HasReset(const QuotaWindow& window);                  // resetAtMs > 0
std::wstring ResetCountdown(long long resetAtMs, long long nowMs);  // "" = no window
std::wstring PercentText(const QuotaWindow& window);       // "39%" / "--"
std::wstring CreditsText(double credits);                  // "0.78"
// Card copy stays ASCII (the 5x7 glyph set has no CJK):
// "2 ACCTS / TIGHT 73% / MONTH 60.45", or "2 ACCTS / ERROR" when all failed.
std::wstring AggregateText(const AccountBoardData& data);

} // namespace aimeter
