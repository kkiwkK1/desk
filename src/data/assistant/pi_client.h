// PiClient - long-lived `pi --mode rpc` subprocess for the assistant (M1.0).
//
// pi is a Node CLI that speaks JSONL on stdin/stdout (docs/rpc.md,
// docs/rpc-commands.md, docs/json.md of pi 1.4): commands come back as
// `response` records correlated by id, model output arrives as session events
// (`message_update` text deltas, tool executions, `agent_settled`).
//
// This wrapper owns the process lifecycle only; the assistant engine builds
// prompts/contexts and renders events. Rules baked in here:
//   * launcher: `node <managed release>/dist/bundle/cli.js` when the installed
//     CLI is found (no extra cmd.exe launcher), else PATH `pi.cmd` through
//     cmd.exe /d /s /c; an explicit piCommand always wins (tests);
//   * the child runs in a private environment block built from a copy of the
//     parent block (see PiClientOptions::childEnv): credential-shaped parent
//     variables are stripped before the caller's entries are applied, and the
//     process environment of this process is never modified;
//   * model tools are hard-off for the M1 child (`--no-tools --exclude-tools *`
//     after every caller argument) and the extension/config surface is closed
//     down explicitly, with the caller's one provider extension passed as `-e`;
//   * anonymous pipes, strict LF framing (a CR before LF is stripped);
//   * stderr is drained separately and never parsed as protocol;
//   * every command has an id; responses are matched by id;
//   * one Ask in flight, 60s deadline; on expiry one terminal event pair is
//     reported and an abort is sent, but the run stays *unsettled* until pi
//     confirms it is over (see Ask below);
//   * session events carry no request id, so events are only accepted while a
//     local Ask owns the run; anything else is dropped, never attributed to
//     the next question;
//   * an unexpected process exit is restarted once;
//   * Start()/Stop() are serialized; RequestStop() is the thread-safe interrupt;
//   * Stop() closes stdin, then kills the whole job object and joins threads.
// Credentials are never read here: the caller supplies ready-made environment
// values (PiClientOptions::childEnv), argv carries none, and nothing secret is
// logged (diagnostics are masked against the supplied values).
//
// Threading contract:
//   * SetEventHandler() must be called before Start().
//   * At most one thread may call Start()/Stop(); they are serialized internally
//     but a second concurrent caller waits, so callers must not rely on that to
//     interrupt an operation in progress.
//   * RequestStop() may be called from any thread at any time (including while
//     Start() or Stop() runs). It only sets flags and wakes waiters: it never
//     joins, never closes a handle and never destroys anything.
//   * Ask()/Cancel()/WaitForIdle()/Running() may be called from any thread.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace assistant {

// One thing the UI needs to know about. Delivered on a worker thread (reader,
// timeout or supervisor), so the handler must be thread-safe and non-blocking.
struct PiEvent {
    enum class Kind { TextDelta, Tool, Usage, Done, Error };

    Kind kind = Kind::TextDelta;
    std::wstring text;        // TextDelta: appended text; Tool: tool name; Error: message
    std::wstring toolName;    // Tool only (same value as text, kept explicit)
    long long inputTokens = 0;   // Usage
    long long outputTokens = 0;  // Usage
    bool aborted = false;     // Done
};

struct PiClientOptions {
    // Config directory (desk.exe config dir): sessions live in
    // <configDir>\pi-sessions. Empty = current directory.
    std::wstring configDir;
    // Child working directory (the folder pi may read with its tools).
    std::wstring workDir;
    // Resume this pi session on start (from SessionPath() of a previous run).
    std::wstring sessionPath;
    // --append-system-prompt persona.md (empty = skip).
    std::wstring personaPath;
    // Legacy tool selection. The M1 child runs with hard zero tools
    // (`--no-tools --exclude-tools *` are always appended after extraArgs), so
    // this field is accepted for compatibility and never passed to pi.
    std::wstring tools = L"read";
    // Explicit launcher override (tests: path to the fake pi executable).
    // Empty = auto-detect: node + the managed dist/bundle/cli.js, then PATH
    // pi.cmd. Either way the child gets the same private environment block and
    // no credential ever appears in its argv.
    std::wstring piCommand;
    // Appended after the standard arguments (tests: --fake-mode=...). The hard
    // zero-tool / closed-resource flags come *after* these, so nothing here can
    // re-open a tool or an ambient extension.
    std::vector<std::wstring> extraArgs;
    // Private child environment: merged case-insensitively into a copy of the
    // parent block (later entries win) after credential-shaped parent variables
    // (API_KEY/TOKEN/SECRET/PASSWORD/CREDENTIAL names) are stripped. It is
    // handed to CreateProcessW only; this process never calls
    // SetEnvironmentVariableW, so no key ends up in the widget's own
    // environment or in argv. Values of credential-shaped names are also added
    // to the diagnostics redaction literals.
    std::vector<std::pair<std::wstring, std::wstring>> childEnv;
    // Absolute path of the installed official provider extension (index.ts).
    // Non-empty = passed as `-e <path>` right after `--no-extensions`, so
    // exactly that one extension is loaded and no ambient extension can join.
    std::wstring providerExtension;
    // Whole-Ask deadline; on expiry the client sends abort and reports Done.
    int requestTimeoutMs = 60000;
    // get_state handshake bound after every (re)start.
    int startTimeoutMs = 20000;
    // Restart pi once after an unexpected exit; false = report and stop.
    bool restartOnCrash = true;
};

class PiClient {
public:
    PiClient();
    explicit PiClient(PiClientOptions options);
    ~PiClient();

    PiClient(const PiClient&) = delete;
    PiClient& operator=(const PiClient&) = delete;

    // Handler is invoked on worker threads. Set it before Start().
    void SetEventHandler(std::function<void(const PiEvent&)> handler);

    // Starts pi and completes the get_state handshake (session path). On
    // failure returns false, stops the child and fills *error. Refuses (false)
    // once RequestStop() was called: the sticky shutdown flag is never cleared
    // by Start, because a stop requested while a Start is in flight must not be
    // lost. Create a new client instance instead.
    bool Start(std::wstring* error = nullptr);
    // Orderly stop: close stdin, wait up to 2s, then terminate the job.
    void Stop();
    // Non-blocking, thread-safe shutdown request. Sets the sticky shutdown and
    // stopping flags and wakes every waiter (including a Start() blocked in the
    // get_state handshake), so a caller can interrupt this client without a
    // second concurrent Stop(). It does not join, reap or destroy anything:
    // the owner still calls Stop() (or destroys the client) afterwards.
    void RequestStop();
    // True only while a handshaken child is up: it turns false as soon as the
    // child is reported gone, and true again only after the restart handshake
    // succeeded (so a run can never be handed to a half-started child).
    bool Running() const;

    // Sends one prompt. Returns the request id, or 0 with *error set when not
    // running, when a previous Ask is still active ("pi busy"), when the child
    // died and its replacement has not completed the handshake ("pi
    // restarting"), or when the previous run timed out and pi has not confirmed
    // it is idle yet ("pi still settling previous run" — retryable, the same
    // question can be submitted again once WaitForIdle() reports idle).
    // Acceptance/rejection and all streaming arrive asynchronously as events.
    long long Ask(std::wstring_view prompt, std::wstring* error = nullptr);
    // Sends {"type":"abort"} for the active run only (Esc in the card): a
    // no-op when nothing is in flight or when an abort is already pending. The
    // abort carries its own id, its response is never reported as an error and
    // Cancel() never waits for it, so it is safe on the UI thread.
    void Cancel();
    // Blocks until no run is in flight (Done reported *and* pi confirmed idle,
    // or the client is idle) or the timeout expires. A timed-out run that pi
    // has not settled yet keeps this false.
    bool WaitForIdle(int timeoutMs);

    // Last session file reported by get_state (empty until the handshake).
    std::wstring SessionPath() const;
    void SetSessionPath(std::wstring path);
    // Last command/transport error text (ASCII, no payload, no secrets).
    std::wstring LastError() const;

    const PiClientOptions& options() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// --- test seam -----------------------------------------------------------
// The exact builder StartChild() uses, exposed so the headless test can assert
// the private block's contents, sorting and double-NUL termination without a
// process launch. Not an application API; behaviour is covered by
// tests/pi_client.cpp.
namespace detail {

struct ChildEnvBlock {
    // UTF-16 block: `NAME=VALUE\0` entries, terminated by an extra `\0`.
    std::wstring text;
    std::size_t count = 0;
};

// Builds the private child environment block from the *current* parent block.
// Credential-shaped parent variables are stripped, `=C:`-style entries are
// kept, then `overrides` are merged case-insensitively (later entries win) and
// the result is sorted. Returns false with *error for an empty name, a name
// containing '=' or a name/value containing an embedded NUL, and when the block
// would exceed the CreateProcessW limit.
bool BuildChildEnvBlock(const std::vector<std::pair<std::wstring, std::wstring>>& overrides,
                        ChildEnvBlock* out, std::wstring* error);

// True when a variable name looks like a credential (API_KEY/APIKEY/API-KEY,
// TOKEN, SECRET, PASSWORD/PASSWD, CREDENTIAL, PRIVATE_KEY; case-insensitive).
bool IsCredentialEnvName(std::wstring_view name);

} // namespace detail

} // namespace assistant
