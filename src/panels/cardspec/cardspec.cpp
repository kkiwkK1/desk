// CARDSPEC card (M2.1): one fixed, declarative card rendered from
// <config dir>\cardspec.json.
//
// The spec is the only input surface and it can never grant itself anything:
// a v1 inline document is rendered locally exactly as in M2.0, while a v2 HTTP
// document only names a trusted config profile. Network access needs a URL the
// *user* configured in cardspec.ini plus an explicit approval saved by the card
// menu; the initial Load never touches the network and no code path fetches
// without an injected transport.
//
// Refresh is still manual (refreshSeconds = 0, no timer, no thread, no retry,
// no cache): after a module menu action the host runs exactly one worker, and
// the menu callbacks below never call RequestRefresh() themselves - a second
// request would be posted while the first worker is still busy, so the board
// would set its `again` flag and fetch twice per click. A successful fetch
// publishes rows plus an "AS OF" caption; any failure replaces the previous
// values instead of faking fresh data.
//
// Permission epochs close the whole publication chain, not just the transport:
// every HTTP view records the epoch it was produced under, the read re-checks
// that epoch after the transport and again after decode/binding, and
// BodyHeightOf/PaintBodyOf/Status compare it once more against the live epoch.
// So a revoke that lands after the worker returned - even after ModuleT::Commit
// swapped the stale view into shown, which is `final` and cannot be intercepted
// - still measures, reports and paints as an expired Blocked card with no old
// values. Only a new authorized fetch can produce a new publication. The packet
// already in flight cannot be recalled.
//
// LoadData / RefreshData fully replace the staged view, so a failure can never
// publish stale successful data, and PaintBody only reads the host's shown
// snapshot (ModuleT staging/shown split).
#include "panels/cardspec/cardspec.h"

#include "panels/cardspec/cardspec_view.h"

#include "widget/module.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Trusted user configuration. Only ASCII profiles exist; anything else can
// never match and is therefore treated as "not configured".
std::string NarrowAscii(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (const wchar_t c : text) {
        if (c < 0x20 || c > 0x7E) continue;
        out.push_back((char)c);
    }
    return out;
}

std::wstring WideAscii(const std::string& text) {
    std::wstring out;
    out.reserve(text.size());
    for (const char c : text) out.push_back((wchar_t)(unsigned char)c);
    return out;
}

cardspec::HttpPolicy PolicyFrom(const std::map<std::wstring, std::wstring>& values) {
    const auto find = [&values](const wchar_t* key) {
        const auto it = values.find(key);
        return it == values.end() ? std::wstring() : it->second;
    };
    cardspec::HttpPolicy policy;
    policy.profile = NarrowAscii(find(L"http.profile"));
    policy.url = find(L"http.url");
    policy.approvedProfile = NarrowAscii(find(L"http.approved_profile"));
    policy.approvedUrl = find(L"http.approved_url");
    return policy;
}

// Production transport. DefaultHttpDependencies() constructs callbacks without
// doing I/O; only FetchHttp (called from the worker, never from Load) can talk
// to the network.
cardspec::HttpFetcher ProductionFetcher() {
    auto dependencies =
        std::make_shared<cardspec::HttpDependencies>(cardspec::DefaultHttpDependencies());
    return [dependencies](std::string_view profile, const cardspec::HttpPolicy& policy) {
        return cardspec::FetchHttp(profile, policy, *dependencies);
    };
}

// Only a real menu click reaches this; a native test injects its own callback
// and no window is created.
bool DefaultConfirm(const std::wstring& message) {
    return MessageBoxW(nullptr, message.c_str(), L"CARDSPEC · 网络授权",
                       MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
}

class CardSpecModule final : public wg::ModuleT<cardspec_view::ViewState> {
public:
    explicit CardSpecModule(CardSpecOptions options)
        : fetch_(std::move(options.fetch)), confirm_(std::move(options.confirmNetwork)) {}

    wg::ModuleSpec Describe() const override {
        wg::ModuleSpec spec;
        spec.id = L"cardspec";
        spec.title = L"CARDSPEC";
        // Store file for host state and the trusted http.* keys. The JSON spec
        // is the sibling fixed file and is never handed to wg::Store.
        spec.configFile = cardspec_view::kConfigFileName;
        spec.sizePreset = wg::SizePreset::Wide;
        spec.refreshSeconds = 0;
        spec.hasDetails = false;
        spec.visibleByDefault = false;
        return spec;
    }

    std::vector<wg::MenuItem> Menu() const override {
        // v1 keeps exactly one manual entry.
        std::vector<wg::MenuItem> menu{wg::MenuItem{L"Reload cardspec.json", 1}};
        if (!shown().isHttp) return menu;
        // Approve/Revoke describe the *shown* request, matched against the
        // trusted config cached by the last load, so the menu never guesses.
        if (ApprovedForShown()) {
            menu.push_back(wg::MenuItem{L"Revoke HTTP approval", 3});
        } else {
            menu.push_back(wg::MenuItem{L"Approve HTTP endpoint...", 2});
        }
        return menu;
    }

    void OnMenu(int id) override {
        // The host starts exactly one refresh after a module menu action
        // (board: OnMenu -> StartRefresh), so nothing here may call
        // RequestRefresh(): that would post a second refresh while the first
        // worker is still busy and the card would fetch twice per click.
        switch (id) {
        case 1:  // Reload: the host's post-menu refresh re-reads the spec.
            return;
        case 2: Approve(); return;
        case 3: Revoke(); return;
        default: return;
        }
    }

protected:
    // Initial Load: local spec + local policy only. allowNetwork is false even
    // when a previous approval is on disk, so startup never opens a socket.
    void LoadData(cardspec_view::ViewState& state) override { state = ReadState(false); }

    // Worker thread: the only place a fetch may happen, and only for an
    // authorized HTTP source.
    void RefreshData(cardspec_view::ViewState& state) override { state = ReadState(true); }

    int BodyHeightOf(const cardspec_view::ViewState& state) const override {
        cardspec_view::ViewState expired;
        if (ExpiredForDisplay(state, &expired)) return cardspec_view::BodyHeight(expired);
        return cardspec_view::BodyHeight(state);
    }

    void PaintBodyOf(const cardspec_view::ViewState& state, wg::Canvas& canvas,
                     const wg::Rect& body) override {
        cardspec_view::ViewState expired;
        if (ExpiredForDisplay(state, &expired)) {
            cardspec_view::DrawBody(expired, canvas, body, theme_);
            return;
        }
        cardspec_view::DrawBody(state, canvas, body, theme_);
    }

    wg::StatusChip Status() const override {
        cardspec_view::ViewState expired;
        if (ExpiredForDisplay(shown(), &expired)) {
            return cardspec_view::Status(expired, theme_);
        }
        return cardspec_view::Status(shown(), theme_);
    }

private:
    std::uint64_t CurrentEpoch() const {
        std::lock_guard<std::mutex> lock(mu_);
        return epoch_;
    }

    // A view is display-expired when its HTTP permission epoch is behind the
    // current one. Such a view is never measured, reported or painted with its
    // old values - only as a Blocked/expired card.
    bool ExpiredForDisplay(const cardspec_view::ViewState& state,
                           cardspec_view::ViewState* expired) const {
        if (!state.isHttp) return false;
        const std::uint64_t now = CurrentEpoch();
        if (state.publicationEpoch == now) return false;
        *expired = cardspec_view::ExpiredHttpView(state);
        expired->publicationEpoch = now;
        return true;
    }

    // The Blocked view published when a revoke/approval invalidated a result
    // that was already returning. It carries no rows and no as-of caption.
    static cardspec_view::ViewState DiscardedView(const cardspec_view::ViewState& stale,
                                                  std::uint64_t epoch) {
        cardspec_view::ViewState out = cardspec_view::ExpiredHttpView(stale);
        out.publicationEpoch = epoch;
        return out;
    }

    bool ApprovedForShown() const {
        std::lock_guard<std::mutex> lock(mu_);
        const std::string profile = shown().request.profile;
        if (profile.empty() || !cacheValid_) return false;
        return cache_.profile == profile && cache_.approvedProfile == profile &&
               !cache_.approvedUrl.empty();
    }

    // Consistent copy of the trusted keys for one load/approval decision.
    cardspec::HttpPolicy PolicySnapshot() {
        std::lock_guard<std::mutex> lock(mu_);
        return PolicyFrom(store().Snapshot());
    }

    void RememberPolicy(const cardspec::HttpPolicy& policy) {
        std::lock_guard<std::mutex> lock(mu_);
        cache_ = policy;
        cacheValid_ = true;
    }

    void Note(const std::wstring& text) {
        std::lock_guard<std::mutex> lock(mu_);
        note_ = text;
    }

    std::wstring TakeNote() {
        std::lock_guard<std::mutex> lock(mu_);
        std::wstring note = std::move(note_);
        note_.clear();
        return note;
    }

    cardspec_view::ViewState ReadState(bool allowNetwork) {
        cardspec_view::LoadOptions options;
        options.allowNetwork = allowNetwork;
        options.policy = PolicySnapshot();

        // The permission epoch is captured before the read and re-checked after
        // the transport, after decode/validation, and then again on every
        // display call. The packet itself cannot be recalled once sent, but a
        // result produced under a superseded epoch is never published or shown.
        const std::uint64_t captured = CurrentEpoch();
        struct EpochGuard {
            std::uint64_t epoch = 0;
            bool discarded = false;
        };
        auto guard = std::make_shared<EpochGuard>();
        guard->epoch = captured;

        if (fetch_) {
            options.fetch = [this, guard](std::string_view profile,
                                          const cardspec::HttpPolicy& policy) {
                // Never hold mu_ across the transport: a UI revoke (or a fake
                // transport calling OnMenu) must not deadlock with it. The
                // entry check avoids sending an obviously revoked request; the
                // exit check marks the result as unusable.
                if (CurrentEpoch() != guard->epoch) {
                    guard->discarded = true;
                    return cardspec::HttpResult{};
                }
                cardspec::HttpResult result = fetch_(profile, policy);
                if (CurrentEpoch() != guard->epoch) guard->discarded = true;
                return result;
            };
        }

        cardspec_view::ViewState state =
            cardspec_view::LoadSpecFile(cardspec_view::SpecPathFor(store().path()), options);

        // Menu() is const and must not read the store: cache what this load saw.
        RememberPolicy(PolicySnapshot());

        // Re-check after the whole read: a revoke that landed during decode or
        // binding must not publish this document's rows either.
        const std::uint64_t current = CurrentEpoch();
        if (state.isHttp && (guard->discarded || current != captured)) {
            return DiscardedView(state, current);
        }

        const std::wstring note = TakeNote();
        if (!note.empty()) {
            if (state.detail.empty()) state.detail = note;
            else state.detail += L"；" + note;
        }

        // Stamp the epoch this view was *produced under* (not the newest one):
        // BodyHeightOf/PaintBodyOf/Status compare it once more, so a revoke
        // between this return and the host's Commit still cannot show the old
        // result - not even for one committed frame.
        state.publicationEpoch = captured;
        return state;
    }

    void Approve() {
        const std::string profile = shown().request.profile;
        const cardspec::HttpPolicy policy = PolicySnapshot();

        // Pure candidate grant: AuthorizeHttp validates the trusted URL and the
        // profile/exact-URL binding. No DNS, no store write, no fetch here.
        cardspec::HttpPolicy candidate = policy;
        candidate.approvedProfile = policy.profile;
        candidate.approvedUrl = policy.url;
        cardspec::HttpEndpoint endpoint;
        cardspec::HttpFailure failure = cardspec::HttpFailure::None;
        if (profile.empty() ||
            !cardspec::AuthorizeHttp(profile, candidate, &endpoint, &failure)) {
            // A broken config must surface as a readable blocked card, never as
            // a modal the user cannot control. The note is consumed by the
            // host's post-menu refresh, which is also the single read; asking
            // for another one here would fetch twice.
            Note(L"无法批准：端点配置不合法或未配置");
            return;
        }
        if (!confirm_) {
            Note(L"缺少确认入口，未批准");
            return;
        }
        const std::wstring prompt =
            L"批准该只读 HTTPS 端点？\n\n" + endpoint.url +
            L"\n\n仅 GET；无代理、无鉴权、无重定向；响应最多 32 KiB。";
        if (!confirm_(prompt)) return;  // cancel: zero store writes, zero fetch

        std::wstring note;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const std::wstring oldProfile = store().Get(L"http.approved_profile");
            const std::wstring oldUrl = store().Get(L"http.approved_url");
            store().Set(L"http.approved_profile", WideAscii(profile));
            store().Set(L"http.approved_url", endpoint.url);
            if (!store().Save()) {
                // A failed save must not leave the process authorized.
                store().Set(L"http.approved_profile", oldProfile);
                store().Set(L"http.approved_url", oldUrl);
                note = L"批准未生效：配置保存失败";
            } else {
                ++epoch_;  // any in-flight result is now stale
                cache_.profile = policy.profile;
                cache_.url = policy.url;
                cache_.approvedProfile = profile;
                cache_.approvedUrl = endpoint.url;
                cacheValid_ = true;
            }
        }
        if (!note.empty()) Note(note);
        // The permission epoch just moved: repaint so an already committed
        // result stops being shown. The host's post-menu refresh is the one and
        // only fetch this approval may cause (a saved approval is the only way
        // that refresh may fetch); requesting a refresh here as well would run
        // a second worker right after the first one.
        RequestRepaint();
    }

    void Revoke() {
        std::wstring note;
        {
            std::lock_guard<std::mutex> lock(mu_);
            store().Set(L"http.approved_profile", L"");
            store().Set(L"http.approved_url", L"");
            // Block this process immediately, even if the file write fails: a
            // revoke must never silently keep the network open.
            ++epoch_;
            cache_.approvedProfile.clear();
            cache_.approvedUrl.clear();
            if (!store().Save()) note = L"撤销已在本进程生效，但配置保存失败";
        }
        if (!note.empty()) Note(note);
        // Repaint on the UI thread (serialized with this menu action) so the
        // revoked result is never shown again. The host re-reads once after
        // dispatching this menu action; a RequestRefresh() here would schedule
        // a second worker behind the first one.
        RequestRepaint();
    }

    mutable std::mutex mu_;
    std::uint64_t epoch_ = 0;
    cardspec::HttpPolicy cache_;
    bool cacheValid_ = false;
    std::wstring note_;
    cardspec::HttpFetcher fetch_;
    std::function<bool(const std::wstring&)> confirm_;
    wg::Theme theme_;
};

} // namespace

std::unique_ptr<wg::Module> CreateCardSpecModule() {
    CardSpecOptions options;
    options.fetch = ProductionFetcher();
    options.confirmNetwork = DefaultConfirm;
    return CreateCardSpecModule(std::move(options));
}

std::unique_ptr<wg::Module> CreateCardSpecModule(CardSpecOptions options) {
    // The options overload never substitutes the real transport: an empty
    // fetch stays fail-closed and an empty confirm never opens a window.
    return std::make_unique<CardSpecModule>(std::move(options));
}
