#include "pi_client.h"

#include "json.h"

#include "widget/diag.h"
#include "widget/proc.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace assistant {
namespace {

constexpr const wchar_t* kLogCategory = L"pi";
constexpr const wchar_t* kSessionsDirName = L"pi-sessions";
constexpr const wchar_t* kCliRelative =
    L"node_modules\\@earendil-works\\pi-coding-agent\\dist\\bundle\\cli.js";
constexpr std::size_t kMaxLineBytes = 4u * 1024u * 1024u;  // one JSONL record
constexpr std::size_t kMaxPromptBytes = 512u * 1024u;
constexpr int kStopGraceMs = 2000;
// How much masked diagnostic text is kept for an exit message / bug report.
constexpr std::size_t kMaxDiagTail = 2048;
// A credential shorter than this cannot be told apart from ordinary text, so
// only longer `--api-key` values from the command line become literal patterns.
constexpr std::size_t kMinDiagLiteralBytes = 4;
// Abort ids that may be outstanding at once. Aborts are fire-and-forget (the UI
// must not wait for them), so the set is capped instead of growing unbounded;
// the oldest entry is dropped when the cap is reached.
constexpr std::size_t kMaxPendingAborts = 8;

std::wstring JoinPath(std::wstring left, const wchar_t* right) {
    if (!left.empty() && left.back() != L'\\' && left.back() != L'/') left.push_back(L'\\');
    left += right;
    return left;
}

std::wstring SessionsDir(const std::wstring& configDir) {
    if (configDir.empty()) return kSessionsDirName;
    return JoinPath(configDir, kSessionsDirName);
}

std::wstring ComSpec() {
    wchar_t buffer[MAX_PATH * 4]{};
    const DWORD length = GetEnvironmentVariableW(L"ComSpec", buffer,
                                                 (DWORD)(sizeof(buffer) / sizeof(buffer[0])));
    if (length > 0 && length < sizeof(buffer) / sizeof(buffer[0])) {
        return std::wstring(buffer, length);
    }
    return L"cmd.exe";
}

bool FindOnPath(const wchar_t* name, std::wstring& out) {
    wchar_t buffer[32768]{};
    const DWORD length = SearchPathW(nullptr, name, nullptr,
                                     (DWORD)(sizeof(buffer) / sizeof(buffer[0])), buffer,
                                     nullptr);
    if (length == 0 || length >= sizeof(buffer) / sizeof(buffer[0])) return false;
    out.assign(buffer, length);
    return true;
}

int VersionCompare(const std::wstring& a, const std::wstring& b) {
    std::size_t ia = 0, ib = 0;
    while (ia < a.size() || ib < b.size()) {
        int va = 0, vb = 0;
        while (ia < a.size() && a[ia] >= L'0' && a[ia] <= L'9') va = va * 10 + (a[ia++] - L'0');
        while (ib < b.size() && b[ib] >= L'0' && b[ib] <= L'9') vb = vb * 10 + (b[ib++] - L'0');
        if (va != vb) return va < vb ? -1 : 1;
        while (ia < a.size() && a[ia] != L'.') ++ia;
        while (ib < b.size() && b[ib] != L'.') ++ib;
        if (ia < a.size()) ++ia;
        if (ib < b.size()) ++ib;
    }
    return 0;
}

// Highest installed pi release that actually contains the bundled CLI.
// Used only when `pi.cmd` is not on PATH; the version directory name is the
// sort key, which matches the installer layout (releases\<version>\...).
std::wstring FindLatestPiCli() {
    wchar_t overridePath[MAX_PATH * 4]{};
    const DWORD overrideLength =
        GetEnvironmentVariableW(L"PI_CLI_JS", overridePath,
                                (DWORD)(sizeof(overridePath) / sizeof(overridePath[0])));
    if (overrideLength > 0 && overrideLength < sizeof(overridePath) / sizeof(overridePath[0])) {
        return std::wstring(overridePath, overrideLength);
    }

    wchar_t profile[MAX_PATH]{};
    const DWORD profileLength = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (profileLength == 0 || profileLength >= MAX_PATH) return {};

    const fs::path releases = fs::path(std::wstring(profile, profileLength)) / L".pi" /
                              L"agent" / L"install" / L"releases";
    std::error_code error;
    if (!fs::exists(releases, error) || error) return {};

    std::vector<std::pair<std::wstring, fs::path>> candidates;  // version -> cli.js
    for (fs::directory_iterator it(releases, fs::directory_options::skip_permission_denied,
                                   error),
         end;
         !error && it != end; it.increment(error)) {
        if (!it->is_directory(error) || error) continue;
        const fs::path cli = it->path() / L"node_modules" / L"@earendil-works" /
                             L"pi-coding-agent" / L"dist" / L"bundle" / L"cli.js";
        std::error_code existsError;
        if (fs::exists(cli, existsError) && !existsError) {
            candidates.emplace_back(it->path().filename().wstring(), cli);
        }
    }
    if (candidates.empty()) return {};

    std::sort(candidates.begin(), candidates.end(),
              [](const std::pair<std::wstring, fs::path>& a,
                 const std::pair<std::wstring, fs::path>& b) {
                  return VersionCompare(a.first, b.first) > 0;
              });
    return candidates.front().second.wstring();
}

struct Launch {
    std::wstring app;
    std::wstring args;
};

// --- private child environment -------------------------------------------
// The child gets a whole environment block of its own: it is built from a copy
// of this process's block (so PATH, the locale, =C: drive entries and Unicode
// paths survive), credential-shaped parent variables are dropped, and the
// caller's entries are merged case-insensitively on top. The widget process
// itself is never touched (no SetEnvironmentVariableW anywhere), so a key can
// neither leak into the parent environment nor into the child's argv.
constexpr std::size_t kMaxEnvBlockChars = 32767;  // CreateProcessW block limit

// Case folding for names: environment names are compared case-insensitively by
// Windows, so merging and sorting use an uppercased name. Only ASCII is folded
// on purpose: that is the whole practical name space, it is byte-for-byte the
// same on every machine and locale, and a non-ASCII name simply stays exact
// (it still compares, merges and sorts deterministically).
std::wstring FoldEnvName(std::wstring_view name) {
    std::wstring folded(name);
    for (wchar_t& c : folded) {
        if (c >= L'a' && c <= L'z') c = (wchar_t)(c - 32);
    }
    return folded;
}

bool IsCredentialShape(const std::wstring& foldedName) {
    static const wchar_t* const kShapes[] = {L"API_KEY", L"API-KEY", L"APIKEY",
                                             L"TOKEN",   L"SECRET",  L"PASSWORD",
                                             L"PASSWD",  L"CREDENTIAL", L"PRIVATE_KEY"};
    for (const wchar_t* shape : kShapes) {
        if (foldedName.find(shape) != std::wstring::npos) return true;
    }
    return false;
}

struct EnvEntry {
    std::wstring text;    // "NAME=VALUE", or "=C:=C:\\dir" for a special entry
    std::wstring folded;  // case-folded name: merge key and sort key
};

// Copies the current parent block. Read-only: the returned strings are copies
// and FreeEnvironmentStringsW is called before anything else happens.
bool ReadParentEnv(std::vector<EnvEntry>& out) {
    wchar_t* raw = GetEnvironmentStringsW();
    if (!raw) return false;
    for (const wchar_t* cursor = raw; *cursor != L'\0';) {
        const std::wstring entry(cursor);
        cursor += entry.size() + 1;
        if (entry.empty()) break;
        // A special entry ("=C:=C:\\dir") has an empty name; its name is the
        // "=X:" prefix, i.e. everything up to the second '='.
        const std::size_t nameEnd =
            entry[0] == L'=' ? entry.find(L'=', 1) : entry.find(L'=');
        const std::wstring_view name(entry.data(),
                                     nameEnd == std::wstring::npos ? entry.size()
                                                                   : nameEnd);
        EnvEntry item;
        item.text = entry;
        item.folded = FoldEnvName(name);
        out.push_back(std::move(item));
    }
    FreeEnvironmentStringsW(raw);
    return true;
}

bool ValidateEnvEntry(const std::pair<std::wstring, std::wstring>& entry, std::wstring* error) {
    const std::wstring& name = entry.first;
    const std::wstring& value = entry.second;
    if (name.empty()) {
        if (error) *error = L"child env: empty variable name";
        return false;
    }
    if (name[0] == L'=') {
        if (error) *error = L"child env: reserved '=' name";
        return false;
    }
    if (name.find(L'=') != std::wstring::npos) {
        if (error) *error = L"child env: '=' inside a variable name";
        return false;
    }
    if (name.find(L'\0') != std::wstring::npos || value.find(L'\0') != std::wstring::npos) {
        if (error) *error = L"child env: embedded NUL";
        return false;
    }
    return true;
}

// --- diagnostics redaction literals from childEnv -------------------------
std::vector<std::string> EnvCredentialLiterals(
    const std::vector<std::pair<std::wstring, std::wstring>>& childEnv) {
    std::vector<std::string> out;
    for (const std::pair<std::wstring, std::wstring>& entry : childEnv) {
        if (entry.second.empty()) continue;
        if (!IsCredentialShape(FoldEnvName(entry.first))) continue;
        std::string utf8 = ToUtf8(entry.second);
        if (utf8.size() >= kMinDiagLiteralBytes) out.push_back(std::move(utf8));
    }
    return out;
}

// Keeps the private block exactly as long as CreateProcessW may read it, then
// overwrites the credential values it carried.
struct ZeroedEnvBlock {
    std::wstring text;
    ZeroedEnvBlock() = default;
    ZeroedEnvBlock(const ZeroedEnvBlock&) = delete;
    ZeroedEnvBlock& operator=(const ZeroedEnvBlock&) = delete;
    ~ZeroedEnvBlock() {
        if (!text.empty()) SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
    }
    wchar_t* data() { return text.empty() ? nullptr : text.data(); }
};

std::wstring BuildPiArgs(const PiClientOptions& options, const std::wstring& sessionPath) {
    std::vector<std::wstring> args;
    args.push_back(L"--mode");
    args.push_back(L"rpc");
    args.push_back(L"--session-dir");
    args.push_back(SessionsDir(options.configDir));
    if (!sessionPath.empty()) {
        args.push_back(L"--session");
        args.push_back(sessionPath);
    }
    if (!options.personaPath.empty()) {
        args.push_back(L"--append-system-prompt");
        args.push_back(options.personaPath);
    }
    // Caller arguments (provider/model/tests) first; the closed surface below
    // comes last so a late flag always wins over anything supplied here.
    for (const std::wstring& extra : options.extraArgs) args.push_back(extra);

    // Hard M1 boundary: the model gets no tool at all. `options.tools` is
    // deliberately ignored (no --tools is ever written) and the deny-all list
    // is repeated as a belt-and-braces guard for pi versions that re-enable a
    // default set when only one of the two flags is present.
    args.push_back(L"--no-tools");
    args.push_back(L"--exclude-tools");
    args.push_back(L"*");

    // Closed resource surface: no ambient extension, MCP server, skill, prompt
    // template, theme, context file or trust prompt, and no network fetch for
    // catalogues. The one provider the caller installed is loaded explicitly.
    args.push_back(L"--no-extensions");
    if (!options.providerExtension.empty()) {
        args.push_back(L"-e");
        args.push_back(options.providerExtension);
    }
    args.push_back(L"--no-mcp");
    args.push_back(L"--no-skills");
    args.push_back(L"--no-prompt-templates");
    args.push_back(L"--no-themes");
    args.push_back(L"--no-context-files");
    args.push_back(L"--no-approve");
    args.push_back(L"--offline");

    std::wstring line;
    for (const std::wstring& arg : args) {
        if (!line.empty()) line.push_back(L' ');
        line += wg::QuoteArg(arg);
    }
    return line;
}

bool ResolveLaunch(const PiClientOptions& options, const std::wstring& sessionPath, Launch& out,
                   std::wstring& error) {
    const std::wstring args = BuildPiArgs(options, sessionPath);

    if (!options.piCommand.empty()) {
        out.app = options.piCommand;
        out.args = args;
        return true;
    }

    // Preferred: `node <managed release>/dist/bundle/cli.js`. It skips the
    // pi.cmd shim (one cmd.exe launcher process less, no batch quoting) and
    // always runs the CLI the widget manages. The discovery is unchanged: it
    // does not select by version range and installs nothing.
    std::wstring node;
    if (FindOnPath(L"node.exe", node) || FindOnPath(L"node", node)) {
        const std::wstring cli = FindLatestPiCli();
        if (!cli.empty()) {
            out.app = node;
            out.args = wg::QuoteArg(cli) + L" " + args;
            return true;
        }
    }

    // Fallback: the installed `pi.cmd` shim. `cmd /s /c "<quoted>"` is the
    // documented way to pass a quoted batch path through cmd.exe. The shim and
    // everything it spawns inherit the same private environment block.
    std::wstring piCmd;
    if (FindOnPath(L"pi.cmd", piCmd)) {
        out.app = ComSpec();
        out.args = L"/d /s /c \"" + wg::QuoteArg(piCmd) + L" " + args + L"\"";
        return true;
    }

    error = L"pi not found (no node + managed cli.js, no PATH pi.cmd)";
    return false;
}

// Diagnostics (stderr chunks, non-protocol stdout lines) are never shown or
// logged verbatim. Two rules:
//   * control bytes other than tab/CR/LF become spaces;
//   * credential-shaped values are masked with '*'.
// Masking is incremental and stateful (see DiagStream): a credential may be
// arbitrarily long and may be split across two pipe reads, so the scanner never
// flushes a byte that could still be the start of a marker or of a known
// credential literal. Everything it cannot resolve yet stays in a small hold
// buffer bounded by the longest pattern, and tokens it has already recognised
// are masked byte by byte, so memory stays bounded without a length assumption.
std::string SanitizeControls(const char* data, std::size_t size) {
    std::string text(data, size);
    for (char& c : text) {
        const unsigned char u = (unsigned char)c;
        if ((u < 32 || u == 127) && c != '\n' && c != '\t' && c != '\r') c = ' ';
    }
    return text;
}

unsigned char LowerAscii(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

bool MatchNoCase(const std::string& text, std::size_t at, const char* marker) {
    for (std::size_t i = 0; marker[i] != '\0'; ++i) {
        if (at + i >= text.size()) return false;
        if (LowerAscii((unsigned char)text[at + i]) != LowerAscii((unsigned char)marker[i])) {
            return false;
        }
    }
    return true;
}

// True when text[at..] is a proper prefix of `value` (case-insensitive).
bool MatchNoCasePrefix(const std::string& text, std::size_t at, const std::string& value,
                       std::size_t count) {
    if (at + count > text.size() || count >= value.size()) return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (LowerAscii((unsigned char)text[at + i]) != LowerAscii((unsigned char)value[i])) {
            return false;
        }
    }
    return true;
}

bool StartsWithNoCase(const std::string& text, const char* prefix) {
    const std::size_t length = std::strlen(prefix);
    if (text.size() < length) return false;
    for (std::size_t i = 0; i < length; ++i) {
        if (LowerAscii((unsigned char)text[i]) != LowerAscii((unsigned char)prefix[i])) {
            return false;
        }
    }
    return true;
}

// A space/tab/quote/colon/equals is the gap between a credential name and its
// value, not part of the value.
bool IsDiagSeparator(char c) {
    return c == '=' || c == ':' || c == ' ' || c == '\t' || c == '"' || c == '\'';
}

// Ends a credential value. A newline ends one too: diagnostics are line
// oriented, so a token never continues past its line.
bool IsDiagDelimiter(char c) {
    const unsigned char u = (unsigned char)c;
    return u <= 32 || c == '"' || c == '\'' || c == ',' || c == '}' || c == '\\';
}

struct DiagPatterns {
    struct Marker {
        std::string text;
        bool nameValue;  // "name=value" / "name: value" instead of "sk-value"
    };
    std::vector<Marker> markers;
    // Exact credential values lifted from the command line (`--api-key=...`).
    // They have no marker and no `sk-` shape, so only a literal match catches
    // them. Longest first, so the most specific pattern wins at a position.
    std::vector<std::string> literals;

    std::size_t MaxLength() const {
        std::size_t length = 0;
        for (const Marker& marker : markers) length = std::max(length, marker.text.size());
        for (const std::string& literal : literals) length = std::max(length, literal.size());
        return length;
    }
};

std::vector<std::string> CredentialLiterals(const std::vector<std::wstring>& extraArgs) {
    static const char* const kFlags[] = {"--api-key", "--api_key", "--apikey",
                                         "--token",   "--auth-token", "--access-token"};
    std::vector<std::string> out;
    bool takeNext = false;
    for (const std::wstring& wide : extraArgs) {
        const std::string arg = ToUtf8(wide);
        if (takeNext) {
            takeNext = false;
            if (arg.size() >= kMinDiagLiteralBytes) out.push_back(arg);
            continue;
        }
        for (const char* flag : kFlags) {
            if (!StartsWithNoCase(arg, flag)) continue;
            const std::size_t length = std::strlen(flag);
            if (arg.size() == length) {
                takeNext = true;  // "--api-key <value>"
            } else if (arg[length] == '=' || arg[length] == ':') {
                std::string value = arg.substr(length + 1);
                if (value.size() >= kMinDiagLiteralBytes) out.push_back(std::move(value));
            }
            break;
        }
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return a.size() > b.size();
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

DiagPatterns BuildDiagPatterns(const PiClientOptions& options) {
    static const DiagPatterns::Marker kMarkers[] = {
        {"sk-", false},    {"bearer", true},   {"api-key", true}, {"api_key", true},
        {"apikey", true}, {"authorization", true},
    };
    DiagPatterns patterns;
    for (const DiagPatterns::Marker& marker : kMarkers) patterns.markers.push_back(marker);
    // Two literal sources: credential flags the caller still passes on the
    // command line (legacy/tests) and the values it injects through childEnv.
    // The latter never reach argv, but a crashing CLI may still print them.
    patterns.literals = CredentialLiterals(options.extraArgs);
    const std::vector<std::string> fromEnv = EnvCredentialLiterals(options.childEnv);
    patterns.literals.insert(patterns.literals.end(), fromEnv.begin(), fromEnv.end());
    std::sort(patterns.literals.begin(), patterns.literals.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    patterns.literals.erase(std::unique(patterns.literals.begin(), patterns.literals.end()),
                            patterns.literals.end());
    return patterns;
}

enum class DiagMatch { None, Partial, Marker, Literal };

DiagMatch MatchDiagPattern(const DiagPatterns& patterns, const std::string& text,
                           std::size_t at, std::size_t& length, bool& nameValue) {
    length = 0;
    nameValue = false;
    const std::size_t remaining = text.size() - at;
    bool partial = false;
    for (const std::string& literal : patterns.literals) {
        if (remaining >= literal.size() && MatchNoCase(text, at, literal.c_str())) {
            length = literal.size();
            return DiagMatch::Literal;
        }
        if (remaining < literal.size() && MatchNoCasePrefix(text, at, literal, remaining)) {
            partial = true;
        }
    }
    for (const DiagPatterns::Marker& marker : patterns.markers) {
        if (remaining >= marker.text.size() && MatchNoCase(text, at, marker.text.c_str())) {
            length = marker.text.size();
            nameValue = marker.nameValue;
            return DiagMatch::Marker;
        }
        if (remaining < marker.text.size() &&
            MatchNoCasePrefix(text, at, marker.text, remaining)) {
            partial = true;
        }
    }
    return partial ? DiagMatch::Partial : DiagMatch::None;
}

// Per-stream scanner state. stdout and stderr get their own instance: they are
// independent byte streams, so a token half-read on one must never be joined to
// bytes from the other.
struct DiagStream {
    std::string hold;        // masked bytes the scanner has not classified yet
    std::size_t scan = 0;    // next byte in hold to classify
    bool inSecret = false;   // inside a credential value, waiting for a delimiter
    bool maskedAny = false;  // value bytes already masked for the current token
    bool nameValue = false;  // marker was "name=value", so separators are skipped
};

// Advances the scanner over stream.hold, masking in place. Bytes before
// stream.scan are always safe to emit: either confirmed ordinary text or
// already replaced by '*'. With final=false an incomplete marker/literal prefix
// at the end is left in place so the next chunk can complete it; with final=true
// it is masked instead of being trusted.
void ScanDiag(const DiagPatterns& patterns, DiagStream& stream, bool final) {
    const std::string& text = stream.hold;
    while (stream.scan < text.size()) {
        const char c = text[stream.scan];
        if (stream.inSecret) {
            if (!stream.maskedAny && stream.nameValue && IsDiagSeparator(c)) {
                ++stream.scan;
            } else if (IsDiagDelimiter(c)) {
                stream.inSecret = false;
                stream.maskedAny = false;
                stream.nameValue = false;
                ++stream.scan;
            } else {
                stream.hold[stream.scan] = '*';
                stream.maskedAny = true;
                ++stream.scan;
            }
            continue;
        }

        std::size_t length = 0;
        bool nameValue = false;
        const DiagMatch match = MatchDiagPattern(patterns, text, stream.scan, length, nameValue);
        if (match == DiagMatch::Literal) {
            for (std::size_t i = stream.scan; i < stream.scan + length; ++i) {
                stream.hold[i] = '*';
            }
            stream.scan += length;
            // The real token may continue past the known value.
            stream.inSecret = true;
            stream.maskedAny = true;
            stream.nameValue = false;
            continue;
        }
        if (match == DiagMatch::Marker) {
            stream.scan += length;
            stream.inSecret = true;
            stream.maskedAny = false;
            stream.nameValue = nameValue;
            continue;
        }
        if (match == DiagMatch::Partial) {
            if (!final) break;  // wait for the rest of the pattern
            while (stream.scan < stream.hold.size()) stream.hold[stream.scan++] = '*';
            break;
        }
        ++stream.scan;
    }
}

// Redaction of a *user-visible* error text (PiEvent::Error / LastError).
//
// Not every secret-bearing string arrives on stderr: `prompt` rejections,
// extension_error records, message_update errors and auto_retry_end.finalError
// carry free-form text straight from pi, and a failing extension/provider may
// quote the very key the client injected through childEnv. Those strings go to
// the card, to the error event and to LastError, so they must be masked with the
// same known literals and markers as the diagnostic streams.
//
// It is deliberately a *stateless* pass over a private DiagStream: the stdout /
// stderr incremental state is never read, advanced or flushed here. That keeps
// the two concerns independent (an error cannot consume a half-read token from
// another channel, and a masked byte run on one stream cannot influence an error
// message) and makes the call idempotent, so DescribeExit() output - already
// masked from the stream scanner - survives it unchanged. The pass is one linear
// scan with no hold-over buffer and no growth: the input length is bounded by the
// JSONL record cap (kMaxLineBytes) at the protocol layer.
//
// Model text (TextDelta) is never passed through this: masking model output
// would corrupt ordinary answers.
std::wstring RedactErrorText(const DiagPatterns& patterns, const std::wstring& text) {
    if (text.empty()) return text;
    const std::string utf8 = ToUtf8(text);
    DiagStream stream;
    stream.hold = SanitizeControls(utf8.data(), utf8.size());
    ScanDiag(patterns, stream, /*final=*/true);
    const std::string redacted(stream.hold, 0, stream.scan);
    if (stream.scan == 0 || redacted == utf8) return text;  // nothing matched: keep the text
    return ToWide(redacted);
}

bool WriteAll(HANDLE handle, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        DWORD written = 0;
        const DWORD chunk =
            (DWORD)std::min<std::size_t>(data.size() - offset, (std::size_t)64 * 1024);
        if (!WriteFile(handle, data.data() + offset, chunk, &written, nullptr) ||
            written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

} // namespace

// --- test seam: the private child environment block ----------------------
namespace detail {

bool IsCredentialEnvName(std::wstring_view name) { return IsCredentialShape(FoldEnvName(name)); }

bool BuildChildEnvBlock(const std::vector<std::pair<std::wstring, std::wstring>>& overrides,
                        ChildEnvBlock* out, std::wstring* error) {
    if (!out) {
        if (error) *error = L"child env: no output";
        return false;
    }
    // Validate every caller entry first: a bad entry must fail the launch before
    // anything is merged, and the message never echoes the value.
    for (const std::pair<std::wstring, std::wstring>& entry : overrides) {
        if (!ValidateEnvEntry(entry, error)) return false;
    }

    std::vector<EnvEntry> entries;
    if (!ReadParentEnv(entries)) {
        if (error) *error = L"child env: GetEnvironmentStringsW failed";
        return false;
    }

    std::vector<EnvEntry> merged;
    merged.reserve(entries.size() + overrides.size());
    for (EnvEntry& entry : entries) {
        // Credential-shaped parent variables never reach the child. A caller
        // that does need one (CMD_API_KEY) supplies it through childEnv below.
        if (entry.text[0] != L'=' && IsCredentialShape(entry.folded)) continue;
        merged.push_back(std::move(entry));
    }
    for (const std::pair<std::wstring, std::wstring>& entry : overrides) {
        std::wstring line = entry.first;
        line.push_back(L'=');
        line += entry.second;
        const std::wstring folded = FoldEnvName(entry.first);
        bool replaced = false;
        for (EnvEntry& existing : merged) {
            if (existing.folded != folded) continue;
            existing.text = std::move(line);
            replaced = true;
            break;
        }
        if (replaced) continue;
        EnvEntry added;
        added.text = std::move(line);
        added.folded = folded;
        merged.push_back(std::move(added));
    }

    // Windows expects the block sorted case-insensitively; `=C:` entries begin
    // with '=' (0x3D) and therefore stay in front of the named variables.
    std::sort(merged.begin(), merged.end(), [](const EnvEntry& a, const EnvEntry& b) {
        if (a.folded.empty() || b.folded.empty()) return a.folded.size() < b.folded.size();
        if (a.folded == b.folded) return a.text < b.text;
        return CompareStringOrdinal(a.folded.data(), (int)a.folded.size(), b.folded.data(),
                                    (int)b.folded.size(), TRUE) == CSTR_LESS_THAN;
    });

    std::wstring block;
    for (const EnvEntry& entry : merged) {
        block += entry.text;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');  // the block itself is terminated by an extra NUL
    if (block.size() > kMaxEnvBlockChars) {
        SecureZeroMemory(block.data(), block.size() * sizeof(wchar_t));
        if (error) *error = L"child env: block too large";
        return false;
    }

    out->text = std::move(block);
    out->count = merged.size();
    return true;
}

} // namespace detail

// --- Implementation ------------------------------------------------------

struct PiClient::Impl {
    explicit Impl(PiClientOptions opts) : options(std::move(opts)) {
        sessionPath = options.sessionPath;
        patterns = BuildDiagPatterns(options);
    }

    PiClientOptions options;
    DiagPatterns patterns;  // immutable after construction
    std::function<void(const PiEvent&)> handler;

    mutable std::mutex mutex;
    std::condition_variable cv;
    // Lock order (never taken in the other direction):
    //   lifecycle -> writeMutex -> mutex
    // `lifecycle` serializes Start()/Stop(); the supervisor thread never takes
    // it, so Stop() can always join the supervisor. `writeMutex` owns the stdin
    // handle and every write to it. `mutex` guards the run state and is only
    // held for short bookkeeping, never across I/O, joins or handler calls.
    // RequestStop() touches only the atomics and `mutex`, so it may run while
    // another thread is inside Start()/Stop().
    std::mutex lifecycle;
    std::mutex writeMutex;

    HANDLE job = nullptr;
    HANDLE process = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    HANDLE stderrRead = nullptr;

    std::thread reader;
    std::thread stderrThread;
    std::thread supervisor;
    std::thread timer;

    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false};
    std::atomic<long long> nextId{1};
    std::atomic<bool> restartUsed{false};
    // Sticky shutdown request (RequestStop). Never cleared: Start() refuses
    // instead, so an interrupt that lands between Start's flag reset and the
    // child launch cannot be lost.
    std::atomic<bool> shutdownRequested{false};

    std::wstring sessionPath;  // mutex
    std::wstring lastError;    // mutex (ASCII, no payload)
    // mutex: masked tail of both stdout and stderr, for diagnosis.
    std::string stderrTail;
    // mutex: per-stream masking state. stdout and stderr are parsed
    // independently so a token split on one stream cannot pick up bytes from
    // the other. Holding only an unresolved pattern prefix keeps memory bounded
    // even for a credential of unknown length.
    DiagStream diagOut;
    DiagStream diagErr;
    std::string lineBuffer;    // reader thread only

    struct Pending {
        bool done = false;
        bool success = false;
        std::string error;
        JsonValue data;
    };
    std::map<long long, Pending> pending;  // mutex

    // One session-level run at a time. Idle is the only state that accepts
    // Ask(). After a local timeout the run is Unsettled: the terminal event
    // pair has been reported, but pi has not confirmed the run is over, so a
    // late delta/tool/usage/error or the old run's agent_settled must never be
    // charged to the next question. Stop() and a real process restart (new
    // child, so nothing of the old run can arrive) reset the state.
    enum class RunState { Idle, Active, Unsettled };
    RunState runState = RunState::Idle;              // mutex
    long long activePromptId = 0;                    // mutex, prompt of that run
    std::set<long long> pendingAborts;               // mutex, bounded
    std::chrono::steady_clock::time_point requestDeadline;  // mutex
    // mutex: the child that owned the last run is gone and no new one has
    // finished its handshake yet. Ask() is refused while this is set, so a
    // Done/Error callback cannot start a run on a dead child; only a successful
    // handshake clears it (and only then does Running() become true again).
    bool exitPending = false;

    long long lastInputTokens = 0;   // mutex
    long long lastOutputTokens = 0;  // mutex
    bool haveUsage = false;          // mutex

    // --- event plumbing ---------------------------------------------------
    void Emit(const PiEvent& event) {
        try {
            if (handler) handler(event);
        } catch (...) {
            // A throwing handler must not kill the reader thread.
        }
    }

    void EmitErrorKind(PiEvent::Kind kind, std::wstring text) {
        PiEvent event;
        event.kind = kind;
        event.text = std::move(text);
        Emit(event);
    }

    // A failure belongs to the question that is currently in flight. With no
    // local run (idle, or a timed-out run pi has not settled yet) there is no
    // owner: emit nothing and remember nothing, so a stale exit/stream/command
    // error can never surface against the next question. The text is masked with
    // the known literals/markers before it can become an event or LastError:
    // prompt rejections and extension/stream errors are free-form protocol text
    // and may echo the provider key that only exists in childEnv.
    void Fail(std::wstring text) {
        text = RedactErrorText(patterns, text);
        bool active = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            active = runState == RunState::Active;
            if (active) lastError = text;
        }
        if (!active) {
            wg::Log(wg::LogLevel::Info, kLogCategory,
                    L"pi dropped (no active run): " + text);
            return;
        }
        EmitErrorKind(PiEvent::Kind::Error, std::move(text));
    }

    void SetRunning(bool value) { running.store(value); }

    bool Stopping() const { return stopping.load(); }

    // --- run state (all *Locked helpers need `mutex`) ----------------------
    bool IdleLocked() const { return runState == RunState::Idle; }

    void ClearUsageLocked() {
        lastInputTokens = 0;
        lastOutputTokens = 0;
        haveUsage = false;
    }

    // Forgets any run ownership. Used by Stop() and by a real process restart:
    // the child (and with it the run) is gone, so nothing of it can arrive.
    void ResetRunLocked() {
        runState = RunState::Idle;
        activePromptId = 0;
        pendingAborts.clear();
        ClearUsageLocked();
    }

    void AddPendingAbortLocked(long long id) {
        while (pendingAborts.size() >= kMaxPendingAborts) {
            pendingAborts.erase(pendingAborts.begin());
        }
        pendingAborts.insert(id);
    }

    // Which Impl (if any) the current thread is a worker of. Set by WorkerMark
    // at the top of every worker entry point, so "am I on one of my own
    // threads?" never has to walk the std::thread members (the supervisor
    // replaces reader/stderr; reading an id while that happens is a race).
    inline static thread_local const Impl* tlsWorker = nullptr;

    struct WorkerMark {
        explicit WorkerMark(const Impl* owner) { tlsWorker = owner; }
        ~WorkerMark() { tlsWorker = nullptr; }
        WorkerMark(const WorkerMark&) = delete;
        WorkerMark& operator=(const WorkerMark&) = delete;
    };

    bool OnWorkerThread() const { return tlsWorker == this; }

    // One masking/flush path for stderr and non-protocol stdout. Anything
    // written to pi-stderr.log or pasted into an error event comes from here.
    // Must be called with mutex held.
    void EmitDiagnosticLocked(const std::string& bytes) {
        if (bytes.empty()) return;
        stderrTail += bytes;
        if (stderrTail.size() > kMaxDiagTail) {
            stderrTail.erase(0, stderrTail.size() - kMaxDiagTail);
        }
        if (!options.configDir.empty()) {
            std::ofstream out(std::filesystem::path(options.configDir + L"\\pi-stderr.log"),
                              std::ios::binary | std::ios::app);
            if (out) out.write(bytes.data(), (std::streamsize)bytes.size());
        }
    }

    void AppendDiagnostic(DiagStream& stream, const char* data, std::size_t size) {
        std::string chunk = SanitizeControls(data, size);
        std::lock_guard<std::mutex> lock(mutex);
        stream.hold += chunk;
        ScanDiag(patterns, stream, /*final=*/false);
        if (stream.scan == 0) return;
        EmitDiagnosticLocked(std::string(stream.hold, 0, stream.scan));
        stream.hold.erase(0, stream.scan);
        stream.scan = 0;
    }

    // Resolves the unresolved tails (exit reporting). A trailing partial pattern
    // is masked rather than flushed, so a truncated credential cannot escape.
    void FlushDiagnosticStreamsLocked() {
        DiagStream* const streams[] = {&diagOut, &diagErr};
        for (DiagStream* stream : streams) {
            ScanDiag(patterns, *stream, /*final=*/true);
            if (stream->scan > 0) {
                EmitDiagnosticLocked(std::string(stream->hold, 0, stream->scan));
            }
            stream->hold.clear();
            stream->scan = 0;
            stream->inSecret = false;
            stream->maskedAny = false;
            stream->nameValue = false;
        }
    }

    // --- stdin ------------------------------------------------------------
    bool SendRecord(const JsonValue& record, const char* what) {
        const std::string line = record.Dump() + "\n";
        std::lock_guard<std::mutex> lock(writeMutex);
        if (!stdinWrite) {
            std::lock_guard<std::mutex> state(mutex);
            if (lastError.empty()) lastError = L"pi stdin closed";
            return false;
        }
        if (!WriteAll(stdinWrite, line)) {
            std::lock_guard<std::mutex> state(mutex);
            lastError = std::wstring(L"pi stdin write failed (") + ToWide(what) + L")";
            return false;
        }
        return true;
    }

    // Abort carries its own id so its response is attributable: a failure only
    // retires this abort request and is never reported as a command error. On a
    // write failure the id is dropped again, so no pending entry leaks.
    bool SendAbort(long long id) {
        JsonValue command = JsonValue::MakeObject();
        command.Set("type", JsonValue::MakeString("abort"));
        command.Set("id", JsonValue::MakeInt(id));
        if (!SendRecord(command, "abort")) {
            std::lock_guard<std::mutex> lock(mutex);
            pendingAborts.erase(id);
            return false;
        }
        return true;
    }

    // --- responses --------------------------------------------------------
    bool WaitResponse(long long id, int timeoutMs, Pending* out) {
        std::unique_lock<std::mutex> lock(mutex);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 1);
        for (;;) {
            if (stopping.load()) {
                pending.erase(id);  // nobody will read this entry any more
                return false;
            }
            const auto it = pending.find(id);
            if (it != pending.end() && it->second.done) {
                if (out) *out = it->second;
                pending.erase(it);
                return true;
            }
            if (cv.wait_until(lock, deadline) == std::cv_status::timeout) {
                if (stopping.load()) {
                    pending.erase(id);
                    return false;
                }
                const auto late = pending.find(id);
                if (late != pending.end() && late->second.done) {
                    if (out) *out = late->second;
                    pending.erase(late);
                    return true;
                }
                pending.erase(id);
                return false;
            }
        }
    }

    // Emits the terminal pair (optional Usage, then Done). Called without
    // `mutex`; the run state has already been retired by the caller.
    void EmitTerminal(bool aborted, bool usage, long long inputTokens,
                      long long outputTokens) {
        if (usage) {
            PiEvent tokens;
            tokens.kind = PiEvent::Kind::Usage;
            tokens.inputTokens = inputTokens;
            tokens.outputTokens = outputTokens;
            Emit(tokens);
        }
        PiEvent done;
        done.kind = PiEvent::Kind::Done;
        done.aborted = aborted;
        Emit(done);
    }

    // Retires the run this client owns (Active -> Idle) and reports it. Returns
    // false when no local run is active, so a stale or duplicated settlement
    // cannot emit a second Done for a request that is already finished.
    bool Finish(bool aborted) {
        long long inputTokens = 0;
        long long outputTokens = 0;
        bool usage = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (runState != RunState::Active) return false;
            runState = RunState::Idle;
            activePromptId = 0;
            inputTokens = lastInputTokens;
            outputTokens = lastOutputTokens;
            usage = haveUsage;
            ClearUsageLocked();
        }
        cv.notify_all();
        EmitTerminal(aborted, usage, inputTokens, outputTokens);
        return true;
    }

    // Local deadline hit. The upper layer gets exactly one terminal pair, but
    // the run stays Unsettled: pi still owns it, so every further record of that
    // run is dropped and Ask() is refused until pi confirms idle (a successful
    // abort response or the run's own agent_settled).
    void ExpireRun() {
        long long abortId = 0;
        long long inputTokens = 0;
        long long outputTokens = 0;
        bool usage = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (runState != RunState::Active) return;
            runState = RunState::Unsettled;
            activePromptId = 0;
            inputTokens = lastInputTokens;
            outputTokens = lastOutputTokens;
            usage = haveUsage;
            ClearUsageLocked();
            lastError = L"TIMEOUT";
            abortId = nextId.fetch_add(1);
            AddPendingAbortLocked(abortId);
        }
        cv.notify_all();
        // Not through Fail(): the run was just marked Unsettled, and this
        // timeout is exactly the one error the caller must still see.
        EmitErrorKind(PiEvent::Kind::Error, L"TIMEOUT");
        EmitTerminal(/*aborted=*/true, usage, inputTokens, outputTokens);
        SendAbort(abortId);
    }

    // agent_settled carries no request id (pi 1.5 rpc.md/json.md), so it is
    // matched by state instead: it retires the active run, and after a local
    // timeout it is the trustworthy "the old run is over" confirmation. It must
    // never release a *different* request, which is why Ask() only starts from
    // Idle.
    void HandleSettled(bool aborted) {
        bool late = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (runState == RunState::Unsettled) {
                runState = RunState::Idle;
                ClearUsageLocked();
                late = true;
            }
        }
        if (late) cv.notify_all();
        Finish(aborted);
    }

    // abort answers only after the session became idle again (rpc-commands.md
    // "Abort the current operation and wait for the session to become idle
    // before responding"), so a success is a trusted idle confirmation. A
    // failure only retires the abort request: it is never a command error and
    // must not colour the next question.
    void HandleAbortResponse(long long id, bool success) {
        bool confirmed = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (id != 0) {
                const auto it = pendingAborts.find(id);
                if (it == pendingAborts.end()) return;  // not ours / already retired
                pendingAborts.erase(it);
            }
            if (success && runState == RunState::Unsettled) {
                runState = RunState::Idle;
                ClearUsageLocked();
                confirmed = true;
            }
        }
        if (confirmed) cv.notify_all();
    }

    // The child that owned the run is gone. The door is shut *before* anything
    // is published: a re-entrant Ask from a Done/Error callback must not start a
    // run on a dead child, and the run state may not be "Idle" here (that is
    // what made an re-entrant Ask look acceptable). A successful handshake after
    // the restart is the only thing that re-opens it.
    void ReportExit(const std::wstring& text) {
        // Same one-shot redaction as Fail(): the exit description embeds stderr
        // text (already masked) and the base string, and both go straight to an
        // Error event and LastError here instead of through Fail().
        const std::wstring described = RedactErrorText(patterns, DescribeExit(text));
        bool report = false;
        bool usage = false;
        long long inputTokens = 0;
        long long outputTokens = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            report = runState == RunState::Active;
            inputTokens = lastInputTokens;
            outputTokens = lastOutputTokens;
            usage = haveUsage;
            // Not Idle: Ask() has to keep refusing until a real handshake.
            runState = RunState::Unsettled;
            activePromptId = 0;
            pendingAborts.clear();
            ClearUsageLocked();
            exitPending = true;
            if (report) lastError = described;
        }
        cv.notify_all();
        if (!report) {
            wg::Log(wg::LogLevel::Info, kLogCategory,
                    L"pi dropped (no active run): " + described);
            SetRunning(false);
            return;
        }
        EmitErrorKind(PiEvent::Kind::Error, described);
        EmitTerminal(/*aborted=*/true, usage, inputTokens, outputTokens);
        SetRunning(false);
    }

    void HandleResponse(const JsonValue& record) {
        const long long id = record["id"].Integer(0);
        const bool success = record["success"].Bool(false);
        const std::string command = record["command"].String();
        const std::string errorText = record["error"].String();

        // Abort responses are handled first and return early: they never enter
        // the pending map, never raise a command error and never touch the
        // prompt state.
        if (command == "abort") {
            HandleAbortResponse(id, success);
            return;
        }

        if (command == "get_state" && success) {
            const JsonValue& data = record["data"];
            if (data["sessionFile"].IsString()) {
                const std::wstring path = ToWide(data["sessionFile"].String());
                if (!path.empty()) {
                    std::lock_guard<std::mutex> lock(mutex);
                    sessionPath = path;
                }
            }
        }

        if (id != 0) {
            bool resolved = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                const auto it = pending.find(id);
                if (it != pending.end()) {
                    it->second.done = true;
                    it->second.success = success;
                    it->second.error = errorText;
                    if (record.Has("data")) it->second.data = record["data"];
                    resolved = true;
                }
            }
            if (resolved) cv.notify_all();
        } else if (!success) {
            // Malformed command: pi answers a parse response without an id.
            Fail(L"PI COMMAND REJECTED");
        }

        if (command == "prompt") {
            // Only the prompt that owns the current run may change the run
            // state: a response for a retired (timed-out) prompt is stale.
            long long activeId = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (runState == RunState::Active) activeId = activePromptId;
            }
            if (id == 0 || id != activeId) return;
            if (!success) {
                Fail(errorText.empty() ? L"PI PROMPT REJECTED" : ToWide(errorText));
                Finish(true);
                return;
            }
            // "handled" means an extension consumed the prompt and no run
            // starts, so no agent_settled will follow.
            if (record["data"]["disposition"].String() == "handled") Finish(false);
        }
    }

    void HandleMessageUpdate(const JsonValue& record) {
        // Streaming records carry no request id, so ownership is decided by
        // state: only the run a local Ask owns may emit text or usage. Records
        // of a timed-out run (Unsettled) or of nothing at all (Idle) are dropped
        // without a trace, so the next question starts clean.
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (runState != RunState::Active) return;
        }
        const JsonValue& update = record["assistantMessageEvent"];
        const std::string kind = update["type"].String();
        if (kind == "text_delta") {
            const std::wstring delta = ToWide(update["delta"].String());
            if (!delta.empty()) EmitErrorKind(PiEvent::Kind::TextDelta, delta);
        } else if (kind == "error") {
            const std::wstring reason = ToWide(update["error"].String());
            Fail(reason.empty() ? L"PI STREAM ERROR" : reason);
        }

        if (record["usage"].IsObject()) {
            const JsonValue& usage = record["usage"];
            std::lock_guard<std::mutex> lock(mutex);
            if (runState != RunState::Active) return;
            lastInputTokens = usage["input"].Integer(0);
            lastOutputTokens = usage["output"].Integer(0);
            haveUsage = true;
        }
    }

    void HandleLine(const std::string& line) {
        JsonValue record;
        std::string parseError;
        if (!ParseJson(line, &record, &parseError) || !record.IsObject()) {
            // A CLI that dies before RPC (bad key, insufficient balance, trust
            // error) prints its reason as plain stdout/stderr. Keep a short
            // masked tail for the error text instead of only PI PROTOCOL.
            // stdout is a diagnostic channel here too, so it goes through the
            // same masking as stderr (it used to be kept verbatim).
            AppendDiagnostic(diagOut, line.data(), line.size());
            AppendDiagnostic(diagOut, "\n", 1);
            Fail(L"PI PROTOCOL");
            return;
        }

        const std::string type = record["type"].String();
        if (type == "response") {
            HandleResponse(record);
        } else if (type == "message_update") {
            HandleMessageUpdate(record);
        } else if (type == "tool_execution_start") {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (runState != RunState::Active) return;
            }
            PiEvent tool;
            tool.kind = PiEvent::Kind::Tool;
            tool.toolName = ToWide(record["toolName"].String());
            tool.text = tool.toolName;
            Emit(tool);
        } else if (type == "agent_settled") {
            HandleSettled(record["aborted"].Bool(false));
        } else if (type == "extension_error") {
            const std::wstring message = ToWide(record["error"].String());
            Fail(message.empty() ? L"PI EXTENSION ERROR" : message);
        } else if (type == "auto_retry_end" && !record["success"].Bool(true)) {
            const std::wstring message = ToWide(record["finalError"].String());
            Fail(message.empty() ? L"PI RETRY FAILED" : message);
        }
    }

    void PumpLines() {
        for (;;) {
            const std::size_t newline = lineBuffer.find('\n');
            if (newline == std::string::npos) break;
            std::string line = lineBuffer.substr(0, newline);
            lineBuffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF tolerance
            if (line.empty()) continue;
            if (line.size() > kMaxLineBytes) {
                Fail(L"PI PROTOCOL");
                continue;
            }
            HandleLine(line);
        }
        if (lineBuffer.size() > kMaxLineBytes) {
            lineBuffer.clear();
            Fail(L"PI PROTOCOL");
        }
    }

    // --- threads ----------------------------------------------------------
    void ReaderMain() {
        WorkerMark mark(this);
        std::vector<char> buffer(8192);
        for (;;) {
            HANDLE handle = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex);
                handle = stdoutRead;
            }
            if (!handle) return;
            DWORD got = 0;
            if (!ReadFile(handle, buffer.data(), (DWORD)buffer.size(), &got, nullptr) ||
                got == 0) {
                return;
            }
            lineBuffer.append(buffer.data(), got);
            PumpLines();
        }
    }

    void StderrMain() {
        WorkerMark mark(this);
        HANDLE handle = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            handle = stderrRead;
        }
        if (!handle) return;
        char buffer[4096];
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(handle, buffer, (DWORD)sizeof(buffer), &got, nullptr) || got == 0) {
                return;
            }
            // Keep a short, masked tail: a pi exit is otherwise opaque, and
            // the first line of stderr is what tells "unknown tool" apart from
            // a model/trust problem. Never write this to the log verbatim.
            AppendDiagnostic(diagErr, buffer, got);
        }
    }

    void TimerMain() {
        WorkerMark mark(this);
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopping.load()) {
            if (runState != RunState::Active) {
                cv.wait(lock);
                continue;
            }
            if (cv.wait_until(lock, requestDeadline) != std::cv_status::timeout) continue;
            if (runState != RunState::Active || stopping.load()) continue;
            // ExpireRun() emits events and writes to stdin, so it must not run
            // under `mutex` (writeMutex -> mutex is the only allowed order).
            lock.unlock();
            ExpireRun();
            lock.lock();
        }
    }

    std::wstring ExitText(DWORD code) const {
        return std::wstring(L"PI EXIT ") + std::to_wstring((unsigned long long)code);
    }

    std::wstring CrashText(DWORD code) const {
        return std::wstring(L"PI CRASH ") + std::to_wstring((unsigned long long)code);
    }

    // Appends the last stderr lines to an exit/crash error so the card shows
    // the real reason ("unknown tool", "no model", a launcher stack trace).
    // A Node launcher prints its stack first and "Node.js vX" last, so one line
    // is not enough. The same sanitized tail is written next to the config for
    // a bug report.
    std::wstring DescribeExit(const std::wstring& base) {
        std::string tail;
        {
            std::lock_guard<std::mutex> lock(mutex);
            FlushDiagnosticStreamsLocked();
            tail = stderrTail;  // already masked by the scanner
        }
        if (tail.empty()) return base;

        if (!options.configDir.empty()) {
            std::ofstream out(std::filesystem::path(options.configDir + L"\\pi-stderr.log"),
                              std::ios::binary | std::ios::trunc);
            if (out) out.write(tail.data(), (std::streamsize)tail.size());
        }

        std::vector<std::string> lines;
        std::size_t pos = 0;
        while (pos < tail.size() && lines.size() < 40) {
            std::size_t end = tail.find('\n', pos);
            if (end == std::string::npos) end = tail.size();
            std::string line = tail.substr(pos, end - pos);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            std::size_t first = line.find_first_not_of(" \t");
            if (first != std::string::npos) {
                line.erase(0, first);
                if (line.size() > 200) line.resize(200);
                lines.push_back(std::move(line));
            }
            pos = end + 1;
        }
        if (lines.empty()) return base;

        // Keep the last six meaningful lines: the launcher's exception (first
        // line of the trace) plus the Node footer still identify the cause.
        const std::size_t begin = lines.size() > 6 ? lines.size() - 6 : 0;
        std::string joined;
        for (std::size_t i = begin; i < lines.size(); ++i) {
            if (!joined.empty()) joined += " | ";
            joined += lines[i];
        }
        if (joined.size() > 500) joined = joined.substr(0, 500);
        std::wstring wide = ToWide(joined);
        return wide.empty() ? base : base + L": " + wide;
    }

    void SupervisorMain() {
        WorkerMark mark(this);
        for (;;) {
            HANDLE child = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex);
                child = process;
            }
            if (!child || stopping.load()) return;

            const DWORD wait = WaitForSingleObject(child, 200);
            if (stopping.load()) return;
            if (wait != WAIT_OBJECT_0) continue;

            DWORD code = 0;
            GetExitCodeProcess(child, &code);
            if (stopping.load()) return;

            bool restart = false;
            if (options.restartOnCrash && !restartUsed.load()) {
                restart = restartUsed.exchange(true) == false;
            }

            if (!restart) {
                if (!stopping.load()) {
                    // The run died with the child: report it once and forget it,
                    // so nothing waits for a settlement that cannot arrive.
                    ReportExit(ExitText(code));
                }
                return;
            }

            wg::Log(wg::LogLevel::Info, kLogCategory, L"pi exited; restarting once");
            // The in-flight ask is lost with the process: report it before
            // restarting so the UI does not wait forever. This also clears a
            // timed-out run: a new child cannot settle the old one.
            ReportExit(CrashText(code));
            // Kill any survivor first: a wrapper (cmd.exe -> node) may keep the
            // stdout pipe open after the launcher exits, and joining the reader
            // would otherwise block on it.
            TerminateChild();
            if (reader.joinable()) reader.join();
            if (stderrThread.joinable()) stderrThread.join();
            CloseChildHandles(false);

            if (stopping.load()) return;
            if (!StartChild(nullptr)) {
                Fail(L"PI RESTART FAILED");
                SetRunning(false);
                return;
            }
            if (stopping.load()) {
                CloseChildHandles(true);
                return;
            }
            reader = std::thread([this] { ReaderMain(); });
            stderrThread = std::thread([this] { StderrMain(); });
            if (!Handshake(nullptr)) {
                if (stopping.load()) return;  // Stop() owns the cleanup
                Fail(L"PI RESTART HANDSHAKE FAILED");
                // Kill the child first: that closes the pipe ends, so the two
                // readers started above return instead of having their handle
                // closed under an in-flight ReadFile.
                TerminateChild();
                if (reader.joinable()) reader.join();
                if (stderrThread.joinable()) stderrThread.join();
                CloseChildHandles(true);
                SetRunning(false);
                return;
            }
            // Only a completed handshake re-opens the client. Everything before
            // it (launch, readers, get_state) keeps Ask() refused. The failed run
            // belonged to the dead child, so its state is retired here: the
            // process is gone and no settlement of it can ever arrive, hence the
            // fresh child starts from Idle instead of "still settling".
            {
                std::lock_guard<std::mutex> lock(mutex);
                ResetRunLocked();
                exitPending = false;
            }
            SetRunning(true);
        }
    }

    void TerminateChild() {
        HANDLE jobHandle = nullptr;
        HANDLE processHandle = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            jobHandle = job;
            processHandle = process;
        }
        if (jobHandle) {
            TerminateJobObject(jobHandle, 1);
        } else if (processHandle) {
            TerminateProcess(processHandle, 1);
        }
    }

    // Closes the per-process handles. 'terminate' forces the job down first.
    void CloseChildHandles(bool terminate) {
        HANDLE jobHandle = nullptr;
        HANDLE processHandle = nullptr;
        HANDLE stdinHandle = nullptr;
        HANDLE stdoutHandle = nullptr;
        HANDLE stderrHandle = nullptr;
        {
            std::lock_guard<std::mutex> writeLock(writeMutex);
            stdinHandle = stdinWrite;
            stdinWrite = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            jobHandle = job;
            processHandle = process;
            stdoutHandle = stdoutRead;
            stderrHandle = stderrRead;
            job = nullptr;
            process = nullptr;
            stdoutRead = nullptr;
            stderrRead = nullptr;
        }

        if (stdinHandle) CloseHandle(stdinHandle);
        if (terminate) {
            if (jobHandle) {
                TerminateJobObject(jobHandle, 1);
            } else if (processHandle) {
                TerminateProcess(processHandle, 1);
            }
        }
        if (processHandle) CloseHandle(processHandle);
        if (stdoutHandle) CloseHandle(stdoutHandle);
        if (stderrHandle) CloseHandle(stderrHandle);
        if (jobHandle) CloseHandle(jobHandle);  // kills anything the child spawned
    }

    bool StartChild(std::wstring* error) {
        // A stop may have been requested before the launch (RequestStop touches
        // no lock this path holds): never start a child just to kill it.
        if (shutdownRequested.load() || stopping.load()) {
            if (error) *error = L"pi shutdown requested";
            return false;
        }
        Launch launch;
        std::wstring launchError;
        if (!ResolveLaunch(options, sessionPath, launch, launchError)) {
            if (error) *error = launchError;
            return false;
        }

        // Private child environment. Built before any pipe/handle exists so a
        // rejected entry (bad name, embedded NUL, oversized block) costs nothing
        // to unwind. The block stays alive across CreateProcessW and is zeroed
        // by ~ZeroedEnvBlock afterwards; this process's own environment is never
        // modified.
        ZeroedEnvBlock environment;
        {
            detail::ChildEnvBlock built;
            std::wstring buildError;
            if (!detail::BuildChildEnvBlock(options.childEnv, &built, &buildError)) {
                if (error) *error = buildError.empty() ? L"child env failed" : buildError;
                return false;
            }
            environment.text = std::move(built.text);
        }

        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        HANDLE inRead = nullptr, inWrite = nullptr;
        HANDLE outRead = nullptr, outWrite = nullptr;
        HANDLE errRead = nullptr, errWrite = nullptr;
        auto closeAll = [&] {
            if (inRead) CloseHandle(inRead);
            if (inWrite) CloseHandle(inWrite);
            if (outRead) CloseHandle(outRead);
            if (outWrite) CloseHandle(outWrite);
            if (errRead) CloseHandle(errRead);
            if (errWrite) CloseHandle(errWrite);
        };
        auto fail = [&](const wchar_t* what) {
            closeAll();
            if (error) *error = what;
            return false;
        };

        if (!CreatePipe(&inRead, &inWrite, &attributes, 0)) return fail(L"CreatePipe failed");
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
        if (!CreatePipe(&outRead, &outWrite, &attributes, 0)) return fail(L"CreatePipe failed");
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        if (!CreatePipe(&errRead, &errWrite, &attributes, 0)) return fail(L"CreatePipe failed");
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdInput = inRead;
        startup.hStdOutput = outWrite;
        startup.hStdError = errWrite;

        PROCESS_INFORMATION child{};
        std::wstring commandLine = wg::QuoteArg(launch.app) + L" " + launch.args;
        const wchar_t* workDir = options.workDir.empty() ? nullptr : options.workDir.c_str();
        // CreateProcessW must be told explicitly that the block is UTF-16 (a
        // Unicode value such as a Chinese path would otherwise be misread), and
        // the block stays alive across the call because it is read there.
        const BOOL created = CreateProcessW(launch.app.c_str(), commandLine.data(), nullptr,
                                            nullptr, TRUE,
                                            CREATE_NO_WINDOW | CREATE_SUSPENDED |
                                                CREATE_UNICODE_ENVIRONMENT,
                                            environment.data(), workDir, &startup, &child);
        if (!created) {
            const std::wstring text =
                L"CreateProcess failed (" + std::to_wstring(GetLastError()) + L")";
            closeAll();
            if (error) *error = text;
            return false;
        }

        CloseHandle(inRead);
        CloseHandle(outWrite);
        CloseHandle(errWrite);
        inRead = nullptr;
        outWrite = nullptr;
        errWrite = nullptr;

        HANDLE jobHandle = wg::CreateKillJob();
        if (jobHandle) AssignProcessToJobObject(jobHandle, child.hProcess);
        ResumeThread(child.hThread);
        CloseHandle(child.hThread);

        // Checked again after the launch: a stop requested while CreateProcess
        // ran must not leave an unpublished, unkillable child behind.
        if (shutdownRequested.load() || stopping.load()) {
            if (jobHandle) TerminateJobObject(jobHandle, 1);
            TerminateProcess(child.hProcess, 1);
            CloseHandle(child.hProcess);
            if (jobHandle) CloseHandle(jobHandle);
            CloseHandle(inWrite);
            CloseHandle(outRead);
            CloseHandle(errRead);
            if (error) *error = L"pi shutdown requested";
            return false;
        }

        // Publish under writeMutex first: that is the lock every stdin user
        // holds, so no writer can be looking at a replaced handle. Nested
        // writeMutex -> mutex is the only allowed order.
        {
            std::lock_guard<std::mutex> writeLock(writeMutex);
            stdinWrite = inWrite;
            std::lock_guard<std::mutex> lock(mutex);
            job = jobHandle;
            process = child.hProcess;
            stdoutRead = outRead;
            stderrRead = errRead;
            // A half-read JSONL record of the previous child must not be glued
            // to this child's first bytes. Every caller has already joined the
            // old readers, and this child's readers start after StartChild
            // returns, so lineBuffer is unowned here.
            lineBuffer.clear();
        }
        return true;
    }

    bool Handshake(std::wstring* error) {
        JsonValue command = JsonValue::MakeObject();
        command.Set("type", JsonValue::MakeString("get_state"));
        const long long id = nextId.fetch_add(1);
        command.Set("id", JsonValue::MakeInt(id));
        {
            // Register the request before writing: the reader thread may see the
            // response as soon as the bytes leave this thread.
            std::lock_guard<std::mutex> lock(mutex);
            pending[id] = Pending{};
        }
        if (!SendRecord(command, "get_state")) {
            std::lock_guard<std::mutex> lock(mutex);
            pending.erase(id);
            if (error) *error = L"pi stdin closed";
            return false;
        }

        Pending response;
        if (!WaitResponse(id, options.startTimeoutMs, &response)) {
            if (error) *error = L"pi did not answer get_state";
            return false;
        }
        if (!response.success) {
            if (error) *error = L"pi get_state failed";
            return false;
        }
        return true;
    }

    // --- lifecycle --------------------------------------------------------
    // Start() and Stop() are serialized by `lifecycle`; the supervisor never
    // takes it, so Stop() can always join the supervisor. RequestStop() takes
    // no lifecycle lock at all: it is the only entry point that is safe to call
    // while another thread sits inside Start() or Stop().
    bool Start(std::wstring* error) {
        std::lock_guard<std::mutex> lifecycleLock(lifecycle);
        return StartLocked(error);
    }

    bool StartLocked(std::wstring* error) {
        // Assigning to a joinable std::thread calls std::terminate, and a
        // Stop() from a worker thread would self-join. Refuse before touching
        // any thread object.
        if (OnWorkerThread()) {
            if (error) *error = L"pi restart from worker thread";
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (running.load()) return true;
            if (shutdownRequested.load()) {
                if (error) *error = L"pi shutdown requested";
                return false;
            }
        }
        // Reap unconditionally under `lifecycle`: the std::thread members are
        // only touched here, by StopLocked() and by the supervisor, and Stop()
        // joins the supervisor *before* it reads or joins reader/stderr. Probing
        // `joinable()` first would race the supervisor's re-assignment of those
        // members (running is already false during the restart window), so there
        // is no safe cheap test: reaping an empty/clean state is a no-op.
        StopLocked();
        {
            std::lock_guard<std::mutex> lock(mutex);
            // Re-checked after the reap: a RequestStop that landed in between
            // must not be overwritten by the flag reset below.
            if (shutdownRequested.load()) {
                if (error) *error = L"pi shutdown requested";
                return false;
            }
            stopping.store(false);
            restartUsed.store(false);
            ResetRunLocked();
            lineBuffer.clear();
            // Nothing from a previous child may leak into this run: neither the
            // masked diagnosis tail nor the last error (a stale "PI EXIT n"
            // would be reported against a fresh request).
            stderrTail.clear();
            lastError.clear();
            DiagStream* const streams[] = {&diagOut, &diagErr};
            for (DiagStream* stream : streams) {
                stream->hold.clear();
                stream->scan = 0;
                stream->inSecret = false;
                stream->maskedAny = false;
                stream->nameValue = false;
            }
            if (sessionPath.empty()) sessionPath = options.sessionPath;
            if (!options.configDir.empty()) {
                DeleteFileW((options.configDir + L"\\pi-stderr.log").c_str());
            }
        }

        if (!StartChild(error)) return false;
        reader = std::thread([this] { ReaderMain(); });
        stderrThread = std::thread([this] { StderrMain(); });
        supervisor = std::thread([this] { SupervisorMain(); });
        timer = std::thread([this] { TimerMain(); });

        if (!Handshake(error)) {
            StopLocked();
            return false;
        }
        // Running() only becomes true once pi answered get_state: a caller that
        // sees Running() can rely on a live, handshaken child (and Ask() is not
        // gated on a window where the child is still starting).
        {
            std::lock_guard<std::mutex> lock(mutex);
            ResetRunLocked();  // same clean slate as a supervisor restart
            exitPending = false;
        }
        SetRunning(true);
        wg::Log(wg::LogLevel::Info, kLogCategory, L"pi rpc started");
        return true;
    }

    // Non-blocking interrupt. Safe concurrently with Start()/Stop() on another
    // thread: it only sets the sticky flags and wakes waiters, so a Start()
    // blocked in the get_state handshake returns promptly instead of burning
    // startTimeoutMs, and no join is ever duplicated.
    void RequestStop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            shutdownRequested.store(true);
            stopping.store(true);
        }
        cv.notify_all();
    }

    void Stop() {
        // The lock comes first: `OnWorkerThread()` walks the std::thread members,
        // which a concurrent Start() would be replacing.
        std::lock_guard<std::mutex> lifecycleLock(lifecycle);
        if (OnWorkerThread()) {
            // Self-join guard: a handler cannot join the thread it runs on.
            RequestStop();
            return;
        }
        StopLocked();
    }

    void StopLocked() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping.store(true);
            // Silence first: a consumer shedding events during shutdown must
            // not receive a settlement it cannot use any more.
            ResetRunLocked();
            exitPending = false;
        }
        cv.notify_all();

        // Close stdin first: that is pi's documented orderly shutdown. Only if
        // it does not exit in the grace period do we take the job down.
        {
            std::lock_guard<std::mutex> writeLock(writeMutex);
            if (stdinWrite) {
                CloseHandle(stdinWrite);
                stdinWrite = nullptr;
            }
        }
        HANDLE child = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            child = process;
        }
        if (child) {
            if (WaitForSingleObject(child, kStopGraceMs) != WAIT_OBJECT_0) {
                TerminateChild();
                WaitForSingleObject(child, 1000);
            }
        }

        // Join in this order: the supervisor may still be restarting (which
        // replaces the reader/stderr thread objects), so it goes first. It
        // observes `stopping`, so every step it can be inside is bounded by the
        // pipe closing and by WaitResponse bailing out on `stopping`.
        if (supervisor.joinable()) supervisor.join();
        // A restart may have published a fresh child while we were closing this
        // one. Take the job down again *before* joining the readers: that closes
        // the pipe write ends, so a reader blocked in ReadFile returns instead of
        // parking the join (and the handle is never closed under it).
        TerminateChild();
        if (reader.joinable()) reader.join();
        if (stderrThread.joinable()) stderrThread.join();
        if (timer.joinable()) timer.join();
        CloseChildHandles(true);  // no-op on an exited child; kills stragglers
        SetRunning(false);
        {
            std::lock_guard<std::mutex> lock(mutex);
            ResetRunLocked();
            // A wrapper that was told to shut down stays stopped: Start() refuses
            // it, so the sticky request cannot be cleared by a later Start.
            stopping.store(shutdownRequested.load());
        }
        cv.notify_all();
        wg::Log(wg::LogLevel::Info, kLogCategory, L"pi rpc stopped");
    }
};

// --- PiClient forwarding -------------------------------------------------

PiClient::PiClient() : impl_(std::make_unique<Impl>(PiClientOptions{})) {}

PiClient::PiClient(PiClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

PiClient::~PiClient() { Stop(); }

void PiClient::SetEventHandler(std::function<void(const PiEvent&)> handler) {
    impl_->handler = std::move(handler);
}

bool PiClient::Start(std::wstring* error) { return impl_->Start(error); }

void PiClient::Stop() { impl_->Stop(); }

void PiClient::RequestStop() { impl_->RequestStop(); }

bool PiClient::Running() const { return impl_->running.load(); }

long long PiClient::Ask(std::wstring_view prompt, std::wstring* error) {
    Impl& impl = *impl_;
    const std::string text = ToUtf8(prompt);
    if (text.size() > kMaxPromptBytes) {
        if (error) *error = L"prompt too long";
        return 0;
    }

    JsonValue command = JsonValue::MakeObject();
    command.Set("type", JsonValue::MakeString("prompt"));
    command.Set("message", JsonValue::MakeString(text));

    // Acceptance and arming are one critical section: two concurrent Ask calls
    // cannot both observe "idle" and then both arm themselves.
    long long id = 0;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (impl.shutdownRequested.load() || !impl.running.load() || impl.stopping.load()) {
            if (error) *error = L"pi not running";
            return 0;
        }
        if (impl.exitPending) {
            // The previous child died and its replacement is still starting: a
            // run started now would be lost with it again.
            if (error) *error = L"pi restarting";
            return 0;
        }
        if (impl.runState == Impl::RunState::Unsettled) {
            // The previous run timed out and pi has not confirmed it is idle
            // yet. Starting a new prompt now would hand the old run's late
            // events to the new question, so refuse with a retryable error.
            if (error) *error = L"pi still settling previous run";
            return 0;
        }
        if (impl.runState != Impl::RunState::Idle) {
            if (error) *error = L"pi busy";
            return 0;
        }
        id = impl.nextId.fetch_add(1);
        command.Set("id", JsonValue::MakeInt(id));
        impl.runState = Impl::RunState::Active;
        impl.activePromptId = id;
        impl.ClearUsageLocked();
        impl.requestDeadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(
                                   impl.options.requestTimeoutMs > 0
                                       ? impl.options.requestTimeoutMs
                                       : 1);
    }
    impl.cv.notify_all();

    if (!impl.SendRecord(command, "prompt")) {
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            if (impl.runState == Impl::RunState::Active && impl.activePromptId == id) {
                impl.runState = Impl::RunState::Idle;
                impl.activePromptId = 0;
            }
        }
        impl.cv.notify_all();
        if (error) *error = L"pi stdin closed";
        return 0;
    }
    return id;
}

void PiClient::Cancel() {
    Impl& impl = *impl_;
    long long id = 0;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        // Only a run this client owns may be aborted: an idle client must not
        // write an abort, and after a timeout an abort is already pending.
        if (impl.runState != Impl::RunState::Active) return;
        if (impl.shutdownRequested.load() || impl.stopping.load()) return;
        id = impl.nextId.fetch_add(1);
        impl.AddPendingAbortLocked(id);
    }
    // Fire and forget: no WaitResponse, so the UI thread never blocks on pi.
    impl.SendAbort(id);
}

bool PiClient::WaitForIdle(int timeoutMs) {
    Impl& impl = *impl_;
    std::unique_lock<std::mutex> lock(impl.mutex);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 1);
    while (impl.runState != Impl::RunState::Idle) {
        if (impl.cv.wait_until(lock, deadline) == std::cv_status::timeout) break;
    }
    return impl.IdleLocked();
}

std::wstring PiClient::SessionPath() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->sessionPath;
}

void PiClient::SetSessionPath(std::wstring path) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->sessionPath = std::move(path);
    impl_->options.sessionPath = impl_->sessionPath;
}

std::wstring PiClient::LastError() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->lastError;
}

const PiClientOptions& PiClient::options() const { return impl_->options; }

} // namespace assistant
