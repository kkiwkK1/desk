// M2.1 single-dispatch, public-IPv4, credential-free HTTPS transport.
#pragma once
#include "data/cardspec/cardspec.h"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace cardspec {

inline constexpr unsigned kHttpTotalTimeoutMs = 10000;
inline constexpr unsigned kHttpResolveTimeoutMs = 2000;
inline constexpr std::size_t kMaxHttpBodyBytes = kMaxSpecBytes;
using IPv4 = std::array<unsigned char, 4>;

enum class HttpFailure {
    None, NotConfigured, NotApproved, InvalidUrl, Dns, AddressBlocked,
    Timeout, UnsupportedSecurity, Transport, Tls, PeerMismatch,
    Redirect, HttpStatus, ContentType, ContentEncoding, TooLarge
};

// Trusted user configuration, outside model-writable cardspec.json.
// Approval is valid only for the same profile and canonical exact URL.
struct HttpPolicy {
    std::string profile;
    std::wstring url;
    std::string approvedProfile;
    std::wstring approvedUrl;
};

struct HttpEndpoint {
    std::wstring host;   // canonical lowercase ASCII DNS hostname (no IP literal).
    std::wstring target; // exact ASCII path/query, starting '/'.
    std::wstring url;    // canonical HTTPS URL; not for logs/errors.
};

struct ResolveResult {
    HttpFailure failure = HttpFailure::Dns;
    unsigned platformError = 0;
    std::vector<IPv4> addresses;
};

struct WireRequest {
    HttpEndpoint endpoint;
    IPv4 address{};
    unsigned timeoutMs = kHttpTotalTimeoutMs; // remaining total budget.
};

struct WireResult {
    HttpFailure failure = HttpFailure::Transport;
    unsigned platformError = 0;
    unsigned status = 0;
    bool peerVerified = false;
    IPv4 peer{};
    std::string contentType;
    std::string contentEncoding;
    std::string body;
};

struct HttpResult {
    HttpFailure failure = HttpFailure::Transport;
    unsigned platformError = 0;
    unsigned status = 0;
    std::string body; // nonempty only on complete, accepted success.
    bool ok() const { return failure == HttpFailure::None && status == 200; }
};

struct HttpDependencies {
    std::function<ResolveResult(const std::wstring&, unsigned timeoutMs)> resolve;
    std::function<WireResult(const WireRequest&)> exchange;
    std::function<std::uint64_t()> nowMs;
};

// No I/O. URL: https/443 only, ASCII DNS host with >=2 labels; no userinfo,
// IP literal, explicit port, fragment, controls, backslash or spaces; <=2048.
// Exact target/query is trusted user input, never supplied by the spec.
bool ParseHttpEndpoint(std::wstring_view url, HttpEndpoint* out,
                       HttpFailure* failure = nullptr);
bool AuthorizeHttp(std::string_view profile, const HttpPolicy& policy,
                   HttpEndpoint* out, HttpFailure* failure = nullptr);
bool IsPublicIPv4(const IPv4& address);
std::wstring HttpFailureText(HttpFailure failure); // static, payload-free.

// Defaults construct callbacks without doing I/O. Only FetchHttp may call them.
HttpDependencies DefaultHttpDependencies();
HttpResult FetchHttp(std::string_view profile, const HttpPolicy& policy,
                     const HttpDependencies& dependencies);
using HttpFetcher = std::function<HttpResult(std::string_view, const HttpPolicy&)>;

} // namespace cardspec
