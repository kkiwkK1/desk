// fake_pi_rpc - a scripted stand-in for `pi --mode rpc` used by tests.
//
// It implements just enough of the documented protocol (docs/rpc.md,
// docs/rpc-commands.md, docs/json.md) to test PiClient without pi, Node or the
// network: JSONL commands on stdin, responses/events on stdout, strict LF
// framing. Behaviour is selected with --fake-mode=<mode>:
//
//   echo    (default) prompt -> response + 2 text deltas + tool + agent_settled
//   hang    prompt -> no response at all (drives the client timeout); an abort
//           is still answered with success (pi is idle again by then)
//   abort   prompt -> response + one delta, then settles on {"type":"abort"}
//   qa      the GUI/script fixture: prompt -> response, three Chinese text
//           deltas (the last one carries `[[src:dsh-session-fixture]]`) and
//           agent_settled; used by tools/desk-assistant-test.ps1 style scripts
//   crash   prompt -> exit(3) immediately (drives the restart-once path)
//   bogus   prompt -> a response with a wrong id, then the correct one
//   garbage prompt -> one non-JSON line before the normal stream
//   crlf    like echo, but every record is terminated with CRLF
//   secret  prompt -> credential-shaped diagnostics on stdout/stderr split
//           across writes (including a marker split across two writes, a
//           token without delimiter, and the `--api-key` value from the
//           command line), then exit(4) (drives the masked-tail path)
//   json-error  prompt -> five *valid* JSON error records whose payload echoes
//           the private CMD_API_KEY (prompt rejection, extension_error,
//           message_update error, auto_retry_end) plus one marker-shaped
//           (`api_key=...`) echo; they become user-visible Error events and
//           LastError, so the client has to redact the text itself, then the
//           prompt is rejected (drives the one-shot error redaction)
//   no-state get_state is never answered (drives the handshake bound)
//   stale   the first prompt is never answered and never settles (the client
//           times out locally); the abort is answered only after the release
//           sentinel appears, and the run's late tail is emitted first
//   stale-fail  like stale, but the abort is answered with success:false at
//           once; the late tail still follows the release sentinel
//   stale-silent like stale, but the abort is never answered at all
//
// `stale*` answer any later prompt normally ("Fresh") so a test can prove that
// nothing of the timed-out run leaked into the next question. The release
// sentinel (`--fake-release=<path>`) makes the ordering deterministic: the test
// decides when the old run's tail arrives instead of racing a Sleep.
// `--fake-log=<path>` appends one line per received command, which lets a test
// check that (for example) an idle Cancel never wrote an abort. Only the command
// *type* is logged (never the prompt text or a payload), so the same file is a
// safe fixture for GUI scripts.
//
// `--fake-mode=`, `--fake-log=` and `--fake-release=` fall back to the
// environment variables FAKE_PI_MODE / FAKE_PI_LOG / FAKE_PI_RELEASE (a command
// line argument always wins): a GUI harness can then select the mode through the
// private child environment without adding arguments. `--fake-dump=<path>`
// writes a fixture with GetCommandLineW() and the values of exactly the variables
// named in `--fake-dump-env=A,B` (missing ones are recorded as `absent	NAME`) -
// it never dumps the whole environment, so a real provider key cannot be copied
// into a fixture by accident.
//
// The second text delta contains a raw U+2028 (bytes E2 80 A8) inside a JSON
// string: a client that (wrongly) splits on Unicode line separators instead of
// LF would corrupt or reject it.
#include "data/assistant/json.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using assistant::JsonValue;

std::string g_mode = "echo";
std::string g_apiKey;  // value of --api-key=<value>, echoed in "secret" mode
// Handshake paths stay wide: the test binary sits under a non-ASCII workspace,
// so a byte-wise narrowing of the path would corrupt it.
std::wstring g_releasePath;  // sentinel the test drops to release a stale run
std::wstring g_logPath;      // optional command log for ordering assertions
std::wstring g_dumpPath;     // optional launch fixture (env + command line)
std::vector<std::wstring> g_dumpEnvNames;  // exactly these variables are dumped
int g_prompts = 0;
bool g_crlf = false;

// Reads one variable without ever printing it. An empty result can mean
// "absent" too, so callers that must distinguish use GetEnvironmentVariableW
// directly (see DumpFixture).
std::wstring EnvValue(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    value.resize(written < needed ? written : 0);
    return value;
}

std::string NarrowAscii(const std::wstring& wide) {
    std::string out;
    out.reserve(wide.size());
    for (const wchar_t c : wide) {
        if (c == 0 || c > 127) continue;  // mode names / variable names are ASCII
        out.push_back((char)c);
    }
    return out;
}

// Launch fixture for the private-environment tests. Deliberately narrow: the
// command line plus the values of the named variables, nothing else.
void DumpFixture() {
    if (g_dumpPath.empty()) return;
    std::string out;
    out += "cmdline\t" + assistant::ToUtf8(GetCommandLineW()) + "\n";
    for (const std::wstring& name : g_dumpEnvNames) {
        const DWORD needed = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
        if (needed == 0) {
            out += "absent\t" + assistant::ToUtf8(name) + "\n";
            continue;
        }
        std::wstring value(needed, L'\0');
        const DWORD written = GetEnvironmentVariableW(name.c_str(), value.data(), needed);
        value.resize(written < needed ? written : 0);
        out += "env\t" + assistant::ToUtf8(name) + "\t" + assistant::ToUtf8(value) + "\n";
    }
    if (FILE* file = _wfopen(g_dumpPath.c_str(), L"wb")) {
        std::fwrite(out.data(), 1, out.size(), file);
        std::fclose(file);
    }
}

void EmitRaw(const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

void EmitErrRaw(const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
}

void EmitLine(const JsonValue& value) {
    std::string line = value.Dump();
    line += g_crlf ? "\r\n" : "\n";
    EmitRaw(line);
}

std::string SessionFileUtf8() {
    wchar_t temp[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"TEMP", temp, MAX_PATH);
    std::wstring path = (length > 0 && length < MAX_PATH) ? std::wstring(temp, length)
                                                          : std::wstring(L".");
    path += L"\\fake-pi-session.jsonl";
    return assistant::ToUtf8(path);
}

JsonValue StatusData() {
    JsonValue data = JsonValue::MakeObject();
    data.Set("disposition", JsonValue::MakeString("started"));
    return data;
}

JsonValue StateData() {
    JsonValue data = JsonValue::MakeObject();
    data.Set("sessionFile", JsonValue::MakeString(SessionFileUtf8()));
    data.Set("sessionId", JsonValue::MakeString("fake-session"));
    data.Set("isStreaming", JsonValue::MakeBool(false));
    data.Set("messageCount", JsonValue::MakeInt(0));
    return data;
}

JsonValue Respond(const JsonValue& request, const char* command, bool success,
                  JsonValue data = JsonValue()) {
    JsonValue response = JsonValue::MakeObject();
    response.Set("type", JsonValue::MakeString("response"));
    response.Set("command", JsonValue::MakeString(command));
    response.Set("success", JsonValue::MakeBool(success));
    if (request.Has("id")) response.Set("id", request["id"]);
    if (!data.IsNull()) response.Set("data", std::move(data));
    return response;
}

JsonValue Usage(long long input = 7, long long output = 3) {
    JsonValue usage = JsonValue::MakeObject();
    usage.Set("input", JsonValue::MakeInt(input));
    usage.Set("output", JsonValue::MakeInt(output));
    usage.Set("cacheRead", JsonValue::MakeInt(0));
    usage.Set("cacheWrite", JsonValue::MakeInt(0));
    usage.Set("totalTokens", JsonValue::MakeInt(input + output));
    return usage;
}

JsonValue TextDelta(const char* utf8, long long input = 7, long long output = 3) {
    JsonValue update = JsonValue::MakeObject();
    update.Set("type", JsonValue::MakeString("text_delta"));
    update.Set("contentIndex", JsonValue::MakeInt(0));
    update.Set("delta", JsonValue::MakeString(utf8));

    JsonValue record = JsonValue::MakeObject();
    record.Set("type", JsonValue::MakeString("message_update"));
    record.Set("usage", Usage(input, output));
    record.Set("assistantMessageEvent", std::move(update));
    return record;
}

JsonValue ToolStart() {
    JsonValue record = JsonValue::MakeObject();
    record.Set("type", JsonValue::MakeString("tool_execution_start"));
    record.Set("toolCallId", JsonValue::MakeString("call_1"));
    record.Set("toolName", JsonValue::MakeString("read"));
    record.Set("args", JsonValue::MakeObject());
    return record;
}

JsonValue Settled(bool aborted) {
    JsonValue record = JsonValue::MakeObject();
    record.Set("type", JsonValue::MakeString("agent_settled"));
    record.Set("aborted", JsonValue::MakeBool(aborted));
    return record;
}

// One line per received command, in arrival order: lets a test assert that a
// command was (or was not) written at a given point of the conversation.
void LogCommand(const std::string& type) {
    if (g_logPath.empty()) return;
    if (FILE* file = _wfopen(g_logPath.c_str(), L"ab")) {
        std::fwrite(type.data(), 1, type.size(), file);
        std::fputc('\n', file);
        std::fclose(file);
    }
}

// Bounded wait on the release sentinel (10s cap, so a forgotten sentinel fails
// a test instead of hanging the suite).
void WaitForRelease() {
    if (g_releasePath.empty()) return;
    for (int waited = 0; waited < 10000; waited += 10) {
        if (GetFileAttributesW(g_releasePath.c_str()) != INVALID_FILE_ATTRIBUTES) return;
        Sleep(10);
    }
}

// Late records of a run the client already timed out locally: text, a tool and
// the run's settlement, with a distinctive usage counter so a leak is
// detectable in the next question.
void EmitStaleTail() {
    EmitLine(TextDelta("STALE", 500, 700));
    EmitLine(ToolStart());
    EmitLine(Settled(true));
}

bool IsStaleMode() {
    return g_mode == "stale" || g_mode == "stale-fail" || g_mode == "stale-silent";
}

void HandlePrompt(const JsonValue& request) {
    ++g_prompts;
    if (g_mode == "crash") {
        std::fflush(stdout);
        ExitProcess(3);
    }
    if (g_mode == "secret") {
        // A CLI that dies before RPC prints its reason on stdout/stderr. Each
        // token is split across two writes so a per-chunk redaction would leak
        // the half after the marker; both credential shapes are covered.
        EmitRaw("auth failed: sk-LIVE-AAAA");
        Sleep(60);
        EmitRaw("BBBB-CCCC\nlauncher: --api-key=Z");
        Sleep(60);
        EmitRaw("ZZZ-SECRET rejected\n");
        std::fflush(stdout);
        // Same shape on stderr, split between the two writes: the chunk that
        // carries only the tail of the token has no marker to match.
        EmitErrRaw("stderr: Bearer tok-EN1");
        Sleep(60);
        EmitErrRaw("EN2-END\n");
        // A marker split across two writes: neither half is maskable alone, so
        // the client has to hold the incomplete prefix back.
        EmitErrRaw("stderr cross: api");
        Sleep(60);
        EmitErrRaw("key=CROSS-TOKEN-9");
        Sleep(60);
        EmitErrRaw("9 end\n");
        // One token with no delimiter and no known length: it must be masked
        // byte by byte instead of being buffered until a delimiter (or dropped).
        EmitErrRaw("stderr long: sk-LONGT");
        Sleep(60);
        EmitErrRaw(std::string(512, 'Q'));
        Sleep(60);
        EmitErrRaw(std::string(512, 'Q'));
        Sleep(60);
        EmitErrRaw("OKEN-END\n");
        // The credential also arrives on the command line: it has no marker and
        // no `sk-` shape, and it is printed in two halves.
        if (!g_apiKey.empty()) {
            EmitErrRaw("cmdline: " + g_apiKey.substr(0, g_apiKey.size() / 2));
            Sleep(60);
            EmitErrRaw(g_apiKey.substr(g_apiKey.size() / 2) + "\n");
        }
        // The M1 path delivers the same key through the private environment
        // only. A crashing CLI may still echo it, so it must be masked as well.
        const std::wstring envKey = EnvValue(L"CMD_API_KEY");
        if (!envKey.empty()) {
            const std::string utf8 = assistant::ToUtf8(envKey);
            EmitErrRaw("envkey: " + utf8.substr(0, utf8.size() / 2));
            Sleep(60);
            EmitErrRaw(utf8.substr(utf8.size() / 2) + "\n");
        }
        Sleep(150);  // let the client drain the pipe before the exit
        ExitProcess(4);
    }
    if (g_mode == "hang") return;  // never answer: the client must time out

    if (g_mode == "qa") {
        // GUI fixture: three Chinese deltas, the citation marker last, then a
        // normal settlement. No tool event: M1 runs zero tools.
        EmitLine(Respond(request, "prompt", true, StatusData()));
        EmitLine(TextDelta("\xE5\xB7\xB2\xE6\x94\xB6\xE5\x88\xB0\xE9\x97\xAE\xE9\xA2\x98\xEF\xBC\x8C"
                           "\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x9F\xA5\xE8\xAF\x81\xE3\x80\x82"));
        EmitLine(TextDelta("\xE6\xA1\x8C\xE9\x9D\xA2\xE7\xBB\x84\xE4\xBB\xB6\xE6\xB5\x8B\xE8\xAF\x95"
                           "\xE5\xAE\x9E\xE4\xBE\x8B\xE8\xBF\x90\xE8\xA1\x8C\xE6\xAD\xA3\xE5\xB8\xB8\xE3\x80\x82"));
        EmitLine(TextDelta("[[src:dsh-session-fixture]]"));
        EmitLine(Settled(false));
        return;
    }

    if (g_mode == "json-error") {
        // Every record below is valid protocol JSON whose *payload* quotes the
        // key that only exists in the private environment: nothing here reaches
        // stderr, so the stream scanner cannot help - the client must redact the
        // error text itself before it becomes an event or LastError.
        const std::wstring envKey = EnvValue(L"CMD_API_KEY");
        const std::string key =
            envKey.empty() ? std::string("FAKE-KEY-PLACEHOLDER") : assistant::ToUtf8(envKey);

        JsonValue extension = JsonValue::MakeObject();
        extension.Set("type", JsonValue::MakeString("extension_error"));
        extension.Set("error", JsonValue::MakeString("extension failed: key=" + key + "-ext"));
        EmitLine(extension);

        JsonValue inner = JsonValue::MakeObject();
        inner.Set("type", JsonValue::MakeString("error"));
        inner.Set("error", JsonValue::MakeString("stream failed: " + key + "-stream"));
        JsonValue message = JsonValue::MakeObject();
        message.Set("type", JsonValue::MakeString("message_update"));
        message.Set("assistantMessageEvent", std::move(inner));
        message.Set("usage", Usage());
        EmitLine(message);

        JsonValue retry = JsonValue::MakeObject();
        retry.Set("type", JsonValue::MakeString("auto_retry_end"));
        retry.Set("success", JsonValue::MakeBool(false));
        retry.Set("finalError", JsonValue::MakeString("retry failed: " + key + "-retry"));
        EmitLine(retry);

        // A marker-shaped echo that is not a known literal: the existing marker
        // rules must catch it on the error path too.
        JsonValue marker = JsonValue::MakeObject();
        marker.Set("type", JsonValue::MakeString("extension_error"));
        marker.Set("error",
                   JsonValue::MakeString("marker echo: api_key=PLAINTEXT-MARKER-7777"));
        EmitLine(marker);

        JsonValue rejected = Respond(request, "prompt", false);
        rejected.Set("error", JsonValue::MakeString("prompt rejected: " + key + "-rejected"));
        EmitLine(rejected);
        return;  // the rejection settles the run, so no agent_settled follows
    }

    if (IsStaleMode()) {
        // Only the first prompt is the run the client will time out on. It is
        // never answered and never settles; its settlement (or the abort
        // answer) is the confirmation the client has to wait for.
        if (g_prompts == 1) return;
        EmitLine(Respond(request, "prompt", true, StatusData()));
        EmitLine(TextDelta("Fresh"));
        EmitLine(Settled(false));
        return;
    }

    if (g_mode == "bogus") {
        // A response for some other request must be ignored by id correlation.
        JsonValue wrong = Respond(request, "prompt", true, StatusData());
        wrong.Set("id", JsonValue::MakeInt(999));
        EmitLine(wrong);
    }
    if (g_mode == "garbage") EmitRaw("this is not json\n");

    EmitLine(Respond(request, "prompt", true, StatusData()));

    if (g_mode == "abort") {
        EmitLine(TextDelta("partial"));
        return;  // the abort command settles this run
    }

    EmitLine(TextDelta("Hello "));
    EmitLine(TextDelta("world\xE2\x80\xA8!"));
    EmitLine(ToolStart());
    EmitLine(Settled(false));
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    // Environment fallbacks first, so a command line argument below wins.
    g_mode = NarrowAscii(EnvValue(L"FAKE_PI_MODE"));
    if (g_mode.empty()) g_mode = "echo";
    g_logPath = EnvValue(L"FAKE_PI_LOG");
    g_releasePath = EnvValue(L"FAKE_PI_RELEASE");
    g_dumpPath = EnvValue(L"FAKE_PI_DUMP");

    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        const std::wstring prefix = L"--fake-mode=";
        if (argument.compare(0, prefix.size(), prefix) == 0) {
            const std::wstring mode = argument.substr(prefix.size());
            g_mode.assign(mode.begin(), mode.end());  // ASCII mode names only
            continue;
        }
        const std::wstring keyPrefix = L"--api-key=";
        if (argument.compare(0, keyPrefix.size(), keyPrefix) == 0) {
            const std::wstring key = argument.substr(keyPrefix.size());
            g_apiKey.assign(key.begin(), key.end());  // ASCII test values only
            continue;
        }
        const std::wstring releasePrefix = L"--fake-release=";
        if (argument.compare(0, releasePrefix.size(), releasePrefix) == 0) {
            g_releasePath = argument.substr(releasePrefix.size());
            continue;
        }
        const std::wstring logPrefix = L"--fake-log=";
        if (argument.compare(0, logPrefix.size(), logPrefix) == 0) {
            g_logPath = argument.substr(logPrefix.size());
            continue;
        }
        const std::wstring dumpPrefix = L"--fake-dump=";
        if (argument.compare(0, dumpPrefix.size(), dumpPrefix) == 0) {
            g_dumpPath = argument.substr(dumpPrefix.size());
            continue;
        }
        const std::wstring dumpEnvPrefix = L"--fake-dump-env=";
        if (argument.compare(0, dumpEnvPrefix.size(), dumpEnvPrefix) == 0) {
            std::wstring list = argument.substr(dumpEnvPrefix.size());
            std::size_t start = 0;
            while (start <= list.size()) {
                const std::size_t comma = list.find(L',', start);
                const std::wstring name = list.substr(
                    start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
                if (!name.empty()) g_dumpEnvNames.push_back(name);
                if (comma == std::wstring::npos) break;
                start = comma + 1;
            }
        }
    }
    g_crlf = g_mode == "crlf";
    DumpFixture();

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        JsonValue request;
        std::string error;
        if (!assistant::ParseJson(line, &request, &error) || !request.IsObject()) continue;

        const std::string type = request["type"].String();
        LogCommand(type);
        if (type == "get_state") {
            if (g_mode == "no-state") continue;  // never answer the handshake
            EmitLine(Respond(request, "get_state", true, StateData()));
        } else if (type == "abort") {
            if (g_mode == "stale-silent") continue;  // never confirm idle
            if (g_mode == "stale") {
                // pi answers abort only once the session is idle again, so the
                // old run's tail goes out first.
                WaitForRelease();
                EmitStaleTail();
                EmitLine(Respond(request, "abort", true));
                continue;
            }
            if (g_mode == "stale-fail") {
                JsonValue failed = Respond(request, "abort", false);
                failed.Set("error", JsonValue::MakeString("synthetic abort failure"));
                EmitLine(failed);
                WaitForRelease();
                EmitStaleTail();
                continue;
            }
            EmitLine(Respond(request, "abort", true));
            if (g_mode == "abort") EmitLine(Settled(true));
        } else if (type == "prompt") {
            HandlePrompt(request);
        }
        // Unknown commands are ignored on purpose: the client must only rely
        // on the documented response/event stream.
    }
    return 0;
}
