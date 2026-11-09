#include "widget/proc.h"

#include <algorithm>
#include <string>

namespace wg {
namespace {

bool DrainPipe(HANDLE pipe, std::string& out, std::size_t maxOutput) {
    bool any = false;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
            break;
        }
        char buf[4096];
        const DWORD want = available < sizeof(buf) ? available : (DWORD)sizeof(buf);
        DWORD got = 0;
        if (!ReadFile(pipe, buf, want, &got, nullptr) || got == 0) break;
        any = true;
        if (out.size() < maxOutput) out.append(buf, got);
    }
    return any;
}

HANDLE MakeInheritable(HANDLE source) {
    if (source == nullptr || source == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE copy = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &copy, 0, TRUE,
                         DUPLICATE_SAME_ACCESS)) {
        return nullptr;
    }
    return copy;
}

// A kill-on-close job is the only reliable way to terminate ssh.exe together
// with anything it may have spawned. Assignment can fail on old systems or
// inside exotic nested jobs; that is not fatal, only the cleanup is weaker.
HANDLE MakeKillJob() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

} // namespace

std::wstring QuoteArg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;

    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

RunResult RunCapture(const std::wstring& app, const std::wstring& args,
                     const std::string& input, int timeoutMs, std::size_t maxOutput) {
    RunResult r;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRead = nullptr, outWrite = nullptr, inRead = nullptr, inWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) return r;
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&inRead, &inWrite, &sa, 0)) {
        CloseHandle(outRead);
        CloseHandle(outWrite);
        return r;
    }
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;

    PROCESS_INFORMATION pi{};
    std::wstring cmdline = QuoteArg(app) + L" " + args;
    const BOOL created = CreateProcessW(app.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
                                        &si, &pi);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (!created) {
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return r;
    }
    r.started = true;

    HANDLE job = MakeKillJob();
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);

    if (!input.empty()) {
        std::size_t offset = 0;
        while (offset < input.size()) {
            DWORD wrote = 0;
            const DWORD chunk = (DWORD)std::min<std::size_t>(input.size() - offset, 4096);
            if (!WriteFile(inWrite, input.data() + offset, chunk, &wrote, nullptr) || wrote == 0) {
                break;
            }
            offset += wrote;
        }
    }
    CloseHandle(inWrite);

    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)(timeoutMs > 0 ? timeoutMs : 1);
    bool exited = false;
    for (;;) {
        const bool got = DrainPipe(outRead, r.output, maxOutput);
        if (!exited && WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) exited = true;

        if (exited) {
            if (got) continue;
            // ssh can write its final error line as it exits; give the pipe a
            // moment to flush instead of losing a useful "timed out" message.
            Sleep(20);
            if (!DrainPipe(outRead, r.output, maxOutput)) break;
            continue;
        }
        if (GetTickCount64() >= deadline) {
            r.timedOut = true;
            if (job) TerminateJobObject(job, 1);
            else TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            exited = true;
            DrainPipe(outRead, r.output, maxOutput);
            break;
        }
        if (!got) Sleep(15);
    }

    if (!r.timedOut) GetExitCodeProcess(pi.hProcess, &r.exitCode);
    if (job) CloseHandle(job);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(outRead);
    return r;
}

int RunInherited(const std::wstring& app, const std::wstring& args, int timeoutMs,
                 bool* timedOut) {
    if (timedOut) *timedOut = false;

    HANDLE dupIn = MakeInheritable(GetStdHandle(STD_INPUT_HANDLE));
    HANDLE dupOut = MakeInheritable(GetStdHandle(STD_OUTPUT_HANDLE));
    HANDLE dupErr = MakeInheritable(GetStdHandle(STD_ERROR_HANDLE));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = dupIn ? dupIn : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = dupOut ? dupOut : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = dupErr ? dupErr : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring cmdline = QuoteArg(app) + L" " + args;
    const BOOL created = CreateProcessW(app.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
                                        CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    if (dupIn) CloseHandle(dupIn);
    if (dupOut) CloseHandle(dupOut);
    if (dupErr) CloseHandle(dupErr);
    if (!created) return -1;

    HANDLE job = MakeKillJob();
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);

    const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs > 0 ? (DWORD)timeoutMs : INFINITE);
    int code = 0;
    if (wait == WAIT_TIMEOUT) {
        if (timedOut) *timedOut = true;
        if (job) TerminateJobObject(job, 1);
        else TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
        code = 124;
    } else {
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        code = (int)exitCode;
    }

    if (job) CloseHandle(job);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

HANDLE CreateKillJob() { return MakeKillJob(); }

} // namespace wg
