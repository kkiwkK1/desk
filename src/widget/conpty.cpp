#include "widget/conpty.h"

#include "widget/proc.h"

#include <algorithm>
#include <cstdio>
#include <thread>
#include <vector>

namespace wg {
namespace {

// The two ends the pseudo console owns; closed in the parent right after
// CreatePseudoConsole so only conhost holds them.
void CloseIfValid(HANDLE& h) {
    if (h) {
        CloseHandle(h);
        h = nullptr;
    }
}

} // namespace

Pty::~Pty() {
    Shutdown();
    Close();
}

bool Pty::Start(const std::wstring& app, const std::wstring& args, int cols, int rows,
                const std::wstring& workDir, DWORD* error) {
    Shutdown();
    Close();
    if (error) *error = 0;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    // Console input: conhost reads from inRead, we write to inWrite.
    HANDLE inRead = nullptr, inWrite = nullptr;
    // Console output: conhost writes to outWrite, we read from outRead.
    HANDLE outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &sa, 0)) {
        if (error) *error = GetLastError();
        return false;
    }
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
        if (error) *error = GetLastError();
        CloseHandle(inRead);
        CloseHandle(inWrite);
        return false;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    const HRESULT hr =
        CreatePseudoConsole(COORD{(SHORT)cols, (SHORT)rows}, inRead, outWrite, 0, &pc_);
    // Keep the console-side handles: closing them here is what the samples do,
    // but on this build conhost then sees a dead console and the client quietly
    // attaches to the caller's console instead. They are released in Close().
    consoleIn_ = inRead;
    consoleOut_ = outWrite;
    if (FAILED(hr) || !pc_) {
        if (error) *error = (DWORD)hr;
        CloseHandle(inWrite);
        CloseHandle(outRead);
        pc_ = nullptr;
        return false;
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    SIZE_T attrBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrBytes);
    // The attribute list must be pointer-aligned; a plain byte buffer is not,
    // and a misaligned list makes the kernel quietly ignore the console.
    const std::size_t slots =
        (attrBytes + sizeof(void*) - 1) / sizeof(void*) + 1;
    std::vector<void*> attrStorage(slots, nullptr);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
    bool attrsOk =
        InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrBytes) != FALSE;
    if (attrsOk) {
        attrsOk = UpdateProcThreadAttribute(si.lpAttributeList, 0,
                                            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pc_,
                                            sizeof(pc_), nullptr, nullptr) != FALSE;
    }
    if (!attrsOk) {
        if (error) *error = GetLastError();
        if (si.lpAttributeList) DeleteProcThreadAttributeList(si.lpAttributeList);
        ClosePseudoConsole(pc_);
        pc_ = nullptr;
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return false;
    }

    PROCESS_INFORMATION pi{};
    std::wstring cmdline = QuoteArg(app) + L" " + args;

    // Deliberately no CREATE_NO_WINDOW: combined with the pseudo console
    // attribute the child can end up attached to nothing at all (conhost then
    // never renders, and the pipe stays silent). A pseudo console has no window
    // to hide in the first place.
    const BOOL created = CreateProcessW(
        app.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT, nullptr,
        workDir.empty() ? nullptr : workDir.c_str(), &si.StartupInfo, &pi);

    DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!created) {
        if (error) *error = GetLastError();
        ClosePseudoConsole(pc_);
        pc_ = nullptr;
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return false;
    }

    job_ = CreateKillJob();
    if (job_) AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    proc_ = pi.hProcess;
    in_ = inWrite;
    out_ = outRead;
    return true;
}

bool Pty::running() const {
    if (!proc_) return false;
    return WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT;
}

DWORD Pty::processId() const { return proc_ ? GetProcessId(proc_) : 0; }

DWORD Pty::exitCode() const {
    if (!proc_) return (DWORD)-1;
    DWORD code = 0;
    if (!GetExitCodeProcess(proc_, &code)) return (DWORD)-1;
    return code;
}

int Pty::Read(char* buf, int len) {
    if (!out_ || len <= 0) return 0;
    DWORD got = 0;
    if (!ReadFile(out_, buf, (DWORD)len, &got, nullptr)) return 0;
    return (int)got;
}

bool Pty::Write(std::string_view bytes) {
    std::lock_guard<std::mutex> lock(inMu_);
    if (!in_) return false;

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD wrote = 0;
        const DWORD chunk = (DWORD)std::min<std::size_t>(bytes.size() - offset, 4096);
        if (!WriteFile(in_, bytes.data() + offset, chunk, &wrote, nullptr) || wrote == 0) {
            return false;
        }
        offset += wrote;
    }
    return true;
}

bool Pty::Resize(int cols, int rows) {
    if (!pc_) return false;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    return SUCCEEDED(ResizePseudoConsole(pc_, COORD{(SHORT)cols, (SHORT)rows}));
}

void Pty::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(inMu_);
        CloseIfValid(in_);
    }

    // Kill the client (and anything it spawned) before closing the pseudo
    // console: ClosePseudoConsole waits for the client to go away, so doing it
    // first would hang on a live session.
    if (proc_ && WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT) {
        if (job_) TerminateJobObject(job_, 1);
        if (WaitForSingleObject(proc_, 1500) == WAIT_TIMEOUT) TerminateProcess(proc_, 1);
        WaitForSingleObject(proc_, 2000);
    }
    if (pc_) {
        HPCON pc = pc_;
        pc_ = nullptr;
        // ClosePseudoConsole blocks until its client is gone. A client that
        // attached to the wrong console (or never attached) would block us
        // forever, so give it a moment and then let the call finish on its own.
        std::thread([pc] { ClosePseudoConsole(pc); }).detach();
        Sleep(150);
    }

    // Release the pipes: a reader blocked in ReadFile only wakes on EOF, and
    // the console that would have closed it may already be gone.
    CloseIfValid(consoleIn_);
    CloseIfValid(consoleOut_);
    CloseIfValid(out_);
}

void Pty::Close() {
    {
        std::lock_guard<std::mutex> lock(inMu_);
        CloseIfValid(in_);
    }
    CloseIfValid(consoleIn_);
    CloseIfValid(consoleOut_);
    CloseIfValid(out_);
    CloseIfValid(proc_);
    CloseIfValid(job_);
}

} // namespace wg
