#include "memory.h"

#include "data/assistant/memory.h"

#include "widget/cjk.h"
#include "widget/prompt.h"
#include "widget/size.h"
#include "widget/skin.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace memory_view {

using namespace wg;

namespace {

// Standard preset minimum/maximum agree with the constants in the header: a
// mismatch here would hand the board a body it then clips.
static_assert(kMinBody == 64, "Standard minimum body is 112 - 48 chrome");
static_assert(kMaxBody == 592, "Standard maximum body is 640 - 48 chrome");
static_assert(kTopInset + kExpandedMaxRows * kRowPitch + kMoreLineH + kErrorLineH +
                      kBottomPad <=
                  kMaxBody,
              "the expanded list (17 rows + MORE + error line) must fit Standard");
static_assert(kCollapsedRows * kRowPitch + kTopInset + kBottomPad >= kMinBody,
              "three collapsed rows must fill the Standard minimum");

const Color kSelectionFill = Rgba(0x60A5FA, 46);
const Color kProject = Rgba(0x22D3EE);
const Color kMilestone = Rgba(0xA78BFA);

std::wstring Upper(std::wstring text) {
    for (wchar_t& c : text) {
        if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
    }
    return text;
}

Color KindColor(const std::wstring& kind, const Theme& theme) {
    const std::wstring upper = Upper(kind);
    if (upper == L"GOAL" || upper == L"OBJECTIVE") return theme.ok;
    if (upper == L"PREF" || upper == L"PREFERENCE" || upper == L"PROFILE") {
        return Rgba(0x60A5FA);
    }
    if (upper == L"CONSTRAINT" || upper == L"RULE" || upper == L"LIMIT") return theme.warn;
    if (upper == L"PROJECT") return kProject;
    if (upper == L"MILESTONE" || upper == L"DECISION") return kMilestone;
    return theme.faint;
}

// "12M", "3H", "5D": short, ASCII and monotonic (the card never shows a
// fabricated date when the fact was written this session).
std::wstring AgeText(long long updatedMs, long long nowMs) {
    if (updatedMs <= 0) return L"NEW";
    const long long age = std::max(0LL, nowMs - updatedMs);
    const long long minutes = age / 60000;
    wchar_t buf[24];
    if (minutes < 60) {
        swprintf(buf, 24, L"%lldM", minutes);
    } else if (minutes < 60 * 24) {
        swprintf(buf, 24, L"%lldH", minutes / 60);
    } else {
        swprintf(buf, 24, L"%lldD", minutes / (60 * 24));
    }
    return buf;
}

long long NowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER v{};
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return (long long)(v.QuadPart / 10000ULL) - 11644473600000LL;
}

// Truncation by cjk metrics: fact text is usually Chinese and Canvas::TextWidth
// would count every glyph as 6px, letting the line run past the card edge.
std::wstring FitTo(std::wstring text, int maxWidth, float scale) {
    if (maxWidth <= 0) return std::wstring();
    if ((int)cjk::TextWidth(text, scale) <= maxWidth) return text;
    while (!text.empty() && (int)cjk::TextWidth(text + L"...", scale) > maxWidth) {
        text.pop_back();
    }
    return text + L"...";
}

// First line of a fact: kind tag + text. The tag carries a "*" when pinned so
// the state is visible without opening the menu.
void DrawFactRow(Canvas& canvas, const FactView& fact, int left, int right, int y,
                 bool selected, const Theme& theme) {
    if (selected) {
        canvas.Fill(left - 4, y - 3, right - left + 8, kRowH, kSelectionFill);
    }

    std::wstring kind = Upper(fact.kind.empty() ? std::wstring(L"FACT") : fact.kind);
    if (fact.pinned) kind.insert(kind.begin(), L'*');
    const Color kindColor = KindColor(fact.kind, theme);
    canvas.Text(kind, (float)left, (float)y, kindColor, kMetaScale);

    const int kindWidth = (int)canvas.TextWidth(kind, kMetaScale) + 8;
    const int textWidth = std::max(0, right - left - kindWidth);
    cjk::TextTop(canvas, FitTo(fact.text, textWidth, kMetaScale), (float)(left + kindWidth),
                 (float)y, theme.text, kMetaScale);

    // Second line: where the fact came from (left) and how old it is (right).
    const std::wstring source = fact.sourceRef.empty() ? L"NO SRC" : L"SRC " + fact.sourceRef;
    canvas.Text(FitTo(source, (right - left) / 2, kMetaScale), (float)left, (float)(y + 15),
                theme.faint, kMetaScale);
    canvas.TextRight(AgeText(fact.updatedMs > 0 ? fact.updatedMs : fact.createdMs, NowMs()),
                     right, (float)(y + 15), fact.pinned ? theme.warn : theme.faint,
                     kMetaScale);
}

void DrawEmpty(Canvas& canvas, const MemoryViewState& state, int left, int right, int top,
               const Theme& theme) {
    (void)right;
    if (!state.error.empty()) {
        canvas.Text(L"MEMORY ERROR", (float)left, (float)top, theme.bad, kMetaScale);
        cjk::TextTop(canvas, FitTo(state.error, kTextWidth, kMetaScale), (float)left,
                     (float)(top + 18), theme.bad, kMetaScale);
        return;
    }
    if (state.loading) {
        canvas.Text(L"LOADING FACTS...", (float)left, (float)top, theme.faint, kMetaScale);
        return;
    }
    canvas.Text(L"NO FACTS YET", (float)left, (float)top, theme.label, kMetaScale);
    canvas.Text(L"SAY \"REMEMBER: ...\" TO TEACH ME", (float)left, (float)(top + 18),
                theme.faint, kMetaScale);
    // The menu is where every mutating action lives; spelling it out keeps the
    // empty state from implying Pin/Delete are the only ones.
    canvas.Text(L"MENU: PIN / DELETE / EDIT / CLEAR ALL / REFRESH", (float)left,
                (float)(top + 36), theme.faint, kMetaScale);
}

} // namespace

int VisibleRows(const MemoryViewState& state, bool expanded) {
    const int count = (int)state.facts.size();
    return std::min(count, expanded ? kExpandedMaxRows : kCollapsedRows);
}

bool HasMore(const MemoryViewState& state, bool expanded) {
    return (int)state.facts.size() > VisibleRows(state, expanded);
}

int BodyHeight(const MemoryViewState& state, bool expanded) {
    if (state.facts.empty()) return kMinBody;
    // The error line is always part of the height, so a failed action can never
    // require a layout re-plan before it becomes visible.
    int height = kTopInset + VisibleRows(state, expanded) * kRowPitch + kErrorLineH +
                 kBottomPad;
    if (HasMore(state, expanded)) height += kMoreLineH;
    return std::clamp(height, kMinBody, kMaxBody);
}

int RowAt(const MemoryViewState& state, int y, bool expanded) {
    const int row = (y - kTopInset) / kRowPitch;
    if (y < kTopInset || row < 0 || row >= VisibleRows(state, expanded)) return -1;
    return row;
}

void DrawBody(const MemoryViewState& state, wg::Canvas& canvas, const wg::Rect& body,
              const wg::Theme& theme, bool expanded) {
    const int left = body.x + kPadX;
    const int right = body.right() - kPadX;
    const int top = body.y + kTopInset;

    if (state.facts.empty()) {
        DrawEmpty(canvas, state, left, right, top, theme);
        return;
    }

    const int rows = VisibleRows(state, expanded);
    for (int i = 0; i < rows; ++i) {
        const FactView& fact = state.facts[(std::size_t)i];
        DrawFactRow(canvas, fact, left, right, top + i * kRowPitch, state.selected == i,
                    theme);
    }

    // A failed menu action is painted as the card's last line on purpose: the
    // card used to show errors only in its empty state, so a locked write looked
    // like it had silently done nothing. Truncated with cjk metrics like a row.
    int lineY = top + rows * kRowPitch;
    if (HasMore(state, expanded)) {
        const int hidden = (int)state.facts.size() - rows;
        wchar_t more[48];
        swprintf(more, 48, L"+%d MORE - EXPAND TO SEE ALL", hidden);
        canvas.Text(more, (float)left, (float)(lineY + 2), theme.warn, kMetaScale);
        lineY += kMoreLineH;
    }
    if (!state.error.empty()) {
        cjk::TextTop(canvas, FitTo(state.error, kTextWidth, kMetaScale), (float)left,
                     (float)(lineY + 2), theme.bad, kMetaScale);
    }
}

wg::StatusChip Status(const MemoryViewState& state, const wg::Theme& theme) {
    if (!state.error.empty()) return wg::StatusChip{L"ERROR", theme.bad};
    if (state.loading) return wg::StatusChip{L"LOADING", theme.faint};
    if (state.facts.empty()) return wg::StatusChip{L"EMPTY", theme.faint};
    wchar_t buf[24];
    swprintf(buf, 24, L"%d FACTS", (int)state.facts.size());
    return wg::StatusChip{buf, theme.ok};
}

} // namespace memory_view

namespace {

using assistant::MemoryStore;
using memory_view::FactView;
using memory_view::MemoryViewState;

constexpr int kMenuPin = 1;
constexpr int kMenuDelete = 2;
constexpr int kMenuRefresh = 3;
constexpr int kMenuEdit = 4;
constexpr int kMenuClear = 5;

// Every field of the stored fact the card paints, mapped in one place so a new
// Fact field cannot silently render as the row default.
FactView ToView(const assistant::Fact& fact) {
    FactView view;
    view.id = fact.id;
    view.kind = fact.kind;
    view.text = fact.text;
    view.sourceRef = fact.sourceRef;
    view.createdMs = fact.createdMs;
    view.updatedMs = fact.updatedMs;
    view.expiresMs = fact.expiresMs;
    view.pinned = fact.pinned;
    return view;
}

std::vector<FactView> ToViews(const std::vector<assistant::Fact>& facts) {
    std::vector<FactView> views;
    views.reserve(facts.size());
    for (const assistant::Fact& fact : facts) views.push_back(ToView(fact));
    return views;
}

// The one thing a store notification may touch. The handler registered with the
// store holds a weak_ptr to this state instead of the card, because
// MemoryStore::Unsubscribe does not wait for a notification that is already in
// flight: a late handler must never dereference a destroyed card.
//
// 'wake' is invoked with 'mu' held. That ordering is what makes destruction
// safe - the destructor clears 'wake' under the same lock, so a notification
// either repaints before the destructor gets the lock or finds nothing to call
// afterwards. The repaint callback only posts a message (wg::Module::
// RequestRepaint -> Board::NotifyDirty -> PostMessageW), so calling it under
// this lock cannot re-enter the card or take another lock of ours.
struct CardState {
    std::mutex mu;
    MemoryViewState view;
    std::function<void()> wake;
    // Host re-plan request (wg::Module::RequestRefresh). A fact-count change can
    // change BodyHeight(), and the board only re-plans a card's rectangle on a
    // layout commit, so a bare repaint could clip or overlap the card. Called
    // only for a store notification, never from Refresh() itself, or the board
    // would refresh forever. Cleared together with 'wake' by ~MemoryCard.
    std::function<void()> replan;
    bool dead = false;   // set by ~MemoryCard under mu
};

// Selected fact id from the list about to be replaced. Rows are addressed by
// id: the store sorts pinned/newest first, so an index kept across a change
// would silently jump to a different fact.
std::wstring SelectedIdLocked(const CardState& state) {
    const int index = state.view.selected;
    if (index < 0 || index >= (int)state.view.facts.size()) return std::wstring();
    return state.view.facts[(std::size_t)index].id;
}

// Replaces the rows and re-finds the selected fact by id. A fact that vanished
// clears the selection instead of selecting whatever moved into that row.
void ApplyFactsLocked(CardState& state, std::vector<FactView> facts) {
    const std::wstring selectedId = SelectedIdLocked(state);
    state.view.facts = std::move(facts);
    state.view.selected = -1;
    if (selectedId.empty()) return;
    for (std::size_t i = 0; i < state.view.facts.size(); ++i) {
        if (state.view.facts[i].id == selectedId) {
            state.view.selected = (int)i;
            return;
        }
    }
}

void WakeLocked(const CardState& state) {
    if (state.wake) state.wake();
}

// Same locking rule as WakeLocked: only called with state.mu held, so the
// destructor either runs after this posts or clears 'replan' first. The host
// callback only does PostMessageW (never a blocking refresh), which is what
// makes calling it under our lock safe.
void ReplanLocked(const CardState& state) {
    if (state.replan) state.replan();
}

// Publishes the shared store's active facts (initial snapshot, store change and
// the menu "Refresh"). Facts() copies under the store's lock and the store
// notifies with that lock released, so reading it here cannot deadlock; the
// state lock is held only for the swap + wake.
//
// clearError is false for a store notification: the error belongs to *this*
// card's failed action, so an unrelated write (the engine adding a fact after an
// answer) must not wipe it. Only the card's own successful action and the
// explicit menu Refresh clear it.
//
// requestReplan is true only for a store notification. That publish may change
// the row count and therefore BodyHeight(), so the host has to re-plan the card;
// Refresh() (which is the host's answer to that request) publishes with false,
// so the request cannot recurse.
void PublishFrom(const std::weak_ptr<MemoryStore>& weakStore, CardState& state,
                 bool clearError, bool requestReplan) {
    const std::shared_ptr<MemoryStore> store = weakStore.lock();
    if (!store) return;
    std::vector<FactView> facts = ToViews(store->Facts());
    std::lock_guard<std::mutex> lock(state.mu);
    if (state.dead) return;
    ApplyFactsLocked(state, std::move(facts));
    state.view.loading = false;
    if (clearError) state.view.error.clear();
    WakeLocked(state);
    if (requestReplan) ReplanLocked(state);
}

// Framework prompt "module does not build its own host" fallback: the
// single-line editor is the board's wg::PromptText, the confirmation is the
// Windows message box. If the prompt is already open it returns false (the
// modal guard), which the card treats exactly like a cancel.
bool DefaultEditPrompt(const std::wstring& initial, std::wstring* text) {
    wg::PromptSpec spec;
    spec.label = L"EDIT MEMORY FACT";
    spec.hint = L"ENTER SAVE   ESC CANCEL";
    spec.initial = initial;
    spec.width = 420;
    std::wstring value;
    if (!wg::PromptText(spec, value)) return false;
    if (text != nullptr) *text = std::move(value);
    return true;
}

bool DefaultConfirm(const std::wstring& message) {
    return MessageBoxW(nullptr, message.c_str(), L"MEMORY",
                       MB_OKCANCEL | MB_ICONWARNING | MB_SETFOREGROUND) == IDOK;
}

class MemoryCard final : public MemoryModule {
public:
    ~MemoryCard() override {
        // Order is the invariant: silence the repaint and re-plan entry points
        // under the state lock, drop the subscription, then release. A
        // notification the store already copied still runs - it owns a
        // shared_ptr to the state, sees 'dead' (and empty wake/replan) and
        // returns without touching the card.
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            state_->wake = nullptr;
            state_->replan = nullptr;
            state_->dead = true;
        }
        DetachStore();
        state_.reset();
    }

    wg::ModuleSpec Describe() const override {
        wg::ModuleSpec spec;
        spec.id = L"memory";
        spec.title = L"MEMORY";
        spec.configFile = L"memory.ini";
        spec.sizePreset = wg::SizePreset::Standard;
        // No polling: the store notifies on a durable change, so an idle card
        // costs no timer and no CPU.
        spec.refreshSeconds = 0;
        spec.hasDetails = true;
        return spec;
    }

    void Load() override {
        // Same directory the assistant engine uses -> the same shared instance,
        // hence the same facts and the same notifications.
        OpenStore(MemoryDir());
    }

    // Menu/host Refresh: re-read the shared instance. Deliberately not a
    // Load(). The board also runs this hook automatically right after every
    // menu action and when a card is shown, so it must *not* clear this card's
    // error line - that would swallow a just-reported failure before the user
    // could read it. The explicit menu "Refresh" below is the one refresh path
    // that clears it.
    //
    // When Load() could not get a store (the frozen contract returns nullptr
    // with the error, Engine and UI alike judge success by the pointer), the
    // open is retried here: a locked or unreadable facts file is usually
    // transient, and the card must not need a board restart to recover.
    void Refresh() override {
        if (!store_) {
            OpenStore(MemoryDir());
            return;
        }
        PublishFrom(std::weak_ptr<MemoryStore>(store_), *state_, /*clearError=*/false,
                    /*requestReplan=*/false);
    }
    void Commit() override {}

    int BodyHeight() const override {
        std::lock_guard<std::mutex> lock(state_->mu);
        return memory_view::BodyHeight(state_->view, expanded());
    }

    void PaintBody(wg::Canvas& canvas, const wg::Rect& body) override {
        MemoryViewState snapshot;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            snapshot = state_->view;
        }
        memory_view::DrawBody(snapshot, canvas, body, theme_, expanded());
    }

    wg::StatusChip Status() const override {
        std::lock_guard<std::mutex> lock(state_->mu);
        return memory_view::Status(state_->view, theme_);
    }

    memory_view::MemoryViewState ViewState() const override {
        std::lock_guard<std::mutex> lock(state_->mu);
        return state_->view;
    }

    std::vector<wg::MenuItem> Menu() const override {
        bool pinned = false;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            if (SelectedLocked()) pinned = SelectedFact()->pinned;
        }
        return {
            wg::MenuItem{pinned ? L"Unpin fact" : L"Pin fact", kMenuPin, pinned},
            wg::MenuItem{L"Delete fact", kMenuDelete},
            wg::MenuItem{L"Edit fact", kMenuEdit},
            wg::MenuItem{L"Clear all memory", kMenuClear},
            wg::MenuItem{L"Refresh", kMenuRefresh},
        };
    }

    void OnMenu(int id) override {
        std::wstring factId;
        std::wstring factText;
        bool pinned = false;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            if (SelectedLocked()) {
                factId = SelectedFact()->id;
                factText = SelectedFact()->text;
                pinned = SelectedFact()->pinned;
            }
        }

        if (id == kMenuPin) {
            if (factId.empty()) return;
            // With a shared store the card performs the change itself, so it is
            // written once; the handler is the fallback for a card whose Load
            // could not open the store.
            if (store_) {
                TogglePin(factId, !pinned);
            } else if (pin_) {
                pin_(factId, !pinned);
            }
        } else if (id == kMenuDelete) {
            if (factId.empty()) return;
            if (store_) {
                RemoveFact(factId);
            } else if (remove_) {
                remove_(factId);
            }
        } else if (id == kMenuEdit) {
            if (factId.empty()) return;
            EditFact(factId, factText);
        } else if (id == kMenuClear) {
            ClearAllMemory();
        } else if (id == kMenuRefresh) {
            if (store_) {
                // The user asked for a fresh snapshot: the only refresh path
                // that also drops a stale error line. It is already inside a
                // host refresh, so it must not request another one.
                PublishFrom(std::weak_ptr<MemoryStore>(store_), *state_,
                            /*clearError=*/true, /*requestReplan=*/false);
            } else if (refresh_) {
                refresh_();
                RequestRepaint();
            }
        }
    }

    void OnClick(int x, int y) override {
        (void)x;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            state_->view.selected = memory_view::RowAt(state_->view, y, expanded());
        }
        RequestRepaint();
    }

    void OnExpandChanged(bool) override { RequestRepaint(); }

    // ---- engine wiring -------------------------------------------------

    void SetFacts(std::vector<FactView> facts) override {
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            ApplyFactsLocked(*state_, std::move(facts));
            state_->view.loading = false;
            state_->view.error.clear();
        }
        RequestRepaint();
    }

    void SetLoading(bool loading) override {
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            state_->view.loading = loading;
        }
        RequestRepaint();
    }

    void SetError(const std::wstring& error) override {
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            state_->view.error = error;
            state_->view.loading = false;
        }
        RequestRepaint();
    }

    void SetPinHandler(std::function<void(const std::wstring&, bool)> handler) override {
        pin_ = std::move(handler);
    }

    void SetDeleteHandler(std::function<void(const std::wstring&)> handler) override {
        remove_ = std::move(handler);
    }

    void SetRefreshHandler(std::function<void()> handler) override {
        refresh_ = std::move(handler);
    }

    void SetEditPromptHandler(EditPrompt prompt) override {
        editPrompt_ = std::move(prompt);
    }

    void SetConfirmHandler(ConfirmPrompt confirm) override {
        confirm_ = std::move(confirm);
    }

private:
    // "<config dir>\memory.ini" -> "<config dir>", exactly as the assistant
    // card derives the engine's config directory from its own store.
    std::wstring MemoryDir() {
        const std::wstring& path = store().path();
        const std::size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
    }

    // Opens (or retries) the shared instance for 'dir'. Success is the returned
    // pointer, never "error is empty": on failure the card has no store, shows
    // the error and keeps the handler fallback.
    void OpenStore(const std::wstring& dir) {
        DetachStore();   // a repeated open must never leave two subscriptions
        std::wstring error;
        // The card never allocates its own store and never re-Loads the file
        // (that would replace the facts the engine worker is holding).
        std::shared_ptr<MemoryStore> opened = assistant::OpenSharedMemory(dir, &error);
        if (!opened) {
            SetError(error.empty() ? std::wstring(L"MEMORY UNAVAILABLE") : error);
            return;
        }
        store_ = std::move(opened);
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            state_->dead = false;
            // Both callbacks are invoked only with state_->mu held (see CardState).
            state_->wake = [this] { RequestRepaint(); };
            state_->replan = [this] { RequestRefresh(); };
        }

        // Subscribe before the first snapshot: a write that lands in between
        // publishes itself, and the snapshot below is never stale by more than
        // that one notification.
        const std::weak_ptr<CardState> weakState = state_;
        const std::weak_ptr<MemoryStore> weakStore = store_;
        token_ = store_->Subscribe([weakState, weakStore] {
            if (const std::shared_ptr<CardState> state = weakState.lock()) {
                // A store change does not clear this card's error: it belongs
                // to a failed action here, not to the fact that just arrived.
                // It does request one host re-plan: the row count (and so
                // BodyHeight) may have changed and a repaint alone cannot
                // re-measure the card.
                PublishFrom(weakStore, *state, /*clearError=*/false,
                            /*requestReplan=*/true);
            }
        });
        PublishFrom(weakStore, *state_, /*clearError=*/true, /*requestReplan=*/false);
    }

    // Callers hold state_->mu.
    bool SelectedLocked() const {
        return state_->view.selected >= 0 &&
               state_->view.selected < (int)state_->view.facts.size();
    }
    const FactView* SelectedFact() const {
        return &state_->view.facts[(std::size_t)state_->view.selected];
    }

    void DetachStore() {
        if (store_ && token_ != 0) store_->Unsubscribe(token_);
        token_ = 0;
        store_.reset();
    }

    // Pin / Delete / Edit / Clear go to the shared store and, on success, the
    // store's notification republishes Facts(): nothing is painted
    // optimistically, so a rolled-back write (store.SaveFactsOrRollback) can
    // never look applied. A no-op SetPinned (already in that state) does not
    // notify and needs no repaint - the view already shows it. A failure sets a
    // readable line that survives later unrelated notifications (see
    // PublishFrom).
    void TogglePin(const std::wstring& factId, bool pinned) {
        std::wstring error;
        if (!store_->SetPinned(factId, pinned, &error)) {
            SetError(error.empty() ? std::wstring(L"PIN FAILED") : error);
            return;
        }
        ClearError();
    }

    void RemoveFact(const std::wstring& factId) {
        std::wstring error;
        if (!store_->DeleteFact(factId, &error)) {
            SetError(error.empty() ? std::wstring(L"DELETE FAILED") : error);
            return;
        }
        ClearError();
    }

    // "Edit fact": the framework prompt (or the injected test callback) supplies
    // the replacement, then the shared store's UpdateFact writes it. Empty and
    // sensitive text is rejected by the store (kFactEmpty / kFactSensitive);
    // the card shows that message instead of dropping it, so a non-empty list
    // never looks like the edit silently did nothing.
    void EditFact(const std::wstring& factId, const std::wstring& current) {
        if (!store_) {
            SetError(L"MEMORY UNAVAILABLE - EDIT NOT SAVED");
            return;
        }
        std::wstring text;
        if (!editPrompt_ || !editPrompt_(current, &text)) return;   // cancelled
        std::wstring error;
        if (!store_->UpdateFact(factId, text, &error)) {
            SetError(L"EDIT FAILED: " +
                     (error.empty() ? std::wstring(L"UNKNOWN ERROR") : error));
            return;
        }
        ClearError();
    }

    // "Clear all memory": confirm first (the card opens no host of its own; the
    // default is a message box, tests inject the callback), then ClearAll on the
    // shared store. On success the store notification republishes an empty list
    // before ClearAll returns, so the card is empty immediately; on failure the
    // facts stay and the error is visible.
    void ClearAllMemory() {
        if (!store_) {
            SetError(L"MEMORY UNAVAILABLE - NOTHING CLEARED");
            return;
        }
        std::size_t facts = 0;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            facts = state_->view.facts.size();
        }
        const std::size_t episodes = store_->RecentEpisodes(0).size();
        const std::wstring message =
            L"Clear all memory?\n\n" + std::to_wstring(facts) + L" facts and " +
            std::to_wstring(episodes) +
            L" episodes will be deleted.\nThis cannot be undone.";
        if (!confirm_ || !confirm_(message)) return;   // cancelled: no change

        std::wstring error;
        if (!store_->ClearAll(&error)) {
            SetError(L"CLEAR FAILED: " +
                     (error.empty() ? std::wstring(L"UNKNOWN ERROR") : error));
            return;
        }
        ClearError();
    }

    // Drops the error line without touching the rows. Called only for this
    // card's own successful action (and through PublishFrom for Refresh), never
    // for an unrelated notification.
    void ClearError() {
        bool hadError = false;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            hadError = !state_->view.error.empty();
            state_->view.error.clear();
        }
        if (hadError) RequestRepaint();
    }

    std::shared_ptr<CardState> state_ = std::make_shared<CardState>();
    std::shared_ptr<MemoryStore> store_;
    uint64_t token_ = 0;
    std::function<void(const std::wstring&, bool)> pin_;
    std::function<void(const std::wstring&)> remove_;
    std::function<void()> refresh_;
    // Defaults are the real framework prompt / message box; desk_render injects
    // its own so a card-level test never opens a modal loop.
    EditPrompt editPrompt_ = DefaultEditPrompt;
    ConfirmPrompt confirm_ = DefaultConfirm;
    wg::Theme theme_;
};

} // namespace

std::unique_ptr<MemoryModule> CreateMemoryModule() {
    return std::make_unique<MemoryCard>();
}
