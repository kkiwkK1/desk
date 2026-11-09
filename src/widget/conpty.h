// wg::Pty - a Windows pseudo console (ConPTY) plus the client attached to it.
//
// This is the terminal shape of a child process: the client thinks it owns a
// real console (line editing, TUI apps, ANSI colours, window size), while we
// only read a byte stream and write keystrokes back. Requires Windows 10 1809+
// (CreatePseudoConsole lives in kernel32 since then).
//
// Lifetime rule, same as the pipe helper it replaces: Shutdown() ends the client
// tree and tears the pseudo console down, Close() frees the handles and must run
// only after the reader thread has returned.
#pragma once

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <mutex>
#include <string>
#include <string_view>

namespace wg {

class Pty {
public:
    Pty() = default;
    ~Pty();
    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    // 'workDir' empty = inherit ours. 'error' (optional) receives the failing
    // HRESULT/GetLastError for the status line.
    bool Start(const std::wstring& app, const std::wstring& args, int cols, int rows,
               const std::wstring& workDir, DWORD* error = nullptr);
    bool running() const;
    DWORD processId() const;
    DWORD exitCode() const;

    // Blocking read of the console's VT byte stream. 0 = end of stream.
    int Read(char* buf, int len);
    bool Write(std::string_view bytes);
    bool Resize(int cols, int rows);

    void Shutdown();   // kill the client tree, then close the pseudo console
    void Close();      // release handles; only after the reader has returned

private:
    HPCON pc_ = nullptr;
    HANDLE out_ = nullptr;    // we read the console output here
    HANDLE consoleIn_ = nullptr;   // console side of the input pipe
    HANDLE consoleOut_ = nullptr;  // console side of the output pipe
    HANDLE in_ = nullptr;     // we write keystrokes here
    HANDLE proc_ = nullptr;
    HANDLE job_ = nullptr;
    std::mutex inMu_;
};

} // namespace wg
