// One synchronous HTTPS GET on WinHTTP. Called from a widget's worker thread,
// so blocking here never freezes the desktop.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <functional>
#include <string>

namespace wg {

// Returning false aborts the transfer (used to stop a canceled stream).
using HttpChunkFn = std::function<bool(const char*, std::size_t)>;

struct HttpResponse {
    bool transport = false;   // request completed (any status)
    DWORD status = 0;         // HTTP status when transport == true
    std::string body;

    bool ok() const { return transport && status == 200; }
};

// Direct connection on purpose: WPAD auto-discovery can stall ~30s on networks
// without a proxy, and the APIs these widgets call are reachable directly.
HttpResponse HttpGet(const wchar_t* host, const wchar_t* path,
                     const std::wstring& bearer,
                     const wchar_t* userAgent = L"WinWidget/1.0");

// JSON POST for chat-style APIs. 'receiveTimeoutMs' bounds each read, not the
// whole call: a non-streaming completion can stay silent for tens of seconds
// while the model generates, so pass something larger than the 8s GET default.
HttpResponse HttpPost(const wchar_t* host, const wchar_t* path,
                      const std::wstring& bearer, const std::string& body,
                      int receiveTimeoutMs = 30000,
                      const wchar_t* userAgent = L"WinWidget/1.0");

// Streaming POST (Server-Sent Events). 'onChunk' receives bytes as they
// arrive; the body is not accumulated on success. Non-200 responses still
// collect the error body so the caller can report it.
HttpResponse HttpPostStream(const wchar_t* host, const wchar_t* path,
                            const std::wstring& bearer, const std::string& body,
                            const HttpChunkFn& onChunk,
                            int receiveTimeoutMs = 60000,
                            const wchar_t* userAgent = L"WinWidget/1.0");

} // namespace wg
