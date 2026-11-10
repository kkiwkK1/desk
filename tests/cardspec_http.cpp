// M2.1 HTTP policy/transport tests.
//
// Default run: fully offline. Every resolver, exchange and clock is a fake, so
// no socket, DNS query, proxy lookup or user configuration is touched. The
// single "--live-probe" branch is a fixed public HTTPS GET that is never part of
// the default test run; it is only for a separately authorized probe session.
#include "data/cardspec/http.h"

#include "data/cardspec/strict_json.h"
#include "data/assistant/json.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;
int checks = 0;
int fakeResolves = 0;
int fakeExchanges = 0;

void Check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        std::printf("[cardspec-http-test] FAIL %s\n", what.c_str());
        ++failures;
    }
}

std::string With(const char* label, const std::string& what) {
    return std::string(label) + what;
}

cardspec::IPv4 Addr(unsigned a, unsigned b, unsigned c, unsigned d) {
    return cardspec::IPv4{static_cast<unsigned char>(a), static_cast<unsigned char>(b),
                          static_cast<unsigned char>(c), static_cast<unsigned char>(d)};
}

const cardspec::IPv4 kPublic1 = Addr(1, 1, 1, 1);
const cardspec::IPv4 kPublic2 = Addr(8, 8, 8, 8);

// ---------------------------------------------------------------------------
// URL rules
// ---------------------------------------------------------------------------

cardspec::HttpEndpoint ExpectValid(const std::wstring& url, const std::string& label) {
    cardspec::HttpEndpoint endpoint;
    cardspec::HttpFailure failure = cardspec::HttpFailure::InvalidUrl;
    const bool ok = cardspec::ParseHttpEndpoint(url, &endpoint, &failure);
    Check(ok, With("accept url: ", label));
    Check(failure == cardspec::HttpFailure::None, With("failure None: ", label));
    return endpoint;
}

void ExpectInvalid(const std::wstring& url, const std::string& label) {
    cardspec::HttpEndpoint endpoint;
    endpoint.host = L"unchanged";
    endpoint.target = L"unchanged";
    endpoint.url = L"unchanged";
    cardspec::HttpFailure failure = cardspec::HttpFailure::None;
    const bool ok = cardspec::ParseHttpEndpoint(url, &endpoint, &failure);
    Check(!ok, With("reject url: ", label));
    Check(failure == cardspec::HttpFailure::InvalidUrl, With("failure InvalidUrl: ", label));
    Check(endpoint.host == L"unchanged" && endpoint.target == L"unchanged" &&
              endpoint.url == L"unchanged",
          With("out untouched: ", label));
}

void TestEndpointRules() {
    const cardspec::HttpEndpoint plain = ExpectValid(L"https://api.example.com", "bare host");
    Check(plain.host == L"api.example.com", "host canonical");
    Check(plain.target == L"/", "empty target becomes /");
    Check(plain.url == L"https://api.example.com/", "canonical url");

    const std::wstring repo =
        L"https://api.github.com/repos/microsoft/Windows-classic-samples";
    const cardspec::HttpEndpoint exact = ExpectValid(repo, "path only");
    Check(exact.host == L"api.github.com", "exact host");
    Check(exact.target == L"/repos/microsoft/Windows-classic-samples", "exact target");
    Check(exact.url == repo, "exact url round-trip");

    const cardspec::HttpEndpoint cased = ExpectValid(L"HTTPS://API.Example.COM/a?b=1", "cased");
    Check(cased.host == L"api.example.com", "scheme/host lowercased");
    Check(cased.target == L"/a?b=1", "query kept verbatim");
    Check(cased.url == L"https://api.example.com/a?b=1", "canonical cased url");

    Check(ExpectValid(L"https://a.b?x=1", "query without path").target == L"/?x=1",
          "query without path gains /");
    Check(ExpectValid(L"https://123.example.com/1", "numeric leading label").host ==
              L"123.example.com",
          "numeric non-final label allowed");
    ExpectValid(L"https://a.b/%41%2f?page=2", "escaped path and query");

    ExpectInvalid(L"", "empty");
    ExpectInvalid(L"http://a.b/", "plain http");
    ExpectInvalid(L"ftp://a.b/", "ftp");
    ExpectInvalid(L"https:/a.b/", "missing slash");
    ExpectInvalid(L"https:///x", "empty authority");
    ExpectInvalid(L"https://", "scheme only");
    ExpectInvalid(L"https://a.b:443/", "explicit port");
    ExpectInvalid(L"https://user@a.b/", "userinfo");
    ExpectInvalid(L"https://a.b#frag", "fragment");
    ExpectInvalid(L"https://a.b/x#frag", "fragment after path");
    ExpectInvalid(L"https://127.0.0.1/", "ipv4 literal");
    ExpectInvalid(L"https://10.1.2.3/", "private ipv4 literal");
    ExpectInvalid(L"https://[::1]/", "ipv6 literal");
    ExpectInvalid(L"https://localhost/", "localhost");
    ExpectInvalid(L"https://foo.localhost/", "localhost label");
    ExpectInvalid(L"https://foo.local/x", "local label");
    ExpectInvalid(L"https://foo.internal/", "internal label");
    ExpectInvalid(L"https://foo.lan/", "lan label");
    ExpectInvalid(L"https://foo.home/", "home label");
    ExpectInvalid(L"https://intranet/", "single label");
    ExpectInvalid(L"https://a..b/", "empty label");
    ExpectInvalid(L"https://a.b./", "trailing dot");
    ExpectInvalid(L"https://-a.b/", "leading hyphen");
    ExpectInvalid(L"https://a-.b/", "trailing hyphen");
    ExpectInvalid(L"https://a.b/ x", "space");
    ExpectInvalid(L" https://a.b/", "leading space");
    ExpectInvalid(L"https://a.b/ ", "trailing space");
    ExpectInvalid(L"https://a.b/a b", "space in path");
    ExpectInvalid(L"https://a.b/\\x", "backslash");
    ExpectInvalid(L"https://a.b/\x01", "control byte");
    ExpectInvalid(L"https://a.b/\x4e2d", "non-ascii target");
    ExpectInvalid(L"https://a.b/%", "lone percent");
    ExpectInvalid(L"https://a.b/%0", "short escape");
    ExpectInvalid(L"https://a.b/%0g", "non-hex escape");
    ExpectInvalid(L"https://a.b/%00", "escaped NUL");
    ExpectInvalid(L"https://a.b/%1f", "escaped control");
    ExpectInvalid(L"https://a.b/%7F", "escaped DEL");
    ExpectInvalid(L"https://a.b/%5c", "escaped backslash");

    ExpectInvalid(L"https://a.b/x?token=1", "token key");
    ExpectInvalid(L"https://a.b/x?a=1&access_token=2", "access_token key");
    ExpectInvalid(L"https://a.b/x?API_KEY=1", "api_key key");
    ExpectInvalid(L"https://a.b/x?apikey=1", "apikey key");
    ExpectInvalid(L"https://a.b/x?api-key=1", "api-key key");
    ExpectInvalid(L"https://a.b/x?x-api-key=1", "x-api-key key");
    ExpectInvalid(L"https://a.b/x?key=1", "key key");
    ExpectInvalid(L"https://a.b/x?password=1", "password key");
    ExpectInvalid(L"https://a.b/x?secret=1", "secret key");
    ExpectInvalid(L"https://a.b/x?auth=1", "auth key");
    ExpectInvalid(L"https://a.b/x?authorization=1", "authorization key");
    ExpectInvalid(L"https://a.b/x?signature=1", "signature key");
    ExpectInvalid(L"https://a.b/x?t%6Fken=1", "percent-escaped token key");
    ExpectValid(L"https://a.b/x?monkey=1", "non-exact key allowed");
    ExpectValid(L"https://a.b/x?page=2&sort=updated&since=2024-01-01", "safe query keys");

    // Host/label/URL ceilings.
    std::wstring longLabel = L"https://";
    longLabel.append(64, L'a');
    longLabel += L".com/";
    ExpectInvalid(longLabel, "64-char label");

    std::wstring longHost = L"https://";
    longHost.append(191, L'a');
    longHost += L".";
    longHost.append(62, L'b');
    longHost += L"/";
    ExpectInvalid(longHost, "254-char host");

    std::wstring maxUrl = L"https://a.b/";
    maxUrl.append(2048 - maxUrl.size(), L'a');
    Check(maxUrl.size() == 2048, "2048 builder");
    ExpectValid(maxUrl, "2048-char url");

    std::wstring overUrl = maxUrl + L"a";
    ExpectInvalid(overUrl, "2049-char url");

    // A null out pointer still reports the reason without touching memory.
    cardspec::HttpFailure nullFailure = cardspec::HttpFailure::None;
    Check(!cardspec::ParseHttpEndpoint(L"https://a.b/", nullptr, &nullFailure),
          "null out rejected");
    Check(nullFailure == cardspec::HttpFailure::InvalidUrl, "null out failure");

    // The transport cap is the v1 spec cap; a literal pins the contract.
    Check(cardspec::kMaxHttpBodyBytes == 32768u, "body cap is exactly 32768 bytes");
}

// ---------------------------------------------------------------------------
// Public address matrix
// ---------------------------------------------------------------------------

void TestPublicAddresses() {
    const cardspec::IPv4 allowed[] = {
        Addr(1, 1, 1, 1),     Addr(8, 8, 8, 8),     Addr(140, 82, 112, 3),
        Addr(11, 0, 0, 1),    Addr(9, 255, 255, 255), Addr(172, 15, 255, 255),
        Addr(172, 32, 0, 1),  Addr(100, 63, 255, 255), Addr(100, 128, 0, 1),
        Addr(198, 17, 255, 255), Addr(198, 20, 0, 1),  Addr(192, 0, 1, 1),
        Addr(203, 0, 114, 1), Addr(223, 255, 255, 255),
    };
    for (const cardspec::IPv4& address : allowed) {
        Check(cardspec::IsPublicIPv4(address),
              "public address accepted: " + std::to_string(address[0]) + "." +
                  std::to_string(address[1]) + "." + std::to_string(address[2]) + "." +
                  std::to_string(address[3]));
    }

    const cardspec::IPv4 blocked[] = {
        Addr(0, 0, 0, 0),      Addr(0, 1, 2, 3),      Addr(10, 0, 0, 1),
        Addr(10, 255, 255, 255), Addr(100, 64, 0, 0),  Addr(100, 127, 255, 255),
        Addr(127, 0, 0, 1),    Addr(169, 254, 1, 1),   Addr(172, 16, 0, 1),
        Addr(172, 31, 255, 255), Addr(192, 0, 0, 1),   Addr(192, 0, 2, 1),
        Addr(192, 88, 99, 1),  Addr(192, 168, 1, 1),   Addr(198, 18, 0, 1),
        Addr(198, 19, 255, 255), Addr(198, 51, 100, 1), Addr(203, 0, 113, 1),
        Addr(224, 0, 0, 1),    Addr(239, 255, 255, 255), Addr(240, 0, 0, 1),
        Addr(255, 255, 255, 255),
    };
    for (const cardspec::IPv4& address : blocked) {
        Check(!cardspec::IsPublicIPv4(address),
              "non-public address rejected: " + std::to_string(address[0]) + "." +
                  std::to_string(address[1]) + "." + std::to_string(address[2]) + "." +
                  std::to_string(address[3]));
    }
}

// ---------------------------------------------------------------------------
// Approval binding
// ---------------------------------------------------------------------------

cardspec::HttpPolicy ApprovedPolicy() {
    cardspec::HttpPolicy policy;
    policy.profile = "demo";
    policy.url = L"https://api.github.com/repos/microsoft/Windows-classic-samples";
    policy.approvedProfile = "demo";
    policy.approvedUrl = policy.url;
    return policy;
}

cardspec::HttpEndpoint ExpectAuthorized(std::string_view profile,
                                        const cardspec::HttpPolicy& policy,
                                        const std::string& label) {
    cardspec::HttpEndpoint endpoint;
    cardspec::HttpFailure failure = cardspec::HttpFailure::NotConfigured;
    const bool ok = cardspec::AuthorizeHttp(profile, policy, &endpoint, &failure);
    Check(ok, With("authorize: ", label));
    Check(failure == cardspec::HttpFailure::None, With("authorize failure None: ", label));
    return endpoint;
}

void ExpectDenied(std::string_view profile, const cardspec::HttpPolicy& policy,
                  cardspec::HttpFailure expected, const std::string& label) {
    cardspec::HttpEndpoint endpoint;
    endpoint.host = L"unchanged";
    endpoint.target = L"unchanged";
    endpoint.url = L"unchanged";
    cardspec::HttpFailure failure = cardspec::HttpFailure::None;
    const bool ok = cardspec::AuthorizeHttp(profile, policy, &endpoint, &failure);
    Check(!ok, With("deny: ", label));
    Check(failure == expected, With("deny reason: ", label));
    Check(endpoint.host == L"unchanged" && endpoint.target == L"unchanged" &&
              endpoint.url == L"unchanged",
          With("deny out untouched: ", label));
}

void TestAuthorization() {
    const cardspec::HttpPolicy base = ApprovedPolicy();
    const cardspec::HttpEndpoint endpoint = ExpectAuthorized("demo", base, "approved");
    Check(endpoint.host == L"api.github.com", "authorized host");
    Check(endpoint.target == L"/repos/microsoft/Windows-classic-samples", "authorized target");
    Check(endpoint.url == base.url, "authorized url");

    cardspec::HttpPolicy cased = base;
    cased.approvedUrl = L"HTTPS://API.GitHub.COM/repos/microsoft/Windows-classic-samples";
    ExpectAuthorized("demo", cased, "case-insensitive canonical match");

    cardspec::HttpPolicy profileCase = base;
    ExpectDenied("Demo", profileCase, cardspec::HttpFailure::NotConfigured,
                 "profile is case sensitive");

    cardspec::HttpPolicy trailingSlash = base;
    trailingSlash.approvedUrl += L"/";
    ExpectDenied("demo", trailingSlash, cardspec::HttpFailure::NotApproved, "extra slash");

    cardspec::HttpPolicy queryDiff = base;
    queryDiff.approvedUrl += L"?page=1";
    ExpectDenied("demo", queryDiff, cardspec::HttpFailure::NotApproved, "extra query");

    cardspec::HttpPolicy otherApprover = base;
    otherApprover.approvedProfile = "other";
    ExpectDenied("demo", otherApprover, cardspec::HttpFailure::NotApproved, "other profile");

    cardspec::HttpPolicy unapproved = base;
    unapproved.approvedProfile.clear();
    unapproved.approvedUrl.clear();
    ExpectDenied("demo", unapproved, cardspec::HttpFailure::NotApproved, "no approval");

    cardspec::HttpPolicy noUrl = base;
    noUrl.url.clear();
    ExpectDenied("demo", noUrl, cardspec::HttpFailure::NotConfigured, "no configured url");

    cardspec::HttpPolicy otherConfigured = base;
    otherConfigured.profile = "other";
    ExpectDenied("demo", otherConfigured, cardspec::HttpFailure::NotConfigured,
                 "configured profile differs");

    cardspec::HttpPolicy invalidConfigured = base;
    invalidConfigured.url = L"http://api.github.com/repos/x";
    ExpectDenied("demo", invalidConfigured, cardspec::HttpFailure::InvalidUrl,
                 "configured url not https");

    cardspec::HttpPolicy invalidApproved = base;
    invalidApproved.approvedUrl = L"https://api.github.com:8443/repos/microsoft/Windows-classic-samples";
    ExpectDenied("demo", invalidApproved, cardspec::HttpFailure::InvalidUrl,
                 "approved url has port");

    ExpectDenied("", base, cardspec::HttpFailure::NotConfigured, "empty profile");
    ExpectDenied("bad profile", base, cardspec::HttpFailure::NotConfigured, "space in profile");
    ExpectDenied("a@b", base, cardspec::HttpFailure::NotConfigured, "at in profile");
    ExpectDenied(std::string(33, 'a'), base, cardspec::HttpFailure::NotConfigured,
                 "33-char profile");
    ExpectAuthorized(std::string(32, 'a'), [] {
        cardspec::HttpPolicy p = ApprovedPolicy();
        p.profile = std::string(32, 'a');
        p.approvedProfile = p.profile;
        return p;
    }(), "32-char profile");
}

// ---------------------------------------------------------------------------
// Error text
// ---------------------------------------------------------------------------

void TestFailureText() {
    const cardspec::HttpFailure all[] = {
        cardspec::HttpFailure::None,       cardspec::HttpFailure::NotConfigured,
        cardspec::HttpFailure::NotApproved, cardspec::HttpFailure::InvalidUrl,
        cardspec::HttpFailure::Dns,        cardspec::HttpFailure::AddressBlocked,
        cardspec::HttpFailure::Timeout,    cardspec::HttpFailure::UnsupportedSecurity,
        cardspec::HttpFailure::Transport,  cardspec::HttpFailure::Tls,
        cardspec::HttpFailure::PeerMismatch, cardspec::HttpFailure::Redirect,
        cardspec::HttpFailure::HttpStatus, cardspec::HttpFailure::ContentType,
        cardspec::HttpFailure::ContentEncoding, cardspec::HttpFailure::TooLarge,
    };
    for (const cardspec::HttpFailure failure : all) {
        const std::wstring text = cardspec::HttpFailureText(failure);
        Check(!text.empty(), "failure text non-empty");
        Check(text.find(L"://") == std::wstring::npos, "failure text has no url");
        Check(text.find(L"github") == std::wstring::npos, "failure text has no host");
        Check(text.find(L"http") == std::wstring::npos, "failure text has no scheme");
    }
    Check(cardspec::HttpFailureText(cardspec::HttpFailure::InvalidUrl) !=
              cardspec::HttpFailureText(cardspec::HttpFailure::AddressBlocked),
          "failure texts are distinct");
}

// ---------------------------------------------------------------------------
// FetchHttp fake matrix
// ---------------------------------------------------------------------------

struct Fake {
    int resolves = 0;
    int exchanges = 0;
    std::wstring seenHost;
    unsigned seenResolveTimeout = 0;
    cardspec::HttpEndpoint seenEndpoint;
    cardspec::IPv4 seenPin{};
    unsigned seenWireTimeout = 0;
    cardspec::ResolveResult resolveResult;
    cardspec::WireResult wireResult;
    std::vector<std::uint64_t> times;
    std::size_t timeIndex = 0;
    bool throwResolve = false;
    bool throwExchange = false;
    bool throwNow = false;

    cardspec::HttpDependencies Dependencies() {
        cardspec::HttpDependencies deps;
        deps.resolve = [this](const std::wstring& host, unsigned timeoutMs) {
            ++resolves;
            ++fakeResolves;
            seenHost = host;
            seenResolveTimeout = timeoutMs;
            if (throwResolve) throw std::runtime_error("resolve");
            return resolveResult;
        };
        deps.exchange = [this](const cardspec::WireRequest& request) {
            ++exchanges;
            ++fakeExchanges;
            seenEndpoint = request.endpoint;
            seenPin = request.address;
            seenWireTimeout = request.timeoutMs;
            if (throwExchange) throw std::runtime_error("exchange");
            return wireResult;
        };
        deps.nowMs = [this]() -> std::uint64_t {
            if (throwNow) throw std::runtime_error("clock");
            if (timeIndex < times.size()) return times[timeIndex++];
            return times.empty() ? std::uint64_t{0} : times.back();
        };
        return deps;
    }
};

cardspec::ResolveResult ResolveOk(std::vector<cardspec::IPv4> addresses) {
    cardspec::ResolveResult resolved;
    resolved.failure = cardspec::HttpFailure::None;
    resolved.addresses = std::move(addresses);
    return resolved;
}

cardspec::WireResult WireOk(const cardspec::IPv4& peer) {
    cardspec::WireResult wire;
    wire.failure = cardspec::HttpFailure::None;
    wire.status = 200;
    wire.peerVerified = true;
    wire.peer = peer;
    wire.contentType = "application/json";
    wire.body = "{}";
    return wire;
}

// One fully successful fake fetch; callers mutate the pieces they test.
cardspec::HttpResult FetchOnce(Fake& fake, std::string_view profile = "demo") {
    fake.resolveResult = ResolveOk({kPublic1, kPublic2});
    fake.wireResult = WireOk(kPublic1);
    if (fake.times.empty()) fake.times = {1000, 1000, 1000};
    return cardspec::FetchHttp(profile, ApprovedPolicy(), fake.Dependencies());
}

void TestFetchRequiresAuthorization() {
    Fake fake;
    cardspec::HttpPolicy policy = ApprovedPolicy();
    policy.approvedUrl.clear();
    policy.approvedProfile.clear();
    const cardspec::HttpResult refused = cardspec::FetchHttp("demo", policy, fake.Dependencies());
    Check(refused.failure == cardspec::HttpFailure::NotApproved, "fetch needs approval");
    Check(fake.resolves == 0, "no approval means no dns");
    Check(fake.exchanges == 0, "no approval means no exchange");
    Check(refused.body.empty(), "no approval means no body");

    Fake other;
    const cardspec::HttpResult wrongProfile =
        cardspec::FetchHttp("nope", ApprovedPolicy(), other.Dependencies());
    Check(wrongProfile.failure == cardspec::HttpFailure::NotConfigured, "fetch profile mismatch");
    Check(other.resolves == 0 && other.exchanges == 0, "profile mismatch is offline");

    Fake unused;
    const cardspec::HttpDependencies none;
    const cardspec::HttpResult missing = cardspec::FetchHttp("demo", ApprovedPolicy(), none);
    Check(missing.failure == cardspec::HttpFailure::NotConfigured, "missing dependencies");
    Check(unused.resolves == 0 && unused.exchanges == 0, "missing dependencies are offline");

    Fake throwingPolicy;
    cardspec::HttpPolicy invalid = ApprovedPolicy();
    invalid.url = L"http://api.github.com/";
    const cardspec::HttpResult invalidResult =
        cardspec::FetchHttp("demo", invalid, throwingPolicy.Dependencies());
    Check(invalidResult.failure == cardspec::HttpFailure::InvalidUrl, "invalid url fails closed");
    Check(throwingPolicy.resolves == 0, "invalid url is offline");
}

void TestFetchAddressPolicy() {
    Fake privateOnly;
    privateOnly.resolveResult = ResolveOk({Addr(10, 0, 0, 5)});
    privateOnly.times = {1000, 1000, 1000};
    const cardspec::HttpResult blocked =
        cardspec::FetchHttp("demo", ApprovedPolicy(), privateOnly.Dependencies());
    Check(blocked.failure == cardspec::HttpFailure::AddressBlocked, "private answer blocked");
    Check(privateOnly.exchanges == 0, "private answer never reaches exchange");

    Fake mixed;
    mixed.resolveResult = ResolveOk({kPublic1, Addr(192, 168, 1, 1)});
    mixed.times = {1000, 1000, 1000};
    const cardspec::HttpResult mixedResult =
        cardspec::FetchHttp("demo", ApprovedPolicy(), mixed.Dependencies());
    Check(mixedResult.failure == cardspec::HttpFailure::AddressBlocked,
          "mixed answer rejected as a whole");
    Check(mixed.exchanges == 0, "mixed answer never reaches exchange");

    const cardspec::IPv4 blockedRanges[] = {
        Addr(0, 0, 0, 0),       Addr(10, 1, 2, 3),      Addr(100, 64, 0, 1),
        Addr(127, 0, 0, 1),     Addr(169, 254, 1, 1),    Addr(172, 16, 0, 1),
        Addr(192, 0, 0, 1),     Addr(192, 0, 2, 1),      Addr(192, 88, 99, 1),
        Addr(192, 168, 1, 1),   Addr(198, 18, 0, 1),     Addr(198, 51, 100, 1),
        Addr(203, 0, 113, 1),   Addr(224, 0, 0, 1),      Addr(255, 255, 255, 255),
    };
    for (const cardspec::IPv4& address : blockedRanges) {
        Fake fake;
        fake.resolveResult = ResolveOk({address});
        fake.times = {1000, 1000, 1000};
        const cardspec::HttpResult result =
            cardspec::FetchHttp("demo", ApprovedPolicy(), fake.Dependencies());
        Check(result.failure == cardspec::HttpFailure::AddressBlocked,
              "blocked range never resolved further");
        Check(fake.exchanges == 0, "blocked range sends nothing");
    }

    Fake resolverFailure;
    resolverFailure.resolveResult.failure = cardspec::HttpFailure::Dns;
    resolverFailure.resolveResult.platformError = 11001;
    resolverFailure.times = {1000};
    const cardspec::HttpResult dnsFailure =
        cardspec::FetchHttp("demo", ApprovedPolicy(), resolverFailure.Dependencies());
    Check(dnsFailure.failure == cardspec::HttpFailure::Dns, "resolve failure propagates");
    Check(dnsFailure.platformError == 11001, "resolve platform error propagates");
    Check(resolverFailure.exchanges == 0, "resolve failure sends nothing");

    Fake resolveTimeout;
    resolveTimeout.resolveResult.failure = cardspec::HttpFailure::Timeout;
    resolveTimeout.times = {1000};
    const cardspec::HttpResult timeout =
        cardspec::FetchHttp("demo", ApprovedPolicy(), resolveTimeout.Dependencies());
    Check(timeout.failure == cardspec::HttpFailure::Timeout, "resolve timeout propagates");
    Check(resolveTimeout.exchanges == 0, "resolve timeout sends nothing");

    Fake emptyAnswer;
    emptyAnswer.resolveResult = ResolveOk({});
    emptyAnswer.times = {1000};
    const cardspec::HttpResult empty =
        cardspec::FetchHttp("demo", ApprovedPolicy(), emptyAnswer.Dependencies());
    Check(empty.failure == cardspec::HttpFailure::Dns, "empty answer is dns failure");
    Check(emptyAnswer.exchanges == 0, "empty answer sends nothing");
}

void TestFetchBudget() {
    Fake success;
    const cardspec::HttpResult ok = FetchOnce(success);
    Check(ok.ok(), "fake success");
    Check(success.resolves == 1, "exactly one resolve");
    Check(success.exchanges == 1, "exactly one exchange");
    Check(success.seenHost == L"api.github.com", "resolve sees canonical host");
    Check(success.seenResolveTimeout == 2000, "dns sub-budget is 2s");
    Check(success.seenWireTimeout == 10000, "exchange gets the full budget");
    Check(success.seenPin == kPublic1, "first verified address is pinned");
    // The wire request must be the trusted policy endpoint itself, not a path or
    // host supplied anywhere else (the spec only carries a profile).
    Check(success.seenEndpoint.host == L"api.github.com", "wire host comes from policy");
    Check(success.seenEndpoint.target == L"/repos/microsoft/Windows-classic-samples",
          "wire target comes from policy");
    Check(success.seenEndpoint.url ==
              L"https://api.github.com/repos/microsoft/Windows-classic-samples",
          "wire url is the canonical policy endpoint");

    Fake afterResolve;
    afterResolve.resolveResult = ResolveOk({kPublic1});
    afterResolve.wireResult = WireOk(kPublic1);
    afterResolve.times = {1000, 3500, 3500};
    const cardspec::HttpResult spent =
        cardspec::FetchHttp("demo", ApprovedPolicy(), afterResolve.Dependencies());
    Check(spent.ok(), "remaining budget success");
    Check(afterResolve.seenWireTimeout == 7500, "exchange gets the remaining budget");

    Fake exhausted;
    exhausted.resolveResult = ResolveOk({kPublic1});
    exhausted.wireResult = WireOk(kPublic1);
    exhausted.times = {1000, 11000};
    const cardspec::HttpResult noBudget =
        cardspec::FetchHttp("demo", ApprovedPolicy(), exhausted.Dependencies());
    Check(noBudget.failure == cardspec::HttpFailure::Timeout, "exhausted budget times out");
    Check(exhausted.exchanges == 0, "exhausted budget sends nothing");

    Fake overrun;
    overrun.resolveResult = ResolveOk({kPublic1});
    overrun.wireResult = WireOk(kPublic1);
    overrun.wireResult.body = "leak";
    overrun.times = {1000, 1000, 12000};
    const cardspec::HttpResult late =
        cardspec::FetchHttp("demo", ApprovedPolicy(), overrun.Dependencies());
    Check(late.failure == cardspec::HttpFailure::Timeout, "overrun after exchange times out");
    Check(late.body.empty(), "overrun drops the body");

    Fake backwards;
    backwards.resolveResult = ResolveOk({kPublic1});
    backwards.times = {5000, 4000};
    const cardspec::HttpResult regressed =
        cardspec::FetchHttp("demo", ApprovedPolicy(), backwards.Dependencies());
    Check(regressed.failure == cardspec::HttpFailure::Timeout, "clock regression fails closed");
    Check(backwards.exchanges == 0, "clock regression sends nothing");

    Fake backwardsLate;
    backwardsLate.resolveResult = ResolveOk({kPublic1});
    backwardsLate.wireResult = WireOk(kPublic1);
    backwardsLate.wireResult.body = "leak";
    backwardsLate.times = {1000, 1000, 900};
    const cardspec::HttpResult lateRegression =
        cardspec::FetchHttp("demo", ApprovedPolicy(), backwardsLate.Dependencies());
    Check(lateRegression.failure == cardspec::HttpFailure::Timeout,
          "late clock regression fails closed");
    Check(lateRegression.body.empty(), "late clock regression drops the body");
}

void TestFetchWirePolicy() {
    const cardspec::HttpFailure forwarded[] = {
        cardspec::HttpFailure::UnsupportedSecurity, cardspec::HttpFailure::Tls,
        cardspec::HttpFailure::Transport,           cardspec::HttpFailure::Dns,
        cardspec::HttpFailure::Timeout,             cardspec::HttpFailure::PeerMismatch,
        cardspec::HttpFailure::Redirect,            cardspec::HttpFailure::ContentType,
        cardspec::HttpFailure::ContentEncoding,     cardspec::HttpFailure::TooLarge,
    };
    for (const cardspec::HttpFailure failure : forwarded) {
        Fake fake;
        fake.resolveResult = ResolveOk({kPublic1});
        fake.wireResult = WireOk(kPublic1);
        fake.wireResult.failure = failure;
        fake.wireResult.body = "leak";
        fake.times = {1000, 1000, 1000};
        const cardspec::HttpResult result =
            cardspec::FetchHttp("demo", ApprovedPolicy(), fake.Dependencies());
        Check(result.failure == failure, "exchange failure propagates");
        Check(result.body.empty(), "exchange failure drops the body");
        Check(fake.exchanges == 1, "exchange failure is not retried");
    }

    Fake unverified;
    unverified.resolveResult = ResolveOk({kPublic1});
    unverified.wireResult = WireOk(kPublic1);
    unverified.wireResult.peerVerified = false;
    unverified.wireResult.body = "leak";
    unverified.times = {1000, 1000, 1000};
    const cardspec::HttpResult unverifiedResult =
        cardspec::FetchHttp("demo", ApprovedPolicy(), unverified.Dependencies());
    Check(unverifiedResult.failure == cardspec::HttpFailure::PeerMismatch,
          "peer must be verified");
    Check(unverifiedResult.body.empty(), "unverified peer drops the body");

    Fake otherPeer;
    otherPeer.resolveResult = ResolveOk({kPublic1});
    otherPeer.wireResult = WireOk(kPublic2);
    otherPeer.wireResult.body = "leak";
    otherPeer.times = {1000, 1000, 1000};
    const cardspec::HttpResult mismatch =
        cardspec::FetchHttp("demo", ApprovedPolicy(), otherPeer.Dependencies());
    Check(mismatch.failure == cardspec::HttpFailure::PeerMismatch, "peer must equal the pin");
    Check(mismatch.body.empty(), "peer mismatch drops the body");

    const unsigned redirects[] = {301, 302, 303, 307, 308};
    for (const unsigned status : redirects) {
        Fake fake;
        fake.resolveResult = ResolveOk({kPublic1});
        fake.wireResult = WireOk(kPublic1);
        fake.wireResult.status = status;
        fake.wireResult.body = "leak";
        fake.times = {1000, 1000, 1000};
        const cardspec::HttpResult result =
            cardspec::FetchHttp("demo", ApprovedPolicy(), fake.Dependencies());
        Check(result.failure == cardspec::HttpFailure::Redirect, "redirect refused");
        Check(result.body.empty(), "redirect drops the body");
    }

    const unsigned badStatus[] = {400, 401, 403, 404, 407, 418, 500, 503};
    for (const unsigned status : badStatus) {
        Fake fake;
        fake.resolveResult = ResolveOk({kPublic1});
        fake.wireResult = WireOk(kPublic1);
        fake.wireResult.status = status;
        fake.wireResult.body = "leak";
        fake.times = {1000, 1000, 1000};
        const cardspec::HttpResult result =
            cardspec::FetchHttp("demo", ApprovedPolicy(), fake.Dependencies());
        Check(result.failure == cardspec::HttpFailure::HttpStatus, "non-200 refused");
        Check(result.body.empty(), "non-200 drops the body");
    }
}

void TestFetchPayloadPolicy() {
    const char* badTypes[] = {"", "text/html", "text/json", "application/jsonx",
                              "application/json; charset=utf-16", "application/xml"};
    for (const char* type : badTypes) {
        Fake fake;
        fake.resolveResult = ResolveOk({kPublic1});
        fake.wireResult = WireOk(kPublic1);
        fake.wireResult.contentType = type;
        fake.wireResult.body = "leak";
        fake.times = {1000, 1000, 1000};
        const cardspec::HttpResult result =
            cardspec::FetchHttp("demo", ApprovedPolicy(), fake.Dependencies());
        Check(result.failure == cardspec::HttpFailure::ContentType, "non-json mime refused");
        Check(result.body.empty(), "non-json mime drops the body");
    }

    const char* goodTypes[] = {"application/json",
                               "application/json; charset=utf-8",
                               "Application/JSON;charset=UTF-8",
                               "application/json; charset=\"utf-8\"",
                               " application/json "};
    for (const char* type : goodTypes) {
        Fake fake;
        fake.resolveResult = ResolveOk({kPublic1});
        fake.wireResult = WireOk(kPublic1);
        fake.wireResult.contentType = type;
        fake.times = {1000, 1000, 1000};
        const cardspec::HttpResult result =
            cardspec::FetchHttp("demo", ApprovedPolicy(), fake.Dependencies());
        Check(result.ok(), "json mime accepted");
    }

    const char* badEncodings[] = {"gzip", "br", "deflate", "identity,gzip"};
    for (const char* encoding : badEncodings) {
        Fake fake;
        fake.resolveResult = ResolveOk({kPublic1});
        fake.wireResult = WireOk(kPublic1);
        fake.wireResult.contentEncoding = encoding;
        fake.wireResult.body = "leak";
        fake.times = {1000, 1000, 1000};
        const cardspec::HttpResult result =
            cardspec::FetchHttp("demo", ApprovedPolicy(), fake.Dependencies());
        Check(result.failure == cardspec::HttpFailure::ContentEncoding, "encoding refused");
        Check(result.body.empty(), "encoding refuses the body");
    }

    const char* goodEncodings[] = {"", "identity", " Identity "};
    for (const char* encoding : goodEncodings) {
        Fake fake;
        fake.resolveResult = ResolveOk({kPublic1});
        fake.wireResult = WireOk(kPublic1);
        fake.wireResult.contentEncoding = encoding;
        fake.times = {1000, 1000, 1000};
        const cardspec::HttpResult result =
            cardspec::FetchHttp("demo", ApprovedPolicy(), fake.Dependencies());
        Check(result.ok(), "identity encoding accepted");
    }

    Fake overLimit;
    overLimit.resolveResult = ResolveOk({kPublic1});
    overLimit.wireResult = WireOk(kPublic1);
    overLimit.wireResult.body.assign(static_cast<std::size_t>(cardspec::kMaxHttpBodyBytes) + 1, 'a');
    overLimit.times = {1000, 1000, 1000};
    const cardspec::HttpResult tooLarge =
        cardspec::FetchHttp("demo", ApprovedPolicy(), overLimit.Dependencies());
    Check(tooLarge.failure == cardspec::HttpFailure::TooLarge, "oversized body refused");
    Check(tooLarge.body.empty(), "oversized body cleared");

    Fake atLimit;
    atLimit.resolveResult = ResolveOk({kPublic1});
    atLimit.wireResult = WireOk(kPublic1);
    atLimit.wireResult.body.assign(cardspec::kMaxHttpBodyBytes, 'a');
    atLimit.times = {1000, 1000, 1000};
    const cardspec::HttpResult boundary =
        cardspec::FetchHttp("demo", ApprovedPolicy(), atLimit.Dependencies());
    Check(boundary.ok(), "32 KiB body accepted");
    Check(boundary.body.size() == cardspec::kMaxHttpBodyBytes, "32 KiB body kept whole");
}

void TestFetchExceptions() {
    {
        Fake fake;
        fake.throwResolve = true;
        const cardspec::HttpResult result =
            cardspec::FetchHttp("demo", ApprovedPolicy(), fake.Dependencies());
        Check(result.failure == cardspec::HttpFailure::Transport, "resolver throw fails closed");
        Check(result.body.empty(), "resolver throw has no body");
        Check(fake.exchanges == 0, "resolver throw sends nothing");
    }
    {
        Fake fake;
        fake.resolveResult = ResolveOk({kPublic1});
        fake.throwExchange = true;
        fake.times = {1000, 1000};
        const cardspec::HttpResult result =
            cardspec::FetchHttp("demo", ApprovedPolicy(), fake.Dependencies());
        Check(result.failure == cardspec::HttpFailure::Transport, "exchange throw fails closed");
        Check(result.body.empty(), "exchange throw has no body");
    }
    {
        Fake fake;
        fake.throwNow = true;
        const cardspec::HttpResult result =
            cardspec::FetchHttp("demo", ApprovedPolicy(), fake.Dependencies());
        Check(result.failure == cardspec::HttpFailure::Transport, "clock throw fails closed");
        Check(fake.resolves == 0, "clock throw happens before dns");
    }
}

void TestDefaultDependencies() {
    // Construction must stay I/O free: only check that the callbacks exist.
    const cardspec::HttpDependencies dependencies = cardspec::DefaultHttpDependencies();
    Check(static_cast<bool>(dependencies.resolve), "default resolve present");
    Check(static_cast<bool>(dependencies.exchange), "default exchange present");
    Check(static_cast<bool>(dependencies.nowMs), "default clock present");
}

// ---------------------------------------------------------------------------
// Optional fixed live probe (never run by the default test)
// ---------------------------------------------------------------------------

int RunLiveProbe() {
    // Hard-coded public endpoint. No command-line URL, no configuration file,
    // no credentials, no environment inspection.
    constexpr const wchar_t* kProbeUrl =
        L"https://api.github.com/repos/microsoft/Windows-classic-samples";

    cardspec::HttpPolicy policy;
    policy.profile = "m2-live";
    policy.url = kProbeUrl;
    policy.approvedProfile = "m2-live";
    policy.approvedUrl = kProbeUrl;

    const cardspec::HttpResult result =
        cardspec::FetchHttp("m2-live", policy, cardspec::DefaultHttpDependencies());
    if (!result.ok()) {
        std::printf("live-probe: failure=%d status=%u\n", static_cast<int>(result.failure),
                    result.status);
        return 1;
    }

    assistant::JsonValue json;
    std::wstring error;
    if (!cardspec::ParseStrictJson(result.body, &json, &error) || !json.IsObject()) {
        std::printf("live-probe: status=200 strict_json=rejected\n");
        return 1;
    }
    const bool hasName =
        json["full_name"].IsString() && !json["full_name"].String().empty();
    const bool hasStars = json["stargazers_count"].IsNumber();
    std::printf("live-probe: status=200 strict_json=object full_name=%s stargazers_count=%s\n",
                hasName ? "present" : "missing", hasStars ? "present" : "missing");
    return (hasName && hasStars) ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1) {
        if (argc == 2 && std::wcscmp(argv[1], L"--live-probe") == 0) return RunLiveProbe();
        std::printf("[cardspec-http-test] unknown argument rejected\n");
        return 2;
    }

    TestEndpointRules();
    TestPublicAddresses();
    TestAuthorization();
    TestFailureText();
    TestFetchRequiresAuthorization();
    TestFetchAddressPolicy();
    TestFetchBudget();
    TestFetchWirePolicy();
    TestFetchPayloadPolicy();
    TestFetchExceptions();
    TestDefaultDependencies();

    std::printf("[cardspec-http-test] %s checks=%d fake-resolve=%d fake-exchange=%d\n",
                failures == 0 ? "ok" : "failed", checks, fakeResolves, fakeExchanges);
    return failures == 0 ? 0 : 1;
}
