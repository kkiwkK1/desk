// wg::proc - small Win32 process helpers shared by widgets and tools.
//
// Two shapes:
//   RunCapture   - hide the window, feed stdin, capture stdout+stderr, time out
//   RunInherited - hand the console (and redirected stdio) to the child, wait
//
// Everything here is deliberately dependency-free: CreateProcessW plus a job
// object so a timed-out ssh cannot leave children behind.
#pragma once

#include <windows.h>

#include <cstddef>
#include <mutex>
#include <string>

namespace wg {

// Windows command-line quoting (the rules CreateProcess expects for argv).
std::wstring QuoteArg(const std::wstring& arg);

struct RunResult {
    bool started = false;
    bool timedOut = false;
    DWORD exitCode = 0;
    std::string output;   // stdout and stderr, merged, capped at maxOutput
};

// Blocks for at most timeoutMs (0 means "use 1 ms"). Never throws.
RunResult RunCapture(const std::wstring& app, const std::wstring& args,
                     const std::string& input, int timeoutMs,
                     std::size_t maxOutput = 64 * 1024);

// Runs the child attached to this console with inherited stdio so ssh can
// prompt for a password and stream output. Returns the exit code, 124 on
// timeout, or -1 when the process could not be created.
int RunInherited(const std::wstring& app, const std::wstring& args, int timeoutMs,
                 bool* timedOut = nullptr);

// A job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, or nullptr when the
// system refuses one. Anything a child spawns dies with the job.
HANDLE CreateKillJob();

} // namespace wg