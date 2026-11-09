#include "widget/http.h"

#include <winhttp.h>

#include <string>

namespace wg {
namespace {

struct ProxyConfig {
    std::wstring proxy;
    std::wstring bypass;
};

// The machine may reach the API through a local proxy (e.g. a VPN client that
// only sets the Windows proxy). WinHTTP does not use that by default, so read
// the current user's IE/WinINet proxy config and pass it explicitly.
std::wstring SelectProxy(const std::wstring& raw) {
    if (raw.empty() || raw.find(L'=') == std::wstring::npos) return raw;

    std::wstring httpsProxy, httpProxy;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t semicolon = raw.find(L';', pos);
        std::wstring part = raw.substr(pos, semicolon == std::wstring::npos
                                                ? std::wstring::npos
                                                : semicolon - pos);
        const size_t first = part.find_first_not_of(L" \t");
        if (first != std::wstring::npos) part = part.substr(first);
        if (part.compare(0, 6, L"https=") == 0) httpsProxy = part.substr(6);
        else if (part.compare(0, 5, L"http=") == 0) httpProxy = part.substr(5);

        if (semicolon == std::wstring::npos) break;
        pos = semicolon + 1;
    }
    if (!httpsProxy.empty()) return httpsProxy;
    if (!httpProxy.empty()) return httpProxy;
    return raw;
}

bool ReadProxy(ProxyConfig& out) {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&config)) return false;

    if (config.lpszProxy) out.proxy = config.lpszProxy;
    if (config.lpszProxyBypass) out.bypass = config.lpszProxyBypass;
    if (config.lpszAutoConfigUrl) GlobalFree(config.lpszAutoConfigUrl);
    if (config.lpszProxy) GlobalFree(config.lpszProxy);
    if (config.lpszProxyBypass) GlobalFree(config.lpszProxyBypass);

    out.proxy = SelectProxy(out.proxy);
    return !out.proxy.empty();
}

HINTERNET OpenSession(const wchar_t* userAgent, const ProxyConfig* proxy) {
    if (!proxy) {
        return WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_NO_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    return WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_NAMED_PROXY, proxy->proxy.c_str(),
                       proxy->bypass.empty() ? WINHTTP_NO_PROXY_BYPASS : proxy->bypass.c_str(),
                       0);
}

HttpResponse DoRequest(const wchar_t* method, const wchar_t* host, const wchar_t* path,
                       const std::wstring& bearer, const std::string& body,
                       int receiveTimeoutMs, const wchar_t* userAgent,
                       const ProxyConfig* proxy, const HttpChunkFn* onChunk) {
    HttpResponse out;

    HINTERNET session = OpenSession(userAgent, proxy);
    if (!session) return out;
    WinHttpSetTimeouts(session, 3000, 5000, 5000,
                       receiveTimeoutMs > 0 ? receiveTimeoutMs : 8000);

    HINTERNET conn = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) { WinHttpCloseHandle(session); return out; }

    HINTERNET req = WinHttpOpenRequest(conn, method, path, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return out;
    }

    std::wstring headers = onChunk ? L"Accept: text/event-stream" : L"Accept: application/json";
    if (!body.empty()) headers = L"Content-Type: application/json\r\n" + headers;
    if (!bearer.empty()) headers = L"Authorization: Bearer " + bearer + L"\r\n" + headers;

    LPVOID payload = body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data();
    const DWORD payloadSize = body.empty() ? 0 : (DWORD)body.size();

    BOOL sent = WinHttpSendRequest(req, headers.c_str(), (DWORD)-1L, payload, payloadSize,
                                   payloadSize, 0) &&
                WinHttpReceiveResponse(req, nullptr);
    if (sent) {
        out.transport = true;
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        out.status = code;

        const bool streamed = onChunk != nullptr && out.status == 200;
        DWORD available = 0;
        do {
            available = 0;
            if (!WinHttpQueryDataAvailable(req, &available)) break;
            if (!available) break;
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(req, chunk.data(), available, &read)) break;
            chunk.resize(read);
            if (streamed) {
                if (!(*onChunk)(chunk.data(), chunk.size())) break;
            } else {
                out.body += chunk;
                // Error bodies are small; a runaway endpoint should not grow
                // this process without bound.
                if (out.body.size() > 64 * 1024) break;
            }
        } while (available > 0);
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return out;
}

HttpResponse DoWithFallback(const wchar_t* method, const wchar_t* host, const wchar_t* path,
                            const std::wstring& bearer, const std::string& body,
                            int receiveTimeoutMs, const wchar_t* userAgent,
                            const HttpChunkFn* onChunk = nullptr) {
    ProxyConfig proxy;
    const bool hasProxy = ReadProxy(proxy);

    HttpResponse out = DoRequest(method, host, path, bearer, body, receiveTimeoutMs,
                                 userAgent, hasProxy ? &proxy : nullptr, onChunk);
    // A stale Windows proxy (VPN client closed, setting left on) must not break
    // direct connectivity: fall back once without the proxy. Never retry a
    // stream that already delivered bytes: deltas would be duplicated.
    if (!out.transport && hasProxy) {
        out = DoRequest(method, host, path, bearer, body, receiveTimeoutMs, userAgent, nullptr,
                        onChunk);
    }
    return out;
}

} // namespace

HttpResponse HttpGet(const wchar_t* host, const wchar_t* path,
                     const std::wstring& bearer, const wchar_t* userAgent) {
    return DoWithFallback(L"GET", host, path, bearer, std::string(), 8000, userAgent);
}

HttpResponse HttpPost(const wchar_t* host, const wchar_t* path,
                      const std::wstring& bearer, const std::string& body,
                      int receiveTimeoutMs, const wchar_t* userAgent) {
    return DoWithFallback(L"POST", host, path, bearer, body, receiveTimeoutMs, userAgent);
}

HttpResponse HttpPostStream(const wchar_t* host, const wchar_t* path,
                            const std::wstring& bearer, const std::string& body,
                            const HttpChunkFn& onChunk, int receiveTimeoutMs,
                            const wchar_t* userAgent) {
    return DoWithFallback(L"POST", host, path, bearer, body, receiveTimeoutMs, userAgent,
                          onChunk ? &onChunk : nullptr);
}

} // namespace wg
