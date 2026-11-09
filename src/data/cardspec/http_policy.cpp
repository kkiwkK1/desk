// M2.1 HTTP policy: pure URL, approval, address and error-text rules.
//
// This translation unit does no I/O, no logging and no payload echo. It only
// answers three questions:
//   * is this user-configured URL a plain public HTTPS DNS endpoint?
//   * does the stored approval bind that exact profile and canonical URL?
//   * may this literal IPv4 answer be pinned?
// Error text is a fixed table; it never repeats the URL, host, query, response
// body or header bytes (those may carry paths or secrets).
#include "data/cardspec/http.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace cardspec {
namespace {

constexpr std::size_t kMaxUrlChars = 2048;
constexpr std::size_t kMaxHostChars = 253;
constexpr std::size_t kMaxLabelChars = 63;
constexpr std::size_t kMaxProfileChars = 32;
constexpr std::size_t kMinHostLabels = 2;

// Labels that only resolve inside a private network or the local resolver.
constexpr std::wstring_view kBlockedLabels[] = {
    L"localhost", L"local", L"internal", L"lan", L"home",
};

// Query keys that smell like credentials. Matching is by exact decoded key so
// that "monkey=1" is still allowed; no value is ever inspected or echoed.
constexpr std::string_view kBlockedQueryKeys[] = {
    "token",        "access_token", "access-token", "id_token",  "refresh_token",
    "auth_token",   "api_key",      "apikey",       "api-key",   "x-api-key",
    "key",          "private_key",  "password",     "passwd",    "pwd",
    "secret",       "client_secret", "auth",        "authorization", "signature",
    "sig",          "credential",   "credentials",  "bearer",    "session",
    "sessionid",    "session_id",   "jwt",
};

bool IsAsciiAlpha(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
}

bool IsAsciiDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

bool IsHexDigit(wchar_t c) {
    return IsAsciiDigit(c) || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

int HexValue(wchar_t c) {
    if (c >= L'0' && c <= L'9') return static_cast<int>(c - L'0');
    if (c >= L'a' && c <= L'f') return static_cast<int>(c - L'a') + 10;
    return static_cast<int>(c - L'A') + 10;
}

wchar_t ToLowerAscii(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

bool IsPrintableAscii(wchar_t c) { return c >= 0x20 && c <= 0x7E; }

// Scheme match is case-insensitive; canonical output is always lowercase.
bool HasHttpsScheme(std::wstring_view url) {
    constexpr std::wstring_view kScheme = L"https://";
    if (url.size() < kScheme.size()) return false;
    for (std::size_t i = 0; i < kScheme.size(); ++i) {
        if (ToLowerAscii(url[i]) != kScheme[i]) return false;
    }
    return true;
}

bool IsValidLabel(std::wstring_view label, bool isLastLabel) {
    if (label.empty() || label.size() > kMaxLabelChars) return false;
    if (label.front() == L'-' || label.back() == L'-') return false;
    bool allDigits = true;
    for (const wchar_t c : label) {
        if (!(IsAsciiAlpha(c) || IsAsciiDigit(c) || c == L'-')) return false;
        if (!IsAsciiDigit(c)) allDigits = false;
    }
    // An all-numeric final label is how "1.1.1.1" and other IPv4 spellings
    // masquerade as DNS names; a DNS top-level label is never all digits.
    if (isLastLabel && allDigits) return false;
    return true;
}

bool IsBlockedLabel(std::wstring_view label) {
    for (const std::wstring_view blocked : kBlockedLabels) {
        if (label == blocked) return true;
    }
    return false;
}

// Canonicalises an ASCII DNS hostname: lowercase, >=2 labels, 1..63 per label,
// <=253 total. Rejects IP literals, single-label names and private suffixes.
bool CanonicalizeHost(std::wstring_view authority, std::wstring* host) {
    if (authority.empty() || authority.size() > kMaxHostChars) return false;

    std::wstring lowered;
    lowered.reserve(authority.size());
    for (const wchar_t c : authority) {
        if (!(IsAsciiAlpha(c) || IsAsciiDigit(c) || c == L'-' || c == L'.')) {
            return false; // '@', ':', '%', brackets, controls, non-ASCII, ...
        }
        lowered.push_back(ToLowerAscii(c));
    }
    if (lowered.empty() || lowered.back() == L'.') return false; // empty label

    std::size_t labels = 0;
    std::size_t pos = 0;
    while (pos <= lowered.size()) {
        const std::size_t dot = lowered.find(L'.', pos);
        const std::wstring_view label = std::wstring_view(lowered).substr(
            pos, dot == std::wstring::npos ? std::wstring::npos : dot - pos);
        const bool isLastLabel = dot == std::wstring::npos;
        if (!IsValidLabel(label, isLastLabel) || IsBlockedLabel(label)) return false;
        ++labels;
        if (isLastLabel) break;
        pos = dot + 1;
    }
    if (labels < kMinHostLabels) return false;

    *host = std::move(lowered);
    return true;
}

// Exact ASCII path/query: printable ASCII, no space, no backslash/'#', and every
// '%' must start a valid escape that does not hide a control byte or backslash.
// Input is never trimmed: surrounding whitespace simply fails these rules.
bool ValidateEscapedTarget(std::wstring_view target) {
    for (std::size_t i = 0; i < target.size(); ++i) {
        const wchar_t c = target[i];
        if (!IsPrintableAscii(c) || c == L' ') return false;
        if (c == L'\\' || c == L'#') return false;
        if (c != L'%') continue;
        if (i + 2 >= target.size()) return false;
        if (!IsHexDigit(target[i + 1]) || !IsHexDigit(target[i + 2])) return false;
        const int decoded = HexValue(target[i + 1]) * 16 + HexValue(target[i + 2]);
        if (decoded <= 0x1F || decoded == 0x7F) return false;
        if (decoded == static_cast<int>(L'\\')) return false;
        i += 2;
    }
    return true;
}

bool DecodeQueryKey(std::wstring_view raw, std::string* key) {
    std::string decoded;
    decoded.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const wchar_t c = raw[i];
        if (c == L'%') {
            if (i + 2 >= raw.size() || !IsHexDigit(raw[i + 1]) || !IsHexDigit(raw[i + 2])) {
                return false;
            }
            decoded.push_back(static_cast<char>(HexValue(raw[i + 1]) * 16 + HexValue(raw[i + 2])));
            i += 2;
            continue;
        }
        if (!IsPrintableAscii(c)) return false;
        decoded.push_back(static_cast<char>(c));
    }
    for (char& c : decoded) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    *key = std::move(decoded);
    return true;
}

bool IsBlockedQueryKey(std::string_view key) {
    for (const std::string_view blocked : kBlockedQueryKeys) {
        if (key == blocked) return true;
    }
    return false;
}

bool QueryKeysAreAllowed(std::wstring_view target) {
    const std::size_t question = target.find(L'?');
    if (question == std::wstring_view::npos) return true;
    const std::wstring_view query = target.substr(question + 1);
    std::size_t pos = 0;
    while (pos <= query.size()) {
        const std::size_t amp = query.find(L'&', pos);
        const std::wstring_view pair = query.substr(
            pos, amp == std::wstring_view::npos ? std::wstring_view::npos : amp - pos);
        if (!pair.empty()) {
            const std::size_t equals = pair.find(L'=');
            const std::wstring_view rawKey =
                equals == std::wstring_view::npos ? pair : pair.substr(0, equals);
            std::string key;
            if (!DecodeQueryKey(rawKey, &key) || IsBlockedQueryKey(key)) return false;
        }
        if (amp == std::wstring_view::npos) break;
        pos = amp + 1;
    }
    return true;
}

bool IsValidProfile(std::string_view profile) {
    if (profile.empty() || profile.size() > kMaxProfileChars) return false;
    for (const char raw : profile) {
        const unsigned char c = static_cast<unsigned char>(raw);
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool allowed = alpha || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!allowed) return false;
    }
    return true;
}

} // namespace

bool ParseHttpEndpoint(std::wstring_view url, HttpEndpoint* out, HttpFailure* failure) {
    auto fail = [failure](HttpFailure reason) {
        if (failure) *failure = reason;
        return false;
    };

    if (!out) return fail(HttpFailure::InvalidUrl);
    if (url.empty() || url.size() > kMaxUrlChars) return fail(HttpFailure::InvalidUrl);
    if (!HasHttpsScheme(url)) return fail(HttpFailure::InvalidUrl);

    constexpr std::size_t kSchemeChars = 8; // "https://"
    const std::size_t authorityStart = kSchemeChars;
    std::size_t authorityEnd = url.size();
    for (std::size_t i = authorityStart; i < url.size(); ++i) {
        const wchar_t c = url[i];
        if (c == L'/' || c == L'?' || c == L'#') {
            authorityEnd = i;
            break;
        }
    }
    if (authorityEnd == authorityStart) return fail(HttpFailure::InvalidUrl);

    HttpEndpoint endpoint;
    if (!CanonicalizeHost(url.substr(authorityStart, authorityEnd - authorityStart),
                          &endpoint.host)) {
        return fail(HttpFailure::InvalidUrl);
    }

    std::wstring_view rest = url.substr(authorityEnd);
    if (!rest.empty() && rest.front() == L'#') return fail(HttpFailure::InvalidUrl);
    if (rest.empty()) {
        endpoint.target = L"/";
    } else if (rest.front() == L'?') {
        endpoint.target = L"/";
        endpoint.target.append(rest);
    } else {
        endpoint.target.assign(rest);
    }

    if (endpoint.target.empty() || endpoint.target.front() != L'/') {
        return fail(HttpFailure::InvalidUrl);
    }
    if (!ValidateEscapedTarget(endpoint.target)) return fail(HttpFailure::InvalidUrl);
    if (!QueryKeysAreAllowed(endpoint.target)) return fail(HttpFailure::InvalidUrl);

    endpoint.url = L"https://";
    endpoint.url.append(endpoint.host);
    endpoint.url.append(endpoint.target);
    if (endpoint.url.size() > kMaxUrlChars) return fail(HttpFailure::InvalidUrl);

    *out = std::move(endpoint);
    if (failure) *failure = HttpFailure::None;
    return true;
}

bool AuthorizeHttp(std::string_view profile, const HttpPolicy& policy, HttpEndpoint* out,
                   HttpFailure* failure) {
    auto fail = [failure](HttpFailure reason) {
        if (failure) *failure = reason;
        return false;
    };

    if (!out) return fail(HttpFailure::NotConfigured);
    if (!IsValidProfile(profile)) return fail(HttpFailure::NotConfigured);
    if (!IsValidProfile(policy.profile)) return fail(HttpFailure::NotConfigured);
    if (std::string_view(policy.profile) != profile) return fail(HttpFailure::NotConfigured);
    if (policy.url.empty()) return fail(HttpFailure::NotConfigured);

    // Approval must be for this profile and this exact canonical URL.
    if (policy.approvedUrl.empty()) return fail(HttpFailure::NotApproved);
    if (std::string_view(policy.approvedProfile) != profile) return fail(HttpFailure::NotApproved);

    HttpEndpoint configured;
    HttpFailure configuredFailure = HttpFailure::InvalidUrl;
    if (!ParseHttpEndpoint(policy.url, &configured, &configuredFailure)) {
        return fail(configuredFailure);
    }
    HttpEndpoint approved;
    HttpFailure approvedFailure = HttpFailure::InvalidUrl;
    if (!ParseHttpEndpoint(policy.approvedUrl, &approved, &approvedFailure)) {
        return fail(approvedFailure);
    }
    if (configured.host != approved.host || configured.target != approved.target) {
        return fail(HttpFailure::NotApproved);
    }

    *out = std::move(configured);
    if (failure) *failure = HttpFailure::None;
    return true;
}

bool IsPublicIPv4(const IPv4& address) {
    const unsigned b0 = address[0];
    const unsigned b1 = address[1];
    const unsigned b2 = address[2];

    if (b0 == 0) return false;                       // 0.0.0.0/8 ("this host")
    if (b0 == 10) return false;                      // 10.0.0.0/8
    if (b0 == 127) return false;                     // loopback
    if (b0 >= 224) return false;                     // multicast, 240/4, broadcast
    if (b0 == 100 && b1 >= 64 && b1 <= 127) return false;  // 100.64.0.0/10 CGNAT
    if (b0 == 169 && b1 == 254) return false;        // link-local
    if (b0 == 172 && b1 >= 16 && b1 <= 31) return false;   // 172.16.0.0/12
    if (b0 == 192 && b1 == 0 && b2 == 0) return false;     // 192.0.0.0/24
    if (b0 == 192 && b1 == 0 && b2 == 2) return false;     // TEST-NET-1
    if (b0 == 192 && b1 == 88 && b2 == 99) return false;   // 6to4 relay anycast
    if (b0 == 192 && b1 == 168) return false;        // 192.168.0.0/16
    if (b0 == 198 && (b1 == 18 || b1 == 19)) return false; // benchmarking
    if (b0 == 198 && b1 == 51 && b2 == 100) return false;  // TEST-NET-2
    if (b0 == 203 && b1 == 0 && b2 == 113) return false;   // TEST-NET-3
    return true;
}

std::wstring HttpFailureText(HttpFailure failure) {
    switch (failure) {
        case HttpFailure::None: return L"正常";
        case HttpFailure::NotConfigured: return L"未配置 HTTPS 端点";
        case HttpFailure::NotApproved: return L"该端点尚未获得批准";
        case HttpFailure::InvalidUrl: return L"端点地址不符合公开 HTTPS 规则";
        case HttpFailure::Dns: return L"域名解析失败";
        case HttpFailure::AddressBlocked: return L"解析结果不是公网 IPv4，已阻止";
        case HttpFailure::Timeout: return L"网络请求超出预算";
        case HttpFailure::UnsupportedSecurity: return L"当前系统不支持所需安全选项";
        case HttpFailure::Transport: return L"网络传输失败";
        case HttpFailure::Tls: return L"TLS 证书校验失败";
        case HttpFailure::PeerMismatch: return L"实际连接地址与固定地址不一致";
        case HttpFailure::Redirect: return L"服务器要求重定向，已拒绝";
        case HttpFailure::HttpStatus: return L"HTTP 状态不允许";
        case HttpFailure::ContentType: return L"响应不是严格的 JSON 类型";
        case HttpFailure::ContentEncoding: return L"响应使用了不支持的编码";
        case HttpFailure::TooLarge: return L"响应超过 32 KiB 上限";
    }
    return L"网络请求失败";
}

} // namespace cardspec
