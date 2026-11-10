// M2.1 WinHTTP transport: exactly one pinned HTTPS GET per fetch.
//
// Safety invariants:
//   * authorization is decided before any socket work happens (see FetchHttp);
//   * DNS is a single asynchronous GetAddrInfoExW bounded by its own sub-budget;
//     a cancelled lookup is never freed while the provider may still write to
//     its OVERLAPPED, and at most one such orphan is tolerated;
//   * the session is private (NO_PROXY), HTTP/1.x with the HTTP/2+HTTP/3 upgrade
//     flags explicitly off, never pooled globally (195 set once as a flag),
//     never redirected, never cookie/credential or client-certificate capable;
//   * the TCP peer is pinned to the verified public IPv4 through option 165 and
//     then re-checked against the real connection before any body byte is read;
//   * TLS certificate validation is never disabled: session mask is TLS 1.2|1.3,
//     secure-protocol fallback is off, HTTP/2+HTTP/3 upgrades stay disabled;
//   * response bodies are capped at 32 KiB with no truncation-as-success, no
//     retries, no proxy and no framework HTTP helper.
// Error text never includes URLs, hosts, headers or response payload.
#include "data/cardspec/http.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h> // must precede <windows.h>/<winhttp.h> for _WS2DEF_
#include <ws2tcpip.h>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// MinGW's <ws2tcpip.h> declares the synchronous family but omits these Vista+/
// Win8+ exports; both symbols are present in ws2_32. The API owns the name handle
// that GetAddrInfoExW hands back (GetAddrInfoExCancel takes LPHANDLE so it can
// clear it), so this file never calls CloseHandle on it.
extern "C" int WSAAPI GetAddrInfoExCancel(LPHANDLE nameHandle);
extern "C" int WSAAPI GetAddrInfoExOverlappedResult(LPOVERLAPPED overlapped);

namespace cardspec {
namespace {

constexpr const wchar_t* kUserAgent = L"DesktopDesk-CardSpec/2.1";
constexpr unsigned kCancelGraceMs = 100;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

bool EnsureWinsock() {
    static std::once_flag once;
    static bool started = false;
    std::call_once(once, [] {
        WSADATA data{};
        started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    // Deliberately never WSACleanup: a cancelled lookup may still be completing.
    return started;
}

ULONGLONG MonotonicMs() { return GetTickCount64(); }

unsigned RemainingMs(ULONGLONG deadline, unsigned cap) {
    const ULONGLONG now = MonotonicMs();
    if (now >= deadline) return 0;
    const ULONGLONG left = deadline - now;
    return static_cast<unsigned>(std::min<ULONGLONG>(left, cap));
}

std::wstring FormatIPv4(const IPv4& address) {
    std::wstring text;
    for (std::size_t i = 0; i < address.size(); ++i) {
        if (i != 0) text.push_back(L'.');
        text.append(std::to_wstring(address[i]));
    }
    return text;
}

// Header values we accept are ASCII only; anything else is "unreadable" rather
// than silently normalised.
bool NarrowHeader(std::wstring_view text, std::string* out) {
    out->clear();
    out->reserve(text.size());
    for (const wchar_t c : text) {
        if (c < 0x20 || c > 0x7E) return false;
        out->push_back(static_cast<char>(c));
    }
    return true;
}

std::string_view TrimAscii(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

bool EqualsAsciiCI(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        char a = left[i];
        char b = right[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// "application/json" with at most a matching charset=utf-8 parameter.
bool IsJsonContentType(std::string_view value) {
    std::size_t pos = 0;
    bool sawType = false;
    for (;;) {
        const std::size_t semicolon = value.find(';', pos);
        const std::string_view part = TrimAscii(value.substr(
            pos, semicolon == std::string_view::npos ? std::string_view::npos : semicolon - pos));
        if (!sawType) {
            if (!EqualsAsciiCI(part, "application/json")) return false;
            sawType = true;
        } else if (!part.empty()) {
            const std::size_t equals = part.find('=');
            if (equals == std::string_view::npos) return false;
            if (!EqualsAsciiCI(TrimAscii(part.substr(0, equals)), "charset")) return false;
            std::string_view charset = TrimAscii(part.substr(equals + 1));
            if (charset.size() >= 2 && charset.front() == '"' && charset.back() == '"') {
                charset = charset.substr(1, charset.size() - 2);
            }
            if (!EqualsAsciiCI(charset, "utf-8")) return false;
        }
        if (semicolon == std::string_view::npos) break;
        pos = semicolon + 1;
    }
    return sawType;
}

bool IsIdentityEncoding(std::string_view value) {
    const std::string_view trimmed = TrimAscii(value);
    return trimmed.empty() || EqualsAsciiCI(trimmed, "identity");
}

HttpFailure MapWinHttpError(DWORD error) {
    switch (error) {
        case ERROR_WINHTTP_TIMEOUT: return HttpFailure::Timeout;
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return HttpFailure::Dns;
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_REVOKED:
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
        case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
        // A client certificate demand is also refused: we never present one.
        case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
        case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED_PROXY:
            return HttpFailure::Tls;
        default: return HttpFailure::Transport;
    }
}

struct WinHttpHandle {
    HINTERNET value = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : value(handle) {}
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    ~WinHttpHandle() {
        if (value) WinHttpCloseHandle(value);
    }
};

// ---------------------------------------------------------------------------
// DNS: one asynchronous lookup with a hard sub-budget and a single orphan slot
// ---------------------------------------------------------------------------

// Everything the provider may read or write asynchronously lives in this heap
// context, never on the caller's stack: the hostname buffer and hints must stay
// valid for the whole call, and the OVERLAPPED (with its hEvent) must stay valid
// even when the lookup is abandoned as an orphan.
struct DnsLookup {
    std::wstring hostCopy;
    ADDRINFOEXW hints{};
    OVERLAPPED overlapped{};
    HANDLE event = nullptr;
    HANDLE nameHandle = nullptr;
    PADDRINFOEXW result = nullptr;
    bool orphaned = false; // only then may another thread reclaim this slot
};

std::mutex g_dnsMutex;
DnsLookup* g_pending = nullptr; // live or orphaned lookup; at most one

void ReleaseLookup(DnsLookup* lookup, bool freeResult) {
    if (!lookup) return;
    if (freeResult && lookup->result) FreeAddrInfoExW(lookup->result);
    lookup->result = nullptr;
    if (lookup->event) {
        CloseHandle(lookup->event);
        lookup->event = nullptr;
    }
    delete lookup;
}

enum class ReserveStatus { Ok, Busy, NoMemory };

// A previous cancellation may still be completing inside the provider. Reclaim
// it only once it has been marked orphaned (no thread touches it any more) and
// its event is signalled; otherwise refuse to start another lookup so pending
// contexts can never pile up or be freed under the provider's feet.
ReserveStatus ReserveLookup(DnsLookup** out) {
    std::lock_guard<std::mutex> lock(g_dnsMutex);
    if (g_pending) {
        if (!g_pending->orphaned) return ReserveStatus::Busy;
        if (!g_pending->event || WaitForSingleObject(g_pending->event, 0) != WAIT_OBJECT_0) {
            return ReserveStatus::Busy;
        }
        ReleaseLookup(g_pending, true);
        g_pending = nullptr;
    }
    DnsLookup* lookup = new (std::nothrow) DnsLookup();
    if (!lookup) return ReserveStatus::NoMemory;
    g_pending = lookup;
    *out = lookup;
    return ReserveStatus::Ok;
}

void ClearLookup(DnsLookup* lookup, bool freeResult) {
    std::lock_guard<std::mutex> lock(g_dnsMutex);
    if (g_pending == lookup) g_pending = nullptr;
    ReleaseLookup(lookup, freeResult);
}

// Abandoned after a bounded cancel wait: the context, its event and its result
// stay allocated until the provider signals, and the slot is marked so no other
// thread will free it while that completion is still outstanding.
void OrphanLookup(DnsLookup* lookup) {
    std::lock_guard<std::mutex> lock(g_dnsMutex);
    if (g_pending == lookup) lookup->orphaned = true;
}

ResolveResult ResolvePublicHost(const std::wstring& host, unsigned timeoutMs) {
    ResolveResult out;
    out.failure = HttpFailure::Dns;

    if (host.empty() || host.size() > 253u) return out;
    if (timeoutMs == 0) {
        out.failure = HttpFailure::Timeout;
        return out;
    }
    if (!EnsureWinsock()) {
        out.platformError = WSAGetLastError();
        return out;
    }

    DnsLookup* lookup = nullptr;
    switch (ReserveLookup(&lookup)) {
        case ReserveStatus::Ok: break;
        case ReserveStatus::NoMemory: return out;
        case ReserveStatus::Busy:
            out.failure = HttpFailure::Timeout;
            return out;
    }

    lookup->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!lookup->event) {
        out.platformError = GetLastError();
        ClearLookup(lookup, false);
        return out;
    }
    // The provider signals this event on completion; without hEvent the pending
    // lookup would never be observed and the slot would stay busy for ever.
    lookup->overlapped.hEvent = lookup->event;
    lookup->hostCopy.assign(host);
    lookup->hints.ai_family = AF_INET;
    lookup->hints.ai_socktype = SOCK_STREAM;
    lookup->hints.ai_protocol = IPPROTO_TCP;

    int rc = GetAddrInfoExW(lookup->hostCopy.c_str(), nullptr, NS_DNS, nullptr, &lookup->hints,
                            &lookup->result, nullptr, &lookup->overlapped, nullptr,
                            &lookup->nameHandle);
    if (rc != 0 && rc != WSA_IO_PENDING) {
        out.platformError = static_cast<unsigned>(rc);
        ClearLookup(lookup, true);
        return out;
    }

    if (rc == WSA_IO_PENDING) {
        // Reserve <=100ms of the sub-budget for cancellation bookkeeping.
        const unsigned grace = timeoutMs >= 2 * kCancelGraceMs
                                   ? kCancelGraceMs
                                   : static_cast<unsigned>(timeoutMs / 2);
        const unsigned waitMs = timeoutMs - grace;
        const DWORD state = WaitForSingleObject(lookup->event, static_cast<DWORD>(waitMs));
        if (state != WAIT_OBJECT_0) {
            if (state == WAIT_FAILED) out.platformError = GetLastError();
            // Official docs: the provider may keep running after a successful
            // cancel, so the result is discarded and the outcome stays Timeout.
            if (lookup->nameHandle) GetAddrInfoExCancel(&lookup->nameHandle);
            const DWORD graceWait = grace ? static_cast<DWORD>(grace) : 1;
            const DWORD confirmed = WaitForSingleObject(lookup->event, graceWait);
            if (confirmed == WAIT_OBJECT_0) {
                ClearLookup(lookup, true);
            } else {
                // The provider may still write into the context: keep it and the
                // slot alive; the next resolve reclaims it only once signalled.
                OrphanLookup(lookup);
            }
            out.failure = HttpFailure::Timeout;
            return out;
        }
        // The provider's own completion status is authoritative for error codes.
        const int completion = GetAddrInfoExOverlappedResult(&lookup->overlapped);
        if (completion != 0) {
            out.platformError = static_cast<unsigned>(completion);
            ClearLookup(lookup, true);
            out.failure = HttpFailure::Dns;
            return out;
        }
    }
    if (!lookup->result) {
        ClearLookup(lookup, true);
        out.failure = HttpFailure::Dns;
        return out;
    }

    bool malformed = false;
    std::vector<IPv4> addresses;
    for (PADDRINFOEXW entry = lookup->result; entry; entry = entry->ai_next) {
        if (entry->ai_family != AF_INET || !entry->ai_addr ||
            entry->ai_addrlen < sizeof(sockaddr_in)) {
            malformed = true;
            break;
        }
        const sockaddr_in* ipv4 = reinterpret_cast<const sockaddr_in*>(entry->ai_addr);
        IPv4 address{};
        std::memcpy(address.data(), &ipv4->sin_addr, address.size());
        addresses.push_back(address);
    }
    ClearLookup(lookup, true);

    if (malformed || addresses.empty()) {
        out.failure = HttpFailure::Dns;
        return out;
    }
    for (const IPv4& address : addresses) {
        if (!IsPublicIPv4(address)) {
            out.failure = HttpFailure::AddressBlocked;
            return out;
        }
    }
    out.failure = HttpFailure::None;
    out.addresses = std::move(addresses);
    return out;
}

// ---------------------------------------------------------------------------
// WinHTTP single exchange
// ---------------------------------------------------------------------------

bool SetOptionDword(HINTERNET handle, DWORD option, DWORD value) {
    return WinHttpSetOption(handle, option, &value, sizeof(value)) != FALSE;
}

bool SetTimeoutMs(HINTERNET handle, DWORD option, unsigned ms) {
    return SetOptionDword(handle, option, static_cast<DWORD>(ms));
}

// Stage timeouts are always the *remaining* budget and are applied to the
// request scope only: WinHTTP options 6 and 7 are request-settable, so a FALSE
// result fails closed instead of falling back to a session default. A zero
// millisecond timeout would mean "infinite", so callers reject zero first.
bool SetStageTimeout(HINTERNET request, DWORD option, unsigned ms) {
    return SetTimeoutMs(request, option, ms);
}

// WINHTTP_OPTION_DISABLE_GLOBAL_POOLING (195) is flag-typed: the local
// no-network probe accepted only WinHttpSetOption(session, 195, nullptr, 0)
// (TRUE/err 0); a BOOL buffer or a non-empty length is rejected
// (FALSE/ERROR_INVALID_PARAMETER). Single pinned call, no signature probing:
// FALSE still fails closed as UnsupportedSecurity at the caller.
bool DisableGlobalPooling(HINTERNET session) {
    return WinHttpSetOption(session, WINHTTP_OPTION_DISABLE_GLOBAL_POOLING, nullptr, 0) != FALSE;
}

// TLS is a single documented session call: DWORD mask with TLS 1.2 as the floor
// and TLS 1.3 allowed. Fallback to older protocols is disabled with the session
// BOOL. There is deliberately no narrowing retry: if the mask is rejected the
// platform cannot meet the policy, so the caller fails closed.
bool SetTlsPolicy(HINTERNET session) {
    const DWORD mask =
        WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!SetOptionDword(session, WINHTTP_OPTION_SECURE_PROTOCOLS, mask)) return false;
    BOOL noFallback = TRUE;
    return WinHttpSetOption(session, WINHTTP_OPTION_DISABLE_SECURE_PROTOCOL_FALLBACK, &noFallback,
                            sizeof(noFallback)) != FALSE;
}

// HTTP/1.x only: an explicit request-scoped DWORD 0 keeps the HTTP/2 and HTTP/3
// upgrade flags off (WinHTTP never negotiates them without that flag).
bool DisableHttpUpgrade(HINTERNET request) {
    return SetOptionDword(request, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, 0);
}

// Headers are optional: a missing header is reported as absent, an unreadable or
// oversized one makes the caller fail closed. The usual size probe fails with
// ERROR_INSUFFICIENT_BUFFER, which is expected and not an error.
constexpr DWORD kMaxHeaderChars = 4096;

std::wstring QueryHeaderString(HINTERNET request, DWORD query, bool* readable) {
    *readable = true;
    DWORD size = 0;
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        const DWORD error = GetLastError();
        if (error == ERROR_WINHTTP_HEADER_NOT_FOUND) return {}; // absent, not an error
        if (error != ERROR_INSUFFICIENT_BUFFER) {
            *readable = false;
            return {};
        }
    }
    if (size < sizeof(wchar_t)) return {};
    // Bound the allocation before trusting a server-supplied length.
    if (size > kMaxHeaderChars * sizeof(wchar_t)) {
        *readable = false;
        return {};
    }
    std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
    DWORD written = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(),
                             &written, WINHTTP_NO_HEADER_INDEX)) {
        *readable = false;
        return {};
    }
    // A zero or over-long value is never returned in part.
    if (written == 0 || static_cast<std::size_t>(written) > buffer.size() * sizeof(wchar_t)) {
        *readable = false;
        return {};
    }
    buffer.resize(written / sizeof(wchar_t));
    while (!buffer.empty() && buffer.back() == L'\0') buffer.pop_back();
    return buffer;
}

enum class LengthStatus { Absent, Present, Invalid };

// Content-Length is used as an integrity check: absent/chunked is fine, a legal
// value must match the bytes actually received, and an unreadable value fails
// closed instead of being guessed.
LengthStatus QueryDeclaredLength(HINTERNET request, unsigned long long* length) {
    unsigned long long declared = 0;
    DWORD size = sizeof(declared);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                             WINHTTP_HEADER_NAME_BY_INDEX, &declared, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return GetLastError() == ERROR_WINHTTP_HEADER_NOT_FOUND ? LengthStatus::Absent
                                                                : LengthStatus::Invalid;
    }
    if (size != sizeof(declared)) return LengthStatus::Invalid;
    *length = declared;
    return LengthStatus::Present;
}

// The peer check is the second line of defence behind option 165: the socket we
// actually got must be the literal we resolved and pinned.
bool QueryPeer(HINTERNET request, const IPv4& pinned, WireResult* out) {
    WINHTTP_CONNECTION_INFO info{};
    info.cbSize = sizeof(info);
    DWORD size = sizeof(info);
    if (!WinHttpQueryOption(request, WINHTTP_OPTION_CONNECTION_INFO, &info, &size)) return false;
    if (size < sizeof(info)) return false;
    const sockaddr* remote = reinterpret_cast<const sockaddr*>(&info.RemoteAddress);
    if (remote->sa_family != AF_INET) return false;
    const sockaddr_in* ipv4 = reinterpret_cast<const sockaddr_in*>(&info.RemoteAddress);
    if (ntohs(ipv4->sin_port) != INTERNET_DEFAULT_HTTPS_PORT) return false;
    IPv4 peer{};
    std::memcpy(peer.data(), &ipv4->sin_addr, peer.size());
    out->peer = peer;
    if (peer != pinned) return false;
    if (!IsPublicIPv4(peer)) return false;
    out->peerVerified = true;
    return true;
}

WireResult ExchangeHttps(const WireRequest& request) {
    WireResult out;
    out.failure = HttpFailure::Transport;

    if (request.timeoutMs == 0) {
        out.failure = HttpFailure::Timeout;
        return out;
    }
    if (request.endpoint.host.empty() || request.endpoint.host.size() > 253u) {
        out.failure = HttpFailure::InvalidUrl;
        return out;
    }
    if (request.endpoint.target.empty() || request.endpoint.target.front() != L'/' ||
        request.endpoint.target.size() > 2048u) {
        out.failure = HttpFailure::InvalidUrl;
        return out;
    }
    if (!IsPublicIPv4(request.address)) {
        out.failure = HttpFailure::AddressBlocked;
        return out;
    }

    const unsigned budget = request.timeoutMs;
    const ULONGLONG deadline = MonotonicMs() + budget;

    // Private session: no proxy, no automatic config, no shared pool.
    WinHttpHandle session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_NO_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value) {
        out.platformError = GetLastError();
        return out;
    }
    if (!WinHttpSetTimeouts(session.value, static_cast<int>(budget), static_cast<int>(budget),
                            static_cast<int>(budget), static_cast<int>(budget))) {
        out.platformError = GetLastError();
        return out;
    }
    if (!DisableGlobalPooling(session.value)) {
        out.failure = HttpFailure::UnsupportedSecurity;
        out.platformError = GetLastError();
        return out;
    }
    if (!SetOptionDword(session.value, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, 1)) {
        out.failure = HttpFailure::UnsupportedSecurity;
        out.platformError = GetLastError();
        return out;
    }
    if (!SetTlsPolicy(session.value)) {
        out.failure = HttpFailure::UnsupportedSecurity;
        out.platformError = GetLastError();
        return out;
    }

    // Connect to the approved hostname on 443 only; no port or scheme override.
    WinHttpHandle connection(
        WinHttpConnect(session.value, request.endpoint.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.value) {
        out.platformError = GetLastError();
        return out;
    }
    WinHttpHandle requestHandle(WinHttpOpenRequest(connection.value, L"GET",
                                                   request.endpoint.target.c_str(), nullptr,
                                                   WINHTTP_NO_REFERER, nullptr,
                                                   WINHTTP_FLAG_SECURE));
    if (!requestHandle.value) {
        out.platformError = GetLastError();
        return out;
    }

    // Request scope only: these two options are settable on the request handle,
    // so a FALSE result means the guarantee is unavailable. Retrying on another
    // handle would only pretend the flag took effect.
    const DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS |
                           WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_KEEP_ALIVE;
    if (!SetOptionDword(requestHandle.value, WINHTTP_OPTION_DISABLE_FEATURE, disabled)) {
        out.failure = HttpFailure::UnsupportedSecurity;
        out.platformError = GetLastError();
        return out;
    }
    // HIGH autologon: never present the logged-on user's credentials.
    if (!SetOptionDword(requestHandle.value, WINHTTP_OPTION_AUTOLOGON_POLICY,
                        WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH)) {
        out.failure = HttpFailure::UnsupportedSecurity;
        out.platformError = GetLastError();
        return out;
    }
    if (!DisableHttpUpgrade(requestHandle.value)) {
        out.failure = HttpFailure::UnsupportedSecurity;
        out.platformError = GetLastError();
        return out;
    }

    // Pin the numeric address; Host/SNI stay the approved hostname. Option 165
    // needs Windows 10 21H1+ and a request handle before send.
    const std::wstring pin = FormatIPv4(request.address);
    if (!WinHttpSetOption(requestHandle.value, WINHTTP_OPTION_RESOLUTION_HOSTNAME,
                          const_cast<wchar_t*>(pin.c_str()),
                          static_cast<DWORD>((pin.size() + 1) * sizeof(wchar_t)))) {
        out.failure = HttpFailure::UnsupportedSecurity;
        out.platformError = GetLastError();
        return out;
    }

    unsigned remaining = RemainingMs(deadline, budget);
    if (remaining == 0) {
        out.failure = HttpFailure::Timeout;
        return out;
    }
    // Send gets its own remaining slice, never the session's initial full budget,
    // and a zero timeout (which WinHTTP treats as infinite) is never passed.
    if (!SetStageTimeout(requestHandle.value, WINHTTP_OPTION_SEND_TIMEOUT, remaining)) {
        out.platformError = GetLastError();
        return out;
    }
    const wchar_t* headers = L"Accept: application/json\r\nAccept-Encoding: identity";
    if (!WinHttpSendRequest(requestHandle.value, headers, static_cast<DWORD>(-1),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        const DWORD error = GetLastError();
        out.failure = MapWinHttpError(error);
        out.platformError = error;
        return out;
    }

    remaining = RemainingMs(deadline, budget);
    if (remaining == 0) {
        out.failure = HttpFailure::Timeout;
        return out;
    }
    // Option 7 (RECEIVE_RESPONSE_TIMEOUT) is the only timeout that covers the
    // entire response-header phase (default 90s); option 6 does not. Both are
    // request-scope settable, so set each to the same remaining slice and fail
    // closed if either is refused. Option 7 is checked lazily, so the 10s budget
    // is logical: the observed return may overrun by up to one receive timeout.
    if (!SetOptionDword(requestHandle.value, WINHTTP_OPTION_RECEIVE_TIMEOUT, remaining) ||
        !SetOptionDword(requestHandle.value, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT,
                        remaining)) {
        out.platformError = GetLastError();
        return out;
    }
    if (!WinHttpReceiveResponse(requestHandle.value, nullptr)) {
        const DWORD error = GetLastError();
        out.failure = MapWinHttpError(error);
        out.platformError = error;
        return out;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(requestHandle.value,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        out.platformError = GetLastError();
        return out;
    }
    out.status = status;
    if (status >= 300 && status <= 399) {
        out.failure = HttpFailure::Redirect;
        return out;
    }
    if (status != 200) {
        // 401/407/client-cert are refused rather than authenticated or retried.
        out.failure = HttpFailure::HttpStatus;
        return out;
    }

    if (!QueryPeer(requestHandle.value, request.address, &out)) {
        out.failure = HttpFailure::PeerMismatch;
        out.peerVerified = false;
        return out;
    }

    bool readable = true;
    const std::wstring rawType =
        QueryHeaderString(requestHandle.value, WINHTTP_QUERY_CONTENT_TYPE, &readable);
    if (!readable) {
        out.failure = HttpFailure::ContentType;
        return out;
    }
    if (!NarrowHeader(rawType, &out.contentType)) {
        out.failure = HttpFailure::ContentType;
        return out;
    }
    const std::wstring rawEncoding =
        QueryHeaderString(requestHandle.value, WINHTTP_QUERY_CONTENT_ENCODING, &readable);
    if (!readable) {
        out.failure = HttpFailure::ContentEncoding;
        return out;
    }
    if (!NarrowHeader(rawEncoding, &out.contentEncoding)) {
        out.failure = HttpFailure::ContentEncoding;
        return out;
    }
    if (!IsJsonContentType(out.contentType)) {
        out.failure = HttpFailure::ContentType;
        return out;
    }
    if (!IsIdentityEncoding(out.contentEncoding)) {
        out.failure = HttpFailure::ContentEncoding;
        return out;
    }

    unsigned long long declared = 0;
    const LengthStatus lengthStatus = QueryDeclaredLength(requestHandle.value, &declared);
    if (lengthStatus == LengthStatus::Invalid) {
        out.failure = HttpFailure::Transport; // unreadable length: fail closed
        return out;
    }
    const bool hasLength = lengthStatus == LengthStatus::Present;
    if (hasLength && declared > static_cast<unsigned long long>(kMaxHttpBodyBytes)) {
        out.failure = HttpFailure::TooLarge;
        return out;
    }

    std::string body;
    for (;;) {
        remaining = RemainingMs(deadline, budget);
        if (remaining == 0) {
            out.failure = HttpFailure::Timeout;
            return out;
        }
        if (!SetStageTimeout(requestHandle.value, WINHTTP_OPTION_RECEIVE_TIMEOUT, remaining)) {
            out.platformError = GetLastError();
            return out;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(requestHandle.value, &available)) {
            const DWORD error = GetLastError();
            out.failure = MapWinHttpError(error);
            out.platformError = error;
            return out;
        }
        if (available == 0) break;
        const std::size_t room = kMaxHttpBodyBytes - body.size();
        if (static_cast<std::size_t>(available) > room) {
            out.failure = HttpFailure::TooLarge;
            return out; // never a truncated success
        }
        std::vector<char> chunk(static_cast<std::size_t>(available));
        // Recompute again immediately before the blocking read.
        remaining = RemainingMs(deadline, budget);
        if (remaining == 0) {
            out.failure = HttpFailure::Timeout;
            return out;
        }
        if (!SetStageTimeout(requestHandle.value, WINHTTP_OPTION_RECEIVE_TIMEOUT, remaining)) {
            out.platformError = GetLastError();
            return out;
        }
        DWORD read = 0;
        if (!WinHttpReadData(requestHandle.value, chunk.data(), available, &read)) {
            const DWORD error = GetLastError();
            out.failure = MapWinHttpError(error);
            out.platformError = error;
            return out;
        }
        if (read == 0) break;
        if (static_cast<std::size_t>(read) > room) {
            out.failure = HttpFailure::TooLarge;
            return out;
        }
        body.append(chunk.data(), static_cast<std::size_t>(read));
        if (body.size() > kMaxHttpBodyBytes) {
            out.failure = HttpFailure::TooLarge;
            return out;
        }
        if (hasLength && body.size() > static_cast<std::size_t>(declared)) {
            out.failure = HttpFailure::Transport; // more bytes than announced
            return out;
        }
    }

    // A truncated transfer is never reported as success.
    if (hasLength && body.size() != static_cast<std::size_t>(declared)) {
        out.failure = HttpFailure::Transport;
        return out;
    }

    out.failure = HttpFailure::None;
    out.body = std::move(body);
    return out;
}

} // namespace

HttpDependencies DefaultHttpDependencies() {
    // Construction only captures function objects: no socket, DNS or WinHTTP
    // call happens until FetchHttp actually needs a dependency.
    HttpDependencies dependencies;
    dependencies.resolve = [](const std::wstring& host, unsigned timeoutMs) {
        return ResolvePublicHost(host, timeoutMs);
    };
    dependencies.exchange = [](const WireRequest& request) { return ExchangeHttps(request); };
    dependencies.nowMs = [] { return static_cast<std::uint64_t>(MonotonicMs()); };
    return dependencies;
}

HttpResult FetchHttp(std::string_view profile, const HttpPolicy& policy,
                     const HttpDependencies& dependencies) {
    HttpResult out;
    out.failure = HttpFailure::Transport;

    // A dependency that is missing or throws is a closed failure, never an
    // implicit direct connection.
    if (!dependencies.resolve || !dependencies.exchange || !dependencies.nowMs) {
        out.failure = HttpFailure::NotConfigured;
        return out;
    }

    HttpEndpoint endpoint;
    HttpFailure authorization = HttpFailure::NotConfigured;
    if (!AuthorizeHttp(profile, policy, &endpoint, &authorization)) {
        out.failure = authorization;
        return out;
    }

    std::uint64_t start = 0;
    try {
        start = dependencies.nowMs();
    } catch (...) {
        out.failure = HttpFailure::Transport;
        return out;
    }

    // One resolve, bounded by the DNS sub-budget, then exactly one exchange.
    ResolveResult resolved;
    try {
        resolved = dependencies.resolve(
            endpoint.host, std::min<unsigned>(kHttpResolveTimeoutMs, kHttpTotalTimeoutMs));
    } catch (...) {
        out.failure = HttpFailure::Transport;
        return out;
    }
    if (resolved.failure != HttpFailure::None) {
        out.failure = resolved.failure;
        out.platformError = resolved.platformError;
        return out;
    }
    out.platformError = resolved.platformError;
    if (resolved.addresses.empty()) {
        out.failure = HttpFailure::Dns;
        return out;
    }
    // The whole answer must be public; anything else means a mixed/rebinding
    // resolver and is rejected before a socket exists.
    for (const IPv4& address : resolved.addresses) {
        if (!IsPublicIPv4(address)) {
            out.failure = HttpFailure::AddressBlocked;
            return out;
        }
    }
    const IPv4 pin = resolved.addresses.front();

    unsigned remaining = 0;
    {
        std::uint64_t now = 0;
        try {
            now = dependencies.nowMs();
        } catch (...) {
            out.failure = HttpFailure::Transport;
            return out;
        }
        if (now < start) {
            out.failure = HttpFailure::Timeout; // clock regression: fail closed
            return out;
        }
        const std::uint64_t elapsed = now - start;
        if (elapsed >= kHttpTotalTimeoutMs) {
            out.failure = HttpFailure::Timeout;
            return out;
        }
        remaining = static_cast<unsigned>(kHttpTotalTimeoutMs - elapsed);
    }

    WireRequest request;
    request.endpoint = endpoint;
    request.address = pin;
    request.timeoutMs = remaining;

    WireResult exchanged;
    try {
        exchanged = dependencies.exchange(request);
    } catch (...) {
        out.failure = HttpFailure::Transport;
        return out;
    }
    out.platformError = exchanged.platformError;
    out.status = exchanged.status;
    if (exchanged.failure != HttpFailure::None) {
        out.failure = exchanged.failure;
        return out;
    }

    {
        std::uint64_t now = 0;
        try {
            now = dependencies.nowMs();
        } catch (...) {
            out.failure = HttpFailure::Transport;
            return out;
        }
        if (now < start) {
            out.failure = HttpFailure::Timeout;
            return out;
        }
        if (now - start > kHttpTotalTimeoutMs) {
            out.failure = HttpFailure::Timeout;
            return out;
        }
    }

    if (!exchanged.peerVerified) {
        out.failure = HttpFailure::PeerMismatch;
        return out;
    }
    if (exchanged.peer != pin) {
        out.failure = HttpFailure::PeerMismatch;
        return out;
    }
    if (exchanged.status >= 300 && exchanged.status <= 399) {
        out.failure = HttpFailure::Redirect;
        return out;
    }
    if (exchanged.status != 200) {
        out.failure = HttpFailure::HttpStatus;
        return out;
    }
    if (!IsIdentityEncoding(exchanged.contentEncoding)) {
        out.failure = HttpFailure::ContentEncoding;
        return out;
    }
    if (!IsJsonContentType(exchanged.contentType)) {
        out.failure = HttpFailure::ContentType;
        return out;
    }
    if (exchanged.body.size() > kMaxHttpBodyBytes) {
        out.failure = HttpFailure::TooLarge;
        return out;
    }

    out.failure = HttpFailure::None;
    out.body = std::move(exchanged.body);
    return out;
}

} // namespace cardspec
