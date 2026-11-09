// Headless tests for the assistant runtime primitives (M1.0):
//   * JsonValue parse/dump/UTF-8 <-> UTF-16;
//   * PiClient lifecycle, JSONL framing, id correlation, timeout, abort,
//     crash restart and protocol-error handling;
//   * run-state discipline: a locally timed-out run keeps owning the stream
//     (no late delta/tool/usage/error in the next question, no new Ask until
//     pi really settles), an idle Cancel writes no abort, an abort failure is
//     not a command error, and Stop()/RequestStop() are bounded and never race
//     two joins;
//   * the M1 boundary: private Unicode child environment (childEnv), no key in
//     argv, credential-shaped parent variables stripped, PATH and Unicode paths
//     preserved, hard zero-tool/closed-resource flags after the caller
//     arguments, env-key redaction on the stderr stream *and* on user-visible
//     error text from valid protocol records, and the GUI fake contract
//     (env-selected modes, qa fixture, command-type log).
//
// The pi client tests never touch the network or a real pi: they launch
// `fake-pi-rpc.exe` from this executable's directory (built from
// tests/fake_pi_rpc.cpp by build.bat). Every credential-shaped value below is
// synthetic; the fake dumps only the variables the tests name. Exit code 0 =
// all checks passed.
#include "data/assistant/json.h"
#include "data/assistant/pi_client.h"

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
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    if (condition) return;
    std::printf("[pi-test] FAIL %s\n", what);
    ++failures;
}

void CheckText(bool condition, const char* what, const std::wstring& actual) {
    if (condition) return;
    std::printf("[pi-test] FAIL %s (got: %s)\n", what,
                assistant::ToUtf8(actual).c_str());
    ++failures;
}

// Start() failures are the easiest thing to debug with the child's message.
void CheckStart(bool condition, const std::wstring& error, const char* what) {
    if (condition) return;
    std::printf("[pi-test] FAIL %s (start error: %s)\n", what,
                assistant::ToUtf8(error).c_str());
    ++failures;
}

std::wstring ExeDir() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring path(buffer, length);
    const std::size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) path.resize(slash + 1);
    return path;
}

std::wstring FakePiPath() { return ExeDir() + L"fake-pi-rpc.exe"; }

std::wstring TestConfigDir() {
    const std::wstring dir = ExeDir() + L"pi-test-cfg";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool WaitUntil(const std::function<bool()>& predicate, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        if (predicate()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        Sleep(10);
    }
}

long long ElapsedMs(const std::chrono::steady_clock::time_point& since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - since)
        .count();
}

// Scratch files for the fake peer's handshakes (release sentinel, command log).
std::wstring ScratchPath(const wchar_t* name) { return TestConfigDir() + L"\\" + name; }

void DeleteScratch(const std::wstring& path) { DeleteFileW(path.c_str()); }

bool TouchScratch(const std::wstring& path) {
    FILE* file = _wfopen(path.c_str(), L"wb");
    if (!file) return false;
    std::fclose(file);
    return true;
}

std::vector<std::string> ReadScratchLines(const std::wstring& path) {
    std::vector<std::string> lines;
    FILE* file = _wfopen(path.c_str(), L"rb");
    if (!file) return lines;
    std::string text;
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        text.append(buffer, got);
    }
    std::fclose(file);
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
        pos = end + 1;
    }
    return lines;
}

std::size_t CountLine(const std::vector<std::string>& lines, const char* value) {
    std::size_t count = 0;
    for (const std::string& line : lines) {
        if (line == value) ++count;
    }
    return count;
}

// Collects the worker-thread events and lets the test wait on them.
class Collector {
public:
    void Push(const assistant::PiEvent& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(event);
        cv_.notify_all();
    }

    bool WaitForEvent(const std::function<bool(const std::vector<assistant::PiEvent>&)>& predicate,
                      int timeoutMs) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!predicate(events_)) {
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) break;
        }
        return predicate(events_);
    }

    bool WaitForDone(int timeoutMs) {
        return WaitForEvent(
            [](const std::vector<assistant::PiEvent>& events) {
                for (const auto& event : events) {
                    if (event.kind == assistant::PiEvent::Kind::Done) return true;
                }
                return false;
            },
            timeoutMs);
    }

    // A second (or third) run needs a count, not "any Done": the first Done is
    // already in the log when the test starts waiting for the next one.
    bool WaitForDoneCount(std::size_t count, int timeoutMs) {
        return WaitForEvent(
            [count](const std::vector<assistant::PiEvent>& events) {
                std::size_t done = 0;
                for (const auto& event : events) {
                    if (event.kind == assistant::PiEvent::Kind::Done) ++done;
                }
                return done >= count;
            },
            timeoutMs);
    }

    bool LastDoneAborted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        bool aborted = false;
        for (const auto& event : events_) {
            if (event.kind == assistant::PiEvent::Kind::Done) aborted = event.aborted;
        }
        return aborted;
    }

    bool AnyUsageTokens(long long input, long long output) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& event : events_) {
            if (event.kind == assistant::PiEvent::Kind::Usage &&
                event.inputTokens == input && event.outputTokens == output) {
                return true;
            }
        }
        return false;
    }

    std::size_t Count(assistant::PiEvent::Kind kind) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t count = 0;
        for (const auto& event : events_) {
            if (event.kind == kind) ++count;
        }
        return count;
    }

    std::wstring Deltas() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::wstring text;
        for (const auto& event : events_) {
            if (event.kind == assistant::PiEvent::Kind::TextDelta) text += event.text;
        }
        return text;
    }

    std::wstring FirstText(assistant::PiEvent::Kind kind) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& event : events_) {
            if (event.kind == kind) return event.text;
        }
        return {};
    }

    std::wstring FirstToolName() const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& event : events_) {
            if (event.kind == assistant::PiEvent::Kind::Tool) return event.toolName;
        }
        return {};
    }

    bool AnyErrorWithPrefix(const std::wstring& prefix) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& event : events_) {
            if (event.kind == assistant::PiEvent::Kind::Error &&
                event.text.compare(0, prefix.size(), prefix) == 0) {
                return true;
            }
        }
        return false;
    }

    std::size_t IndexOf(assistant::PiEvent::Kind kind) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < events_.size(); ++i) {
            if (events_[i].kind == kind) return i;
        }
        return static_cast<std::size_t>(-1);
    }

    bool FirstDoneAborted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& event : events_) {
            if (event.kind == assistant::PiEvent::Kind::Done) return event.aborted;
        }
        return false;
    }

    bool AnyTextContaining(const std::wstring& needle) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& event : events_) {
            if (event.text.find(needle) != std::wstring::npos) return true;
            if (event.toolName.find(needle) != std::wstring::npos) return true;
        }
        return false;
    }

    assistant::PiEvent FirstOf(assistant::PiEvent::Kind kind) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& event : events_) {
            if (event.kind == kind) return event;
        }
        return {};
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<assistant::PiEvent> events_;
};

assistant::PiClientOptions MakeOptions(const wchar_t* mode, int requestTimeoutMs = 5000,
                                      bool restartOnCrash = true) {
    assistant::PiClientOptions options;
    options.piCommand = FakePiPath();
    options.configDir = TestConfigDir();
    options.workDir = TestConfigDir();
    options.extraArgs = {std::wstring(L"--fake-mode=") + mode};
    options.requestTimeoutMs = requestTimeoutMs;
    options.startTimeoutMs = 5000;
    options.restartOnCrash = restartOnCrash;
    return options;
}

struct Harness {
    Collector events;
    assistant::PiClient client;

    explicit Harness(assistant::PiClientOptions options) : client(std::move(options)) {
        client.SetEventHandler([this](const assistant::PiEvent& event) { events.Push(event); });
    }
};

// --- synthetic environment and launch fixtures ---------------------------
// Every value here is synthetic. The tests never read a real provider key and
// never write one into a fixture; the fake peer dumps only the variables named
// below plus its own GetCommandLineW().
using EnvPairs = std::vector<std::pair<std::wstring, std::wstring>>;

struct ScopedEnv {
    std::wstring name;
    ScopedEnv(std::wstring envName, const wchar_t* value) : name(std::move(envName)) {
        Check(SetEnvironmentVariableW(name.c_str(), value) != FALSE,
              "env: set synthetic parent value");
    }
    ~ScopedEnv() { SetEnvironmentVariableW(name.c_str(), nullptr); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
};

std::wstring ParentEnv(const std::wstring& name) {
    const DWORD needed = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name.c_str(), value.data(), needed);
    value.resize(written < needed ? written : 0);
    return value;
}

bool Contains(const std::wstring& text, const std::wstring& needle) {
    return !needle.empty() && text.find(needle) != std::wstring::npos;
}

// Splits the child's command line the same way the runtime does for the flags
// the tests care about, so `--tools` is never matched inside `--no-tools`.
std::vector<std::wstring> CommandLineTokens(const std::wstring& line) {
    std::vector<std::wstring> tokens;
    std::wstring current;
    bool quoted = false;
    bool have = false;
    for (const wchar_t c : line) {
        if (c == L'"') {
            quoted = !quoted;
            have = true;
            continue;
        }
        if (!quoted && (c == L' ' || c == L'\t')) {
            if (have) {
                tokens.push_back(current);
                current.clear();
                have = false;
            }
            continue;
        }
        current.push_back(c);
        have = true;
    }
    if (have) tokens.push_back(current);
    return tokens;
}

std::size_t TokenIndex(const std::vector<std::wstring>& tokens, const std::wstring& value) {
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == value) return i;
    }
    return static_cast<std::size_t>(-1);
}

bool HasToken(const std::vector<std::wstring>& tokens, const std::wstring& value) {
    return TokenIndex(tokens, value) != static_cast<std::size_t>(-1);
}

bool HasTokenPair(const std::vector<std::wstring>& tokens, const std::wstring& first,
                  const std::wstring& second) {
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == first && tokens[i + 1] == second) return true;
    }
    return false;
}

std::vector<std::wstring> TabFields(const std::wstring& line) {
    std::vector<std::wstring> fields;
    std::size_t start = 0;
    for (;;) {
        const std::size_t tab = line.find(L'\t', start);
        fields.push_back(line.substr(start, tab == std::wstring::npos ? std::wstring::npos
                                                                     : tab - start));
        if (tab == std::wstring::npos) break;
        start = tab + 1;
    }
    return fields;
}

// What the fake peer saw at launch: its command line and exactly the named
// variables (missing ones are recorded as absent, never as an empty value).
struct LaunchDump {
    bool loaded = false;
    std::wstring commandLine;
    std::vector<std::wstring> tokens;
    std::vector<std::pair<std::wstring, std::wstring>> env;
    std::vector<std::wstring> absent;
};

LaunchDump ReadLaunchDump(const std::wstring& path) {
    LaunchDump dump;
    for (int attempt = 0; attempt < 300; ++attempt) {
        const std::vector<std::string> lines = ReadScratchLines(path);
        if (!lines.empty()) {
            for (const std::string& raw : lines) {
                const std::vector<std::wstring> fields = TabFields(assistant::ToWide(raw));
                if (fields.empty()) continue;
                if (fields[0] == L"cmdline" && fields.size() >= 2) {
                    dump.commandLine = fields[1];
                } else if (fields[0] == L"env" && fields.size() >= 3) {
                    dump.env.emplace_back(fields[1], fields[2]);
                } else if (fields[0] == L"absent" && fields.size() >= 2) {
                    dump.absent.push_back(fields[1]);
                }
            }
            dump.tokens = CommandLineTokens(dump.commandLine);
            dump.loaded = !dump.commandLine.empty();
            if (dump.loaded) return dump;
        }
        Sleep(10);
    }
    return dump;
}

std::wstring DumpEnvValue(const LaunchDump& dump, const std::wstring& name) {
    for (const auto& entry : dump.env) {
        if (entry.first == name) return entry.second;
    }
    return {};
}

bool DumpHasEnv(const LaunchDump& dump, const std::wstring& name) {
    for (const auto& entry : dump.env) {
        if (entry.first == name) return true;
    }
    return false;
}

void AppendDumpArgs(assistant::PiClientOptions& options, const std::wstring& path,
                    const std::vector<std::wstring>& names) {
    options.extraArgs.push_back(L"--fake-dump=" + path);
    if (names.empty()) return;
    std::wstring list = L"--fake-dump-env=";
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) list.push_back(L',');
        list += names[i];
    }
    options.extraArgs.push_back(std::move(list));
}

std::size_t CountCjk(const std::wstring& text) {
    std::size_t count = 0;
    for (const wchar_t c : text) {
        if (c >= 0x2E80 && c <= 0x9FFF) ++count;
    }
    return count;
}

// --- JSON ----------------------------------------------------------------

void TestJson() {
    using assistant::JsonValue;

    const char* document =
        R"({"a":1,"b":[true,null,"x\n\u4e2d\ud83d\ude00"],"c":{"d":-2.5e3}})";
    JsonValue parsed;
    std::string error;
    Check(assistant::ParseJson(document, &parsed, &error), "json: parse object");
    Check(parsed["a"].Integer(-1) == 1, "json: integer");
    Check(parsed["b"].IsArray() && parsed["b"].Size() == 3, "json: array size");
    Check(parsed["b"][1].IsNull(), "json: null item");
    CheckText(assistant::ToWide(parsed["b"][2].String()) == L"x\n\u4e2d\U0001F600",
              "json: escapes and surrogate pair", assistant::ToWide(parsed["b"][2].String()));
    Check(parsed["c"]["d"].Number() == -2500.0, "json: number with exponent");
    Check(parsed["missing"].IsNull(), "json: missing key is null");

    JsonValue command = JsonValue::MakeObject();
    command.Set("type", JsonValue::MakeString("prompt"));
    command.Set("message", JsonValue::MakeString(assistant::ToUtf8(L"\u4f60\u597d")));
    const std::string dump = command.Dump();
    Check(dump == "{\"type\":\"prompt\",\"message\":\"\xE4\xBD\xA0\xE5\xA5\xBD\"}",
          "json: compact deterministic dump");
    JsonValue roundTrip;
    Check(assistant::ParseJson(dump, &roundTrip, &error), "json: reparse dump");
    Check(roundTrip["message"].String() == command["message"].String(),
          "json: utf-8 survives round trip");

    Check(JsonValue::MakeNumber(0.1).Dump() == "0.1", "json: shortest double form");
    Check(JsonValue::MakeInt(42).Dump() == "42", "json: integer dump");
    Check(JsonValue::MakeBool(true).Dump() == "true", "json: bool dump");
    Check(JsonValue::MakeArray().Dump() == "[]", "json: empty array dump");
    Check(JsonValue::MakeObject().Dump() == "{}", "json: empty object dump");
    Check(JsonValue::MakeNull().Dump() == "null", "json: null dump");

    JsonValue escaped = JsonValue::MakeString("\"\\\n\t");
    JsonValue escapedBack;
    Check(assistant::ParseJson(escaped.Dump(), &escapedBack, &error),
          "json: escape round trip parses");
    Check(escapedBack.String() == "\"\\\n\t", "json: escape round trip value");

    JsonValue ignored;
    Check(!assistant::ParseJson("{", &ignored, &error), "json: truncated object rejected");
    Check(!assistant::ParseJson("{\"a\":}", &ignored, &error), "json: missing value rejected");
    Check(!assistant::ParseJson("1 2", &ignored, &error), "json: trailing data rejected");
    Check(!assistant::ParseJson("\"a\nb\"", &ignored, &error),
          "json: raw control character rejected");
    Check(!assistant::ParseJson(std::string(80, '[') + std::string(80, ']'), &ignored, &error),
          "json: excessive nesting rejected");

    Check(assistant::ToWide("").empty() && assistant::ToUtf8(L"").empty(),
          "utf: empty strings");
    Check(assistant::ToWide(assistant::ToUtf8(L"\u4e2d\U0001F600")) == L"\u4e2d\U0001F600",
          "utf: wide round trip");
    Check(assistant::ToUtf8(L"\U0001F600").size() == 4, "utf: emoji is four bytes");
}

// --- PiClient ------------------------------------------------------------

void TestEcho() {
    Harness harness(MakeOptions(L"echo"));
    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "echo: start");
    Check(harness.client.Running(), "echo: running after start");
    CheckText(!harness.client.SessionPath().empty() &&
                  harness.client.SessionPath().find(L"fake-pi-session") != std::wstring::npos,
              "echo: session path from get_state", harness.client.SessionPath());
    Check(harness.client.LastError().empty(), "echo: no error during start");

    const long long id = harness.client.Ask(L"hello", &error);
    Check(id > 0, "echo: ask accepted");
    Check(harness.events.WaitForDone(5000), "echo: settled");
    CheckText(harness.events.Deltas() == L"Hello world\u2028!",
              "echo: LF framing keeps U+2028 inside a record", harness.events.Deltas());
    Check(harness.events.Count(assistant::PiEvent::Kind::Done) == 1,
          "echo: exactly one done");
    Check(!harness.events.FirstDoneAborted(), "echo: done is not aborted");
    Check(harness.events.Count(assistant::PiEvent::Kind::Error) == 0, "echo: no error event");
    Check(harness.events.Count(assistant::PiEvent::Kind::Tool) == 1, "echo: one tool event");
    Check(harness.events.FirstToolName() == L"read", "echo: tool name");
    const assistant::PiEvent usage = harness.events.FirstOf(assistant::PiEvent::Kind::Usage);
    Check(usage.inputTokens == 7 && usage.outputTokens == 3, "echo: usage tokens");
    Check(harness.events.IndexOf(assistant::PiEvent::Kind::Usage) <
              harness.events.IndexOf(assistant::PiEvent::Kind::Done),
          "echo: usage precedes done");
    Check(harness.client.WaitForIdle(500), "echo: idle after done");

    harness.client.Stop();
    Check(!harness.client.Running(), "echo: stopped");
}

void TestBogusId() {
    Harness harness(MakeOptions(L"bogus"));
    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "bogus: start");
    Check(harness.client.Ask(L"hi", &error) > 0, "bogus: ask accepted");
    Check(harness.events.WaitForDone(5000), "bogus: settled");
    Check(harness.events.Count(assistant::PiEvent::Kind::Error) == 0,
          "bogus: response with wrong id ignored");
    CheckText(harness.events.Deltas() == L"Hello world\u2028!", "bogus: stream intact",
              harness.events.Deltas());
    harness.client.Stop();
}

void TestGarbageLine() {
    Harness harness(MakeOptions(L"garbage"));
    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "garbage: start");
    Check(harness.client.Ask(L"hi", &error) > 0, "garbage: ask accepted");
    Check(harness.events.WaitForDone(5000), "garbage: settled despite bad line");
    Check(harness.events.Count(assistant::PiEvent::Kind::Error) == 1,
          "garbage: exactly one protocol error");
    Check(harness.events.FirstText(assistant::PiEvent::Kind::Error) == L"PI PROTOCOL",
          "garbage: protocol error text");
    CheckText(harness.events.Deltas() == L"Hello world\u2028!", "garbage: stream intact",
              harness.events.Deltas());
    harness.client.Stop();
}

void TestCrlfFraming() {
    Harness harness(MakeOptions(L"crlf"));
    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "crlf: start");
    Check(harness.client.Ask(L"hi", &error) > 0, "crlf: ask accepted");
    Check(harness.events.WaitForDone(5000), "crlf: settled");
    CheckText(harness.events.Deltas() == L"Hello world\u2028!", "crlf: CR stripped",
              harness.events.Deltas());
    harness.client.Stop();
}

void TestTimeout() {
    Harness harness(MakeOptions(L"hang", 300));
    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "timeout: start");
    Check(harness.client.Ask(L"hi", &error) > 0, "timeout: ask accepted");
    Check(harness.events.WaitForDone(3000), "timeout: reports done");
    Check(harness.events.AnyErrorWithPrefix(L"TIMEOUT"), "timeout: error text");
    Check(harness.events.FirstDoneAborted(), "timeout: done is aborted");
    Check(harness.client.Running(), "timeout: process still running");
    harness.client.Stop();
}

void TestAbort() {
    Harness harness(MakeOptions(L"abort"));
    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "abort: start");
    Check(harness.client.Ask(L"hi", &error) > 0, "abort: ask accepted");
    Check(harness.events.WaitForEvent(
              [](const std::vector<assistant::PiEvent>& events) {
                  for (const auto& event : events) {
                      if (event.kind == assistant::PiEvent::Kind::TextDelta) return true;
                  }
                  return false;
              },
              5000),
          "abort: partial delta arrives");
    harness.client.Cancel();
    Check(harness.events.WaitForDone(3000), "abort: done after cancel");
    Check(harness.events.FirstDoneAborted(), "abort: done is aborted");
    Check(harness.events.Count(assistant::PiEvent::Kind::Error) == 0, "abort: no error event");
    harness.client.Stop();
}

void TestCrashRestart() {
    Harness harness(MakeOptions(L"crash"));
    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "crash: start");
    Check(harness.client.Ask(L"first", &error) > 0, "crash: first ask accepted");
    Check(harness.events.WaitForDone(5000), "crash: lost ask settled");
    Check(harness.events.AnyErrorWithPrefix(L"PI CRASH 3"), "crash: crash reported");
    Check(harness.events.FirstDoneAborted(), "crash: lost ask marked aborted");

    // The supervisor restarts pi once; wait until Ask is possible again, then
    // make the restarted process crash too so the client must give up.
    long long second = 0;
    Check(WaitUntil(
              [&] {
                  second = harness.client.Ask(L"second", &error);
                  return second > 0;
              },
              5000),
          "crash: restarted client accepts a second ask");
    Check(harness.events.WaitForEvent(
              [](const std::vector<assistant::PiEvent>& events) {
                  for (const auto& event : events) {
                      if (event.kind == assistant::PiEvent::Kind::Error) return true;
                  }
                  return false;
              },
              5000),
          "crash: restart exhaustion reported");
    Check(WaitUntil([&] { return !harness.client.Running(); }, 5000),
          "crash: client stops after the second crash");
    Check(harness.events.AnyErrorWithPrefix(L"PI EXIT 3"), "crash: final exit reported");
    harness.client.Stop();
}

void TestStopAndRestart() {
    Harness harness(MakeOptions(L"echo"));
    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "restart: first start");
    harness.client.Stop();
    Check(!harness.client.Running(), "restart: stopped");
    CheckStart(harness.client.Start(&error), error, "restart: second start");
    Check(harness.client.Ask(L"hi", &error) > 0, "restart: ask after restart");
    Check(harness.events.WaitForDone(5000), "restart: settled after restart");
    harness.client.Stop();
}

void TestSessionOverride() {
    Harness harness(MakeOptions(L"echo"));
    harness.client.SetSessionPath(L"C:\\fake\\resume.jsonl");
    Check(harness.client.SessionPath() == L"C:\\fake\\resume.jsonl",
          "session: set before start");
    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "session: start");
    Check(!harness.client.SessionPath().empty(), "session: refreshed by get_state");
    harness.client.Stop();
}

// Diagnostics must never carry credential material, on stdout or stderr, on
// an error event, in LastError() or in pi-stderr.log. The fake splits each
// token across writes, splits a marker across writes, emits one token with no
// delimiter at all (unknown length, so no "hold back N bytes" rule can work)
// and prints the `--api-key` value from the command line in two halves; that
// value has neither a marker nor an `sk-` shape.
void TestDiagnosticsMasking() {
    assistant::PiClientOptions options = MakeOptions(L"secret", 5000, /*restartOnCrash=*/false);
    options.extraArgs.push_back(L"--api-key=test-literal-KEY-9182");
    Harness harness(std::move(options));

    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "secret: start");
    Check(harness.client.Ask(L"hi", &error) > 0, "secret: ask accepted");
    // Non-protocol stdout already raises PI PROTOCOL, so waiting for "any
    // error" would race the exit report. Wait for this exact exit instead.
    Check(harness.events.WaitForEvent(
              [](const std::vector<assistant::PiEvent>& events) {
                  for (const auto& event : events) {
                      if (event.kind == assistant::PiEvent::Kind::Error &&
                          event.text.compare(0, 9, L"PI EXIT 4") == 0) {
                          return true;
                      }
                  }
                  return false;
              },
              5000),
          "secret: exit reported");
    Check(harness.events.AnyErrorWithPrefix(L"PI EXIT 4"), "secret: exit code reported");
    // DescribeExit() writes the log before Fail() emits the event, so an
    // arrived event means the log is on disk; still wait for the stop to make
    // the "one exit only" statement deterministic.
    Check(WaitUntil([&] { return !harness.client.Running(); }, 5000),
          "secret: client stops after the exit");
    CheckText(!harness.events.AnyTextContaining(L"AAAA") &&
                  !harness.events.AnyTextContaining(L"BBBB-CCCC") &&
                  !harness.events.AnyTextContaining(L"ZZZ-SECRET") &&
                  !harness.events.AnyTextContaining(L"EN2-END") &&
                  !harness.events.AnyTextContaining(L"CROSS-TOKEN") &&
                  !harness.events.AnyTextContaining(L"LONGT") &&
                  !harness.events.AnyTextContaining(L"QQQQ") &&
                  !harness.events.AnyTextContaining(L"test-literal") &&
                  !harness.events.AnyTextContaining(L"KEY-9182"),
              "secret: credential halves never reach an event", L"leaked");
    CheckText(harness.client.LastError().find(L"BBBB") == std::wstring::npos &&
                  harness.client.LastError().find(L"ZZZ-SECRET") == std::wstring::npos &&
                  harness.client.LastError().find(L"EN2-END") == std::wstring::npos &&
                  harness.client.LastError().find(L"CROSS-TOKEN") == std::wstring::npos &&
                  harness.client.LastError().find(L"QQQQ") == std::wstring::npos &&
                  harness.client.LastError().find(L"KEY-9182") == std::wstring::npos,
              "secret: LastError is masked", harness.client.LastError());

    // The bug-report log goes through the same masking.
    const std::wstring logPath = TestConfigDir() + L"\\pi-stderr.log";
    std::string log;
    for (int attempt = 0; attempt < 50 && log.empty(); ++attempt) {
        if (FILE* file = _wfopen(logPath.c_str(), L"rb")) {
            char buffer[4096];
            std::size_t got = 0;
            while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
                log.append(buffer, got);
            }
            std::fclose(file);
        }
        if (log.empty()) Sleep(10);
    }
    Check(!log.empty(), "secret: diagnosis log written");
    Check(log.find("AAAA") == std::string::npos && log.find("BBBB") == std::string::npos &&
              log.find("ZZZ-SECRET") == std::string::npos &&
              log.find("EN2-END") == std::string::npos &&
              log.find("CROSS-TOKEN") == std::string::npos &&
              log.find("LONGT") == std::string::npos && log.find("QQQQ") == std::string::npos &&
              log.find("test-literal") == std::string::npos &&
              log.find("KEY-9182") == std::string::npos,
          "secret: pi-stderr.log is masked");
    // ...and the long token must not have silenced the stream: the masked text
    // before and after it still reaches the log (bounded memory, not amnesia).
    Check(log.find("stderr cross") != std::string::npos &&
              log.find("stderr long") != std::string::npos,
          "secret: masked stream keeps flowing past long tokens");
    harness.client.Stop();
}

// After the supervisor gives up (crash restart exhausted) running==false but the
// reader/stderr/supervisor std::thread objects are still joinable. Start() must
// reap them instead of assigning over them (which is std::terminate).
void TestStartAfterCrashExhaustion() {
    Harness harness(MakeOptions(L"crash"));
    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "reap: first start");
    Check(harness.client.Ask(L"first", &error) > 0, "reap: first ask accepted");
    long long second = 0;
    Check(WaitUntil(
              [&] {
                  second = harness.client.Ask(L"second", &error);
                  return second > 0;
              },
              5000),
          "reap: restarted client accepts a second ask");
    Check(WaitUntil([&] { return !harness.client.Running(); }, 5000),
          "reap: client gives up after the second crash");

    // Deliberately no Stop(): start again on top of the leftover threads.
    CheckStart(harness.client.Start(&error), error, "reap: start after crash exhaustion");
    Check(harness.client.Running(), "reap: running after restart");
    harness.client.Stop();
    Check(!harness.client.Running(), "reap: stopped");
}

// After a local timeout the terminal pair is reported, but pi still owns the
// run: a new Ask must be refused until pi really settles (the run's
// agent_settled, or an abort answered after the session went idle). Otherwise
// the old run's late delta/tool/usage/settle lands in the next answer.
// `abortFails` selects stale-fail, where the abort response is success:false:
// that must not be treated as an idle confirmation and must not raise an error.
void TestTimeoutKeepsRunUnsettled(const wchar_t* mode, bool abortFails) {
    const std::wstring release = ScratchPath(L"stale-release.txt");
    const std::wstring log = ScratchPath(L"stale-commands.txt");
    DeleteScratch(release);
    DeleteScratch(log);

    assistant::PiClientOptions options = MakeOptions(mode, 300);
    options.extraArgs.push_back(L"--fake-release=" + release);
    options.extraArgs.push_back(L"--fake-log=" + log);
    Harness harness(std::move(options));

    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "unsettled: start");
    Check(harness.client.Ask(L"first", &error) > 0, "unsettled: first ask accepted");
    Check(harness.events.WaitForDone(3000), "unsettled: local timeout reported");
    Check(harness.events.AnyErrorWithPrefix(L"TIMEOUT"), "unsettled: timeout error text");
    Check(harness.events.FirstDoneAborted(), "unsettled: timeout done is aborted");

    // Immediate re-send: refused with the retryable message, not accepted.
    std::wstring refusal;
    Check(harness.client.Ask(L"second", &refusal) == 0, "unsettled: re-ask refused");
    CheckText(refusal == L"pi still settling previous run",
              "unsettled: refusal explains the unsettled run", refusal);
    Check(!harness.client.WaitForIdle(200), "unsettled: not idle before pi settles");
    Check(harness.events.Count(assistant::PiEvent::Kind::TextDelta) == 0,
          "unsettled: no delta in the empty window");

    if (abortFails) {
        // The failure response is emitted before the stale tail, so once the
        // tail has been processed that response has been processed too.
        Check(WaitUntil([&] { return CountLine(ReadScratchLines(log), "abort") == 1; }, 3000),
              "unsettled: the timeout wrote exactly one abort");
    }

    // Before the release the client must still refuse: an abort response was
    // already rejected (stale-fail) or is still being awaited (stale).
    std::wstring stillRefused;
    Check(harness.client.Ask(L"third", &stillRefused) == 0,
          "unsettled: re-ask still refused before the old run is over");
    CheckText(stillRefused == L"pi still settling previous run",
              "unsettled: refusal is stable while unsettled", stillRefused);

    Check(TouchScratch(release), "unsettled: release sentinel written");
    long long second = 0;
    Check(WaitUntil([&] { second = harness.client.Ask(L"second", &error); return second > 0; },
                    5000),
          "unsettled: ask accepted after the old run settled");
    Check(harness.events.WaitForDoneCount(2, 5000), "unsettled: second run settles");

    // Nothing of the timed-out run may show up in the second answer: not its
    // text, not its tool, not its usage counters, and not a second error.
    CheckText(harness.events.Deltas() == L"Fresh", "unsettled: no stale delta in the next answer",
              harness.events.Deltas());
    Check(harness.events.Count(assistant::PiEvent::Kind::Tool) == 0,
          "unsettled: no stale tool event");
    Check(!harness.events.AnyUsageTokens(500, 700), "unsettled: no stale usage");
    Check(harness.events.Count(assistant::PiEvent::Kind::Error) == 1,
          "unsettled: only the timeout is reported");
    Check(!harness.events.LastDoneAborted(), "unsettled: second done is not aborted");
    Check(harness.client.WaitForIdle(1000), "unsettled: idle after the second run");
    harness.client.Stop();
}

// A run that never settles must not block shutdown, and Stop() must clear the
// unsettled state: afterwards the client reports "not running", not "still
// settling". Cancel() during that window must not write a second abort.
void TestStopClearsUnsettledRun() {
    const std::wstring log = ScratchPath(L"unsettled-commands.txt");
    DeleteScratch(log);

    assistant::PiClientOptions options = MakeOptions(L"stale-silent", 250);
    options.extraArgs.push_back(L"--fake-log=" + log);
    Harness harness(std::move(options));

    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "unsettled stop: start");
    Check(harness.client.Ask(L"first", &error) > 0, "unsettled stop: ask accepted");
    Check(harness.events.WaitForDone(3000), "unsettled stop: timeout reported");
    std::wstring refusal;
    Check(harness.client.Ask(L"second", &refusal) == 0, "unsettled stop: re-ask refused");
    Check(!harness.client.WaitForIdle(300), "unsettled stop: not idle");

    Check(WaitUntil([&] { return CountLine(ReadScratchLines(log), "abort") == 1; }, 3000),
          "unsettled stop: the timeout wrote one abort");
    bool stillOne = true;
    for (int i = 0; i < 20 && stillOne; ++i) {
        harness.client.Cancel();  // already aborting: must be a no-op
        Sleep(10);
        stillOne = CountLine(ReadScratchLines(log), "abort") == 1;
    }
    Check(stillOne, "unsettled stop: repeated cancel writes no extra abort");

    const auto stopStart = std::chrono::steady_clock::now();
    harness.client.Stop();
    const long long elapsed = ElapsedMs(stopStart);
    if (elapsed >= 5000) std::printf("[pi-test] unsettled Stop took %lld ms\n", elapsed);
    Check(elapsed < 5000, "unsettled stop: Stop is bounded");
    Check(!harness.client.Running(), "unsettled stop: stopped");
    std::wstring afterStop;
    Check(harness.client.Ask(L"after stop", &afterStop) == 0,
          "unsettled stop: ask refused after stop");
    CheckText(afterStop == L"pi not running", "unsettled stop: stop cleared the run state",
              afterStop);
}

// Esc in an idle card must not write an abort: pi answers it only after the
// session is idle anyway, and the write would be attributed to the next run.
// The fake logs every command, so a write can be detected by ordering alone.
void TestIdleCancelWritesNoAbort() {
    const std::wstring log = ScratchPath(L"idle-cancel-commands.txt");
    DeleteScratch(log);

    assistant::PiClientOptions options = MakeOptions(L"echo");
    options.extraArgs.push_back(L"--fake-log=" + log);
    Harness harness(std::move(options));

    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "idle cancel: start");
    Check(harness.client.Ask(L"first", &error) > 0, "idle cancel: first ask");
    Check(harness.events.WaitForDone(5000), "idle cancel: first run settles");
    Check(harness.client.WaitForIdle(1000), "idle cancel: idle after the first run");

    harness.client.Cancel();  // nothing in flight
    Check(harness.client.Ask(L"second", &error) > 0, "idle cancel: second ask");
    Check(harness.events.WaitForDoneCount(2, 5000), "idle cancel: second run settles");

    const std::vector<std::string> lines = ReadScratchLines(log);
    Check(CountLine(lines, "prompt") == 2, "idle cancel: both prompts logged");
    Check(CountLine(lines, "abort") == 0, "idle cancel: no abort command was written");
    harness.client.Stop();
}

// Stop() has to stay bounded while the supervisor is restarting the crashed
// child (reader/stderr threads are being replaced in that window).
void TestStopIsBoundedDuringRestart() {
    for (int attempt = 0; attempt < 3; ++attempt) {
        Harness harness(MakeOptions(L"crash"));
        std::wstring error;
        CheckStart(harness.client.Start(&error), error, "restart stop: start");
        Check(harness.client.Ask(L"question", &error) > 0, "restart stop: ask accepted");
        // The crash is reported before the supervisor restarts, so Stop() lands
        // inside the restart window.
        Check(harness.events.WaitForEvent(
                  [](const std::vector<assistant::PiEvent>& events) {
                      for (const auto& event : events) {
                          if (event.kind == assistant::PiEvent::Kind::Error &&
                              event.text.compare(0, 10, L"PI CRASH 3") == 0) {
                              return true;
                          }
                      }
                      return false;
                  },
                  5000),
              "restart stop: crash reported");
        const auto stopStart = std::chrono::steady_clock::now();
        harness.client.Stop();
        const long long elapsed = ElapsedMs(stopStart);
        if (elapsed >= 5000) {
            std::printf("[pi-test] restart Stop attempt %d took %lld ms\n", attempt, elapsed);
        }
        Check(elapsed < 5000, "restart stop: Stop is bounded");
        Check(!harness.client.Running(), "restart stop: not running after stop");
    }
}

// RequestStop() is the only control call that is safe while another thread is
// inside Start(): it has to interrupt the get_state wait without a second
// concurrent Stop(), and it is sticky.
void TestRequestStopInterruptsHandshake() {
    const std::wstring log = ScratchPath(L"handshake-commands.txt");
    DeleteScratch(log);

    assistant::PiClientOptions options = MakeOptions(L"no-state", 60000);
    options.startTimeoutMs = 5000;  // the interrupt must be far faster than this
    options.extraArgs.push_back(L"--fake-log=" + log);
    Harness harness(std::move(options));

    std::atomic<bool> finished{false};
    bool started = true;
    std::wstring startError;
    const auto start = std::chrono::steady_clock::now();
    std::thread starter([&] {
        started = harness.client.Start(&startError);
        finished.store(true);
    });
    Check(WaitUntil([&] { return CountLine(ReadScratchLines(log), "get_state") == 1; }, 5000),
          "request stop: get_state reached pi (Start is inside the handshake)");
    harness.client.RequestStop();
    Check(WaitUntil([&] { return finished.load(); }, 3000),
          "request stop: Start returns promptly instead of waiting the deadline");
    starter.join();
    const long long elapsed = ElapsedMs(start);
    Check(elapsed < 3000, "request stop: handshake interrupted, not timed out");
    Check(!started, "request stop: Start fails after the interrupt");
    CheckText(startError == L"pi did not answer get_state",
              "request stop: failure names the interrupted handshake", startError);
    Check(WaitUntil([&] { return !harness.client.Running(); }, 3000),
          "request stop: not running after the failed start");

    std::wstring again;
    Check(!harness.client.Start(&again), "request stop: sticky start refusal");
    CheckText(again == L"pi shutdown requested", "request stop: refusal is explicit", again);
    const auto stopStart = std::chrono::steady_clock::now();
    harness.client.Stop();
    Check(ElapsedMs(stopStart) < 5000, "request stop: a later Stop stays bounded");
    Check(!harness.client.Running(), "request stop: still stopped");
    std::wstring askError;
    Check(harness.client.Ask(L"q", &askError) == 0, "request stop: ask refused");
}

// M1 boundary: the provider key and the private agent dir travel in a private
// environment block, never in argv; credential-shaped parent variables are
// stripped, PATH and Unicode values survive, and Start()/Stop() never touch the
// widget's own environment (the old `--api-key` argv path is only kept as a
// redaction-control fixture in TestDiagnosticsMasking; the engine must not use
// it).
void TestPrivateChildEnvironment() {
    const std::wstring dumpPath = ScratchPath(L"env-dump.txt");
    DeleteScratch(dumpPath);
    const std::wstring agentDir = TestConfigDir() + L"\\pi-agent";
    const std::wstring unicodeDir = L"\u4e2d\u6587\u8def\u5f84\\\u5b50\u76ee\u5f55";
    const std::wstring key = L"synthetic-env-key-9182";

    ScopedEnv parentKey(L"PI_TEST_PARENT_API_KEY", L"parent-canary-should-not-leak");
    ScopedEnv parentToken(L"PI_TEST_PARENT_TOKEN", L"parent-token-canary");
    ScopedEnv parentUnicode(L"PI_TEST_PARENT_UNICODE", unicodeDir.c_str());
    ScopedEnv parentOverride(L"PI_TEST_OVERRIDE", L"parent-value");
    const std::wstring parentCmdKeyBefore = ParentEnv(L"CMD_API_KEY");

    assistant::PiClientOptions options = MakeOptions(L"echo");
    options.childEnv = {
        {L"CMD_API_KEY", key},
        {L"PI_CODING_AGENT_DIR", agentDir},
        {L"PI_TEST_OVERRIDE", L"child-value"},
    };
    AppendDumpArgs(options, dumpPath,
                   {L"CMD_API_KEY", L"PI_CODING_AGENT_DIR", L"PI_TEST_PARENT_API_KEY",
                    L"PI_TEST_PARENT_TOKEN", L"PI_TEST_PARENT_UNICODE", L"PI_TEST_OVERRIDE",
                    L"PATH"});
    Harness harness(std::move(options));

    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "env: start");
    const LaunchDump dump = ReadLaunchDump(dumpPath);
    Check(dump.loaded, "env: fake wrote its launch fixture");

    // The key is in the child environment and nowhere in the command line.
    CheckText(DumpEnvValue(dump, L"CMD_API_KEY") == key, "env: key reaches the child",
              DumpEnvValue(dump, L"CMD_API_KEY"));
    Check(!Contains(dump.commandLine, key), "env: key never appears in argv");
    Check(!HasToken(dump.tokens, L"--api-key"), "env: no --api-key argument");
    Check(!HasToken(dump.tokens, L"--tools"), "env: no --tools argument");
    CheckText(DumpEnvValue(dump, L"PI_CODING_AGENT_DIR") == agentDir,
              "env: private agent dir reaches the child",
              DumpEnvValue(dump, L"PI_CODING_AGENT_DIR"));

    // Hard zero-tool boundary, after every caller argument.
    Check(HasToken(dump.tokens, L"--no-tools"), "env: --no-tools present");
    Check(HasTokenPair(dump.tokens, L"--exclude-tools", L"*"),
          "env: --exclude-tools * present");
    const std::size_t noTools = TokenIndex(dump.tokens, L"--no-tools");
    const std::size_t extraArg = TokenIndex(dump.tokens, L"--fake-mode=echo");
    Check(noTools != static_cast<std::size_t>(-1) &&
              extraArg != static_cast<std::size_t>(-1) && noTools > extraArg,
          "env: hard flags come after the caller arguments");
    Check(!dump.tokens.empty() && dump.tokens.back() == L"--offline",
          "env: closed surface ends the command line");

    // Closed resource surface.
    Check(HasToken(dump.tokens, L"--no-extensions"), "env: --no-extensions present");
    Check(HasToken(dump.tokens, L"--no-mcp"), "env: --no-mcp present");
    Check(HasToken(dump.tokens, L"--no-skills"), "env: --no-skills present");
    Check(HasToken(dump.tokens, L"--no-prompt-templates"),
          "env: --no-prompt-templates present");
    Check(HasToken(dump.tokens, L"--no-themes"), "env: --no-themes present");
    Check(HasToken(dump.tokens, L"--no-context-files"), "env: --no-context-files present");
    Check(HasToken(dump.tokens, L"--no-approve"), "env: --no-approve present");
    Check(!HasToken(dump.tokens, L"-e"), "env: no extension without providerExtension");

    // Parent credential shapes are stripped, ordinary values are inherited.
    Check(!DumpHasEnv(dump, L"PI_TEST_PARENT_API_KEY"), "env: parent *_API_KEY stripped");
    Check(!DumpHasEnv(dump, L"PI_TEST_PARENT_TOKEN"), "env: parent *TOKEN stripped");
    Check(std::find(dump.absent.begin(), dump.absent.end(), L"PI_TEST_PARENT_API_KEY") !=
              dump.absent.end(),
          "env: stripped key is absent in the child, not empty");
    Check(!Contains(dump.commandLine, L"parent-canary-should-not-leak"),
          "env: stripped value never reaches argv");
    Check(!DumpEnvValue(dump, L"PATH").empty(), "env: PATH inherited");
    CheckText(DumpEnvValue(dump, L"PI_TEST_PARENT_UNICODE") == unicodeDir,
              "env: parent Unicode value survives the block copy",
              DumpEnvValue(dump, L"PI_TEST_PARENT_UNICODE"));
    CheckText(DumpEnvValue(dump, L"PI_TEST_OVERRIDE") == L"child-value",
              "env: childEnv overrides a parent value",
              DumpEnvValue(dump, L"PI_TEST_OVERRIDE"));

    // The widget process environment is untouched by the launch.
    CheckText(ParentEnv(L"PI_TEST_PARENT_API_KEY") == L"parent-canary-should-not-leak",
              "env: parent canary still set while the child runs",
              ParentEnv(L"PI_TEST_PARENT_API_KEY"));
    Check(ParentEnv(L"CMD_API_KEY") == parentCmdKeyBefore,
          "env: the client never sets its own environment");

    harness.client.Stop();
    CheckText(ParentEnv(L"PI_TEST_PARENT_API_KEY") == L"parent-canary-should-not-leak",
              "env: parent canary still set after Stop", ParentEnv(L"PI_TEST_PARENT_API_KEY"));
    Check(ParentEnv(L"CMD_API_KEY") == parentCmdKeyBefore,
          "env: no key in the parent environment after Stop");
    DeleteScratch(dumpPath);
}

// Only the explicitly named provider extension may be loaded; without it the
// command line carries no `-e` at all (so --no-extensions really means none).
void TestProviderExtensionFlag() {
    const std::wstring provider = TestConfigDir() + L"\\fake-provider\\index.ts";
    const std::wstring dumpPath = ScratchPath(L"env-dump-provider.txt");
    DeleteScratch(dumpPath);

    assistant::PiClientOptions options = MakeOptions(L"echo");
    options.providerExtension = provider;
    AppendDumpArgs(options, dumpPath, {});
    Harness harness(std::move(options));
    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "provider: start");
    const LaunchDump dump = ReadLaunchDump(dumpPath);
    Check(dump.loaded, "provider: fake wrote its launch fixture");
    Check(HasToken(dump.tokens, L"--no-extensions"), "provider: ambient extensions disabled");
    Check(HasTokenPair(dump.tokens, L"-e", provider),
          "provider: the installed provider is loaded explicitly");
    Check(!Contains(dump.commandLine, L"--api-key"), "provider: still no argv key");
    harness.client.Stop();
    DeleteScratch(dumpPath);
}

// The block itself: double-NUL termination, sorted case-insensitive names, one
// entry per name, =C: entries kept, credentials stripped, bad input rejected.
void TestChildEnvBlockShape() {
    ScopedEnv parentKey(L"PI_TEST_BLOCK_API_KEY", L"block-canary");
    ScopedEnv parentSecret(L"PI_TEST_BLOCK_SECRET", L"block-secret-canary");
    ScopedEnv parentUnicode(L"PI_TEST_BLOCK_UNICODE", L"\u503c\\\u8def\u5f84 \u4e2d\u6587");
    ScopedEnv parentOverride(L"PI_TEST_OVERRIDE", L"parent-value");

    std::vector<std::pair<std::wstring, std::wstring>> overrides = {
        {L"CMD_API_KEY", L"block-key-9182"},
        {L"pi_test_override", L"child-value"},  // case-insensitive replacement
        {L"PI_CODING_AGENT_DIR", L"C:\\agent dir"},
    };
    assistant::detail::ChildEnvBlock block;
    std::wstring error;
    Check(assistant::detail::BuildChildEnvBlock(overrides, &block, &error),
          "block: built from the parent block");
    Check(block.text.size() > 2, "block: non-trivial");
    Check(block.text.size() >= 2 && block.text[block.text.size() - 1] == L'\0' &&
              block.text[block.text.size() - 2] == L'\0',
          "block: double-NUL terminated");

    std::vector<std::wstring> entries;
    for (std::size_t pos = 0; pos < block.text.size();) {
        const std::size_t end = block.text.find(L'\0', pos);
        if (end == std::wstring::npos) break;
        if (end > pos) entries.push_back(block.text.substr(pos, end - pos));
        pos = end + 1;
    }
    Check(entries.size() == block.count, "block: reported count matches its entries");

    auto nameOf = [](const std::wstring& entry) {
        const std::size_t end = entry[0] == L'=' ? entry.find(L'=', 1) : entry.find(L'=');
        return end == std::wstring::npos ? entry : entry.substr(0, end);
    };
    bool sorted = true;
    bool unique = true;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i > 0 && CompareStringOrdinal(nameOf(entries[i - 1]).c_str(), -1,
                                          nameOf(entries[i]).c_str(), -1, TRUE) ==
                         CSTR_GREATER_THAN) {
            sorted = false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (CompareStringOrdinal(nameOf(entries[j]).c_str(), -1, nameOf(entries[i]).c_str(),
                                     -1, TRUE) == CSTR_EQUAL) {
                unique = false;
            }
        }
    }
    Check(sorted, "block: names sorted case-insensitively");
    Check(unique, "block: one entry per name (override replaced, not appended)");

    std::size_t overrideHits = 0;
    bool parentUnicodeKept = false;
    bool credentialKept = false;
    std::size_t specials = 0;
    for (const std::wstring& entry : entries) {
        if (entry[0] == L'=') ++specials;
        // Windows environment names are case-insensitive, so the parent's
        // PI_TEST_OVERRIDE is replaced in place; the contract is that the entry
        // keeps the caller's spelling, which is why the block carries the
        // lowercase name supplied through `overrides` above.
        if (entry == L"pi_test_override=child-value") ++overrideHits;
        if (entry == L"PI_TEST_BLOCK_UNICODE=\u503c\\\u8def\u5f84 \u4e2d\u6587") {
            parentUnicodeKept = true;
        }
        if (nameOf(entry) == L"PI_TEST_BLOCK_API_KEY" || nameOf(entry) == L"PI_TEST_BLOCK_SECRET") {
            credentialKept = true;
        }
    }
    Check(overrideHits == 1,
          "block: caller override applied exactly once, keeping caller name spelling");
    Check(parentUnicodeKept, "block: Unicode parent value copied verbatim");
    Check(!credentialKept, "block: credential-shaped parent names stripped");
    Check(Contains(block.text, L"CMD_API_KEY=block-key-9182"),
          "block: caller credential injected");

    // `=C:`-style entries of the parent block must survive.
    std::size_t parentSpecials = 0;
    if (wchar_t* raw = GetEnvironmentStringsW()) {
        for (const wchar_t* cursor = raw; *cursor != L'\0';) {
            const std::wstring entry(cursor);
            cursor += entry.size() + 1;
            if (!entry.empty() && entry[0] == L'=') ++parentSpecials;
        }
        FreeEnvironmentStringsW(raw);
    }
    Check(specials == parentSpecials, "block: =C: entries preserved");

    // Invalid input fails the launch instead of producing a broken block.
    assistant::detail::ChildEnvBlock rejected;
    std::wstring rejectError;
    Check(!assistant::detail::BuildChildEnvBlock(EnvPairs{{L"", L"value"}}, &rejected,
                                                &rejectError),
          "block: empty name rejected");
    Check(!assistant::detail::BuildChildEnvBlock(EnvPairs{{L"A=B", L"value"}}, &rejected,
                                                &rejectError),
          "block: '=' inside a name rejected");
    Check(!assistant::detail::BuildChildEnvBlock(EnvPairs{{L"=C:", L"value"}}, &rejected,
                                                &rejectError),
          "block: reserved '=' name rejected");
    std::wstring embedded = L"va";
    embedded.push_back(L'\0');
    embedded += L"lue";
    Check(!assistant::detail::BuildChildEnvBlock(EnvPairs{{L"PI_TEST_NUL", embedded}}, &rejected,
                                                &rejectError),
          "block: embedded NUL in a value rejected");

    Check(assistant::detail::IsCredentialEnvName(L"cmd_api_key") &&
              assistant::detail::IsCredentialEnvName(L"GITHUB_TOKEN") &&
              assistant::detail::IsCredentialEnvName(L"client-secret") &&
              assistant::detail::IsCredentialEnvName(L"DB_PASSWORD") &&
              assistant::detail::IsCredentialEnvName(L"AZURE_CREDENTIAL") &&
              !assistant::detail::IsCredentialEnvName(L"PATH") &&
              !assistant::detail::IsCredentialEnvName(L"KEYBOARD_LAYOUT"),
          "block: credential name shapes");
}

// The M1 key arrives through childEnv only, but a crashing CLI can still print
// it: its value must join the diagnostics literals like the old argv literal.
void TestEnvSecretMasking() {
    const std::wstring key = L"ENVKEY-AAABBB-CCCDDD";
    assistant::PiClientOptions options = MakeOptions(L"secret", 5000, /*restartOnCrash=*/false);
    options.childEnv = {{L"CMD_API_KEY", key}};
    Harness harness(std::move(options));

    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "env secret: start");
    Check(harness.client.Ask(L"hi", &error) > 0, "env secret: ask accepted");
    Check(harness.events.WaitForEvent(
              [](const std::vector<assistant::PiEvent>& events) {
                  for (const auto& event : events) {
                      if (event.kind == assistant::PiEvent::Kind::Error &&
                          event.text.compare(0, 9, L"PI EXIT 4") == 0) {
                          return true;
                      }
                  }
                  return false;
              },
              5000),
          "env secret: exit reported");
    Check(WaitUntil([&] { return !harness.client.Running(); }, 5000),
          "env secret: client stops after the exit");

    CheckText(!harness.events.AnyTextContaining(L"ENVKEY") &&
                  !harness.events.AnyTextContaining(L"AAABBB") &&
                  !harness.events.AnyTextContaining(L"CCCDDD"),
              "env secret: no half of the environment key reaches an event", L"leaked");
    CheckText(harness.client.LastError().find(L"ENVKEY") == std::wstring::npos &&
                  harness.client.LastError().find(L"CCCDDD") == std::wstring::npos,
              "env secret: LastError masked", harness.client.LastError());

    const std::wstring logPath = TestConfigDir() + L"\\pi-stderr.log";
    std::string log;
    for (int attempt = 0; attempt < 50 && log.empty(); ++attempt) {
        if (FILE* file = _wfopen(logPath.c_str(), L"rb")) {
            char buffer[4096];
            std::size_t got = 0;
            while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) log.append(buffer, got);
            std::fclose(file);
        }
        if (log.empty()) Sleep(10);
    }
    Check(!log.empty(), "env secret: diagnosis log written");
    Check(log.find("ENVKEY") == std::string::npos && log.find("AAABBB") == std::string::npos &&
              log.find("CCCDDD") == std::string::npos,
          "env secret: pi-stderr.log masked");
    Check(log.find("envkey:") != std::string::npos,
          "env secret: the masked channel still carries its label");
    harness.client.Stop();
}

// A credential can also leave through *valid* protocol records: prompt
// rejections, extension errors, stream ("message_update") errors and retry
// failures carry free-form text that a failing provider/extension may fill with
// the key it read from the environment. Those strings become user-visible Error
// events and LastError without ever touching stderr, so the client has to redact
// the error text itself - statelessly, with the same literals and markers, and
// without consuming the stderr/stdout scanner state.
void TestValidJsonErrorRedaction() {
    const std::wstring key = L"JSONKEY-AAABBB-CCCDDD";
    assistant::PiClientOptions options = MakeOptions(L"json-error", 5000,
                                                     /*restartOnCrash=*/false);
    options.childEnv = {{L"CMD_API_KEY", key}};
    Harness harness(std::move(options));

    std::wstring error;
    CheckStart(harness.client.Start(&error), error, "json error: start");
    Check(harness.client.Ask(L"hi", &error) > 0, "json error: ask accepted");
    Check(harness.events.WaitForDoneCount(1, 5000), "json error: the run is reported");
    Check(harness.events.Count(assistant::PiEvent::Kind::Error) == 5,
          "json error: every protocol error record became an event");
    Check(harness.events.FirstDoneAborted(), "json error: a rejected prompt is aborted");
    Check(harness.client.WaitForIdle(2000), "json error: idle after the rejection");

    // Every protocol path surfaced (so a passing "no key" check is not vacuous)
    // and none of them carries the key, one of its halves, or the marker value.
    Check(harness.events.AnyTextContaining(L"extension failed") &&
              harness.events.AnyTextContaining(L"stream failed") &&
              harness.events.AnyTextContaining(L"retry failed") &&
              harness.events.AnyTextContaining(L"marker echo") &&
              harness.events.AnyTextContaining(L"prompt rejected"),
          "json error: all five error paths surfaced");
    CheckText(!harness.events.AnyTextContaining(L"JSONKEY") &&
                  !harness.events.AnyTextContaining(L"AAABBB") &&
                  !harness.events.AnyTextContaining(L"CCCDDD") &&
                  !harness.events.AnyTextContaining(L"PLAINTEXT-MARKER-7777"),
              "json error: no key material in any error event", L"leaked");
    CheckText(harness.client.LastError().find(L"JSONKEY") == std::wstring::npos &&
                  harness.client.LastError().find(L"AAABBB") == std::wstring::npos &&
                  harness.client.LastError().find(L"CCCDDD") == std::wstring::npos &&
                  harness.client.LastError().find(L"PLAINTEXT-MARKER") == std::wstring::npos,
              "json error: LastError is redacted", harness.client.LastError());
    Check(harness.client.LastError().find(L"prompt rejected") != std::wstring::npos,
          "json error: LastError keeps the non-secret part of the message");

    const std::wstring logPath = TestConfigDir() + L"\\pi-stderr.log";
    std::string log;
    if (FILE* file = _wfopen(logPath.c_str(), L"rb")) {
        char buffer[4096];
        std::size_t got = 0;
        while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) log.append(buffer, got);
        std::fclose(file);
    }
    Check(log.find("JSONKEY") == std::string::npos && log.find("AAABBB") == std::string::npos &&
              log.find("CCCDDD") == std::string::npos &&
              log.find("PLAINTEXT-MARKER") == std::string::npos,
          "json error: diagnosis log free of the key");
    harness.client.Stop();
}

// GUI fake contract: modes selectable through the private child environment
// (a command line argument still wins), a qa fixture with three Chinese deltas
// and a [[src:...]] citation, and a command log that records prompt/abort types
// for scripts (never the prompt text or a credential).
void TestGuiFakeModes() {
    const std::wstring qaLog = ScratchPath(L"qa-commands.txt");
    DeleteScratch(qaLog);

    assistant::PiClientOptions qaOptions;
    qaOptions.piCommand = FakePiPath();
    qaOptions.configDir = TestConfigDir();
    qaOptions.workDir = TestConfigDir();
    qaOptions.requestTimeoutMs = 5000;
    qaOptions.startTimeoutMs = 5000;
    qaOptions.childEnv = {{L"FAKE_PI_MODE", L"qa"}, {L"FAKE_PI_LOG", qaLog}};
    {
        Harness harness(std::move(qaOptions));
        std::wstring error;
        CheckStart(harness.client.Start(&error), error, "qa: start with env-selected mode");
        Check(harness.client.Ask(L"fixture question", &error) > 0, "qa: ask accepted");
        Check(harness.events.WaitForDone(5000), "qa: settled");
        Check(harness.events.Count(assistant::PiEvent::Kind::TextDelta) == 3,
              "qa: exactly three deltas");
        const std::wstring deltas = harness.events.Deltas();
        Check(CountCjk(deltas) >= 6, "qa: the deltas are Chinese text");
        Check(Contains(deltas, L"[[src:dsh-session-fixture]]"),
              "qa: citation marker in the stream");
        Check(harness.events.Count(assistant::PiEvent::Kind::Tool) == 0,
              "qa: zero tool events");
        Check(harness.events.Count(assistant::PiEvent::Kind::Error) == 0, "qa: no error event");
        Check(WaitUntil([&] { return CountLine(ReadScratchLines(qaLog), "prompt") == 1; }, 3000),
              "qa: the command log records the prompt type for scripts");
        Check(CountLine(ReadScratchLines(qaLog), "get_state") == 1,
              "qa: the log stays a command-type stream");
        harness.client.Stop();
    }

    // A command line argument wins over the environment fallback.
    const std::wstring echoDump = ScratchPath(L"env-dump-precedence.txt");
    DeleteScratch(echoDump);
    assistant::PiClientOptions echoOptions = MakeOptions(L"echo");
    echoOptions.childEnv = {{L"FAKE_PI_MODE", L"qa"}};
    AppendDumpArgs(echoOptions, echoDump, {});
    {
        Harness harness(std::move(echoOptions));
        std::wstring error;
        CheckStart(harness.client.Start(&error), error, "precedence: start");
        Check(harness.client.Ask(L"fixture question", &error) > 0, "precedence: ask accepted");
        Check(harness.events.WaitForDone(5000), "precedence: settled");
        CheckText(harness.events.Deltas() == L"Hello world\u2028!",
                  "precedence: argv mode beats FAKE_PI_MODE", harness.events.Deltas());
        harness.client.Stop();
    }
    DeleteScratch(echoDump);

    // Abort fixture through the environment: one partial delta, then the abort
    // command must settle the run without an error event.
    const std::wstring abortLog = ScratchPath(L"abort-commands.txt");
    DeleteScratch(abortLog);
    assistant::PiClientOptions abortOptions;
    abortOptions.piCommand = FakePiPath();
    abortOptions.configDir = TestConfigDir();
    abortOptions.workDir = TestConfigDir();
    abortOptions.requestTimeoutMs = 5000;
    abortOptions.startTimeoutMs = 5000;
    abortOptions.childEnv = {{L"FAKE_PI_MODE", L"abort"}, {L"FAKE_PI_LOG", abortLog}};
    {
        Harness harness(std::move(abortOptions));
        std::wstring error;
        CheckStart(harness.client.Start(&error), error, "abort fixture: start");
        Check(harness.client.Ask(L"fixture question", &error) > 0, "abort fixture: ask accepted");
        Check(harness.events.WaitForEvent(
                  [](const std::vector<assistant::PiEvent>& events) {
                      for (const auto& event : events) {
                          if (event.kind == assistant::PiEvent::Kind::TextDelta) return true;
                      }
                      return false;
                  },
                  5000),
              "abort fixture: partial delta arrives");
        harness.client.Cancel();
        Check(harness.events.WaitForDone(3000), "abort fixture: done after cancel");
        Check(harness.events.FirstDoneAborted(), "abort fixture: done is aborted");
        Check(harness.events.Count(assistant::PiEvent::Kind::Error) == 0,
              "abort fixture: no error event");
        Check(WaitUntil([&] { return CountLine(ReadScratchLines(abortLog), "abort") == 1; }, 3000),
              "abort fixture: the log records the abort type");
        harness.client.Stop();
    }
    DeleteScratch(qaLog);
    DeleteScratch(abortLog);
}

} // namespace

int wmain() {
    const std::wstring fake = FakePiPath();
    if (GetFileAttributesW(fake.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("[pi-test] fake-pi-rpc.exe not found next to the test executable\n");
        return 1;
    }

    TestJson();
    TestEcho();
    TestBogusId();
    TestGarbageLine();
    TestCrlfFraming();
    TestTimeout();
    TestAbort();
    TestCrashRestart();
    TestStopAndRestart();
    TestSessionOverride();
    TestDiagnosticsMasking();
    TestStartAfterCrashExhaustion();
    TestTimeoutKeepsRunUnsettled(L"stale", /*abortFails=*/false);
    TestTimeoutKeepsRunUnsettled(L"stale-fail", /*abortFails=*/true);
    TestStopClearsUnsettledRun();
    TestIdleCancelWritesNoAbort();
    TestStopIsBoundedDuringRestart();
    TestRequestStopInterruptsHandshake();
    TestPrivateChildEnvironment();
    TestProviderExtensionFlag();
    TestChildEnvBlockShape();
    TestEnvSecretMasking();
    TestValidJsonErrorRedaction();
    TestGuiFakeModes();

    std::printf("[pi-test] %s (%d failure%s)\n", failures == 0 ? "ok" : "failed", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
