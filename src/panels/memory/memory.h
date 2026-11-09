// MEMORY card (Standard 340) for M1.1: the long-term facts the assistant has
// been taught, each with its kind, source and age; Pin / Delete / Edit /
// Clear all / Refresh in the card menu.
//
// The card and the assistant engine share one assistant::MemoryStore instance
// for the same config directory (OpenSharedMemory), so a fact the engine stores
// appears here and a Pin / Delete / Edit / Clear from the card menu is the same
// durable change the engine reads back. Load() opens that shared instance,
// subscribes to its change notification and only then publishes the first
// snapshot; a change the store reports republishes Facts() - the card never
// re-Loads the file (that would replace the state the engine is holding) and
// never builds its own store. Because a fact-count change also changes
// BodyHeight(), a notification additionally asks the host for one re-plan
// (wg::Module::RequestRefresh, an async post); the card's own Refresh() is the
// host's answer to that request and never asks again.
//
// A menu action that fails (locked file, empty or sensitive text, unknown id)
// sets a readable error that the card paints as its last line as well, i.e. a
// non-empty list is not allowed to hide it. The error is only cleared by the
// card's own next successful action or by the explicit menu "Refresh": a store
// notification (the engine writing a fact) and the board's automatic post-menu
// Refresh must not swallow it.
//
// The painted view is still a plain snapshot (MemoryViewState) mapped through
// Fact -> FactView, so the render code and the render test stay independent of
// the store headers. Row selection lives in the card: a click picks a fact and
// the menu acts on it, by fact id - never by row index, because a pin/update
// reorders the list.
#pragma once

#include "widget/canvas.h"
#include "widget/geom.h"
#include "widget/module.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace memory_view {

// Standard preset width. The board hands PaintBody exactly this wide a body.
constexpr int kCardWidth = 340;
constexpr int kPadX = 12;
constexpr int kTextWidth = kCardWidth - kPadX * 2;   // 316
// Fact rows are dense metadata: 5x7 font at 1.0, mixed with the 14px CJK mask.
constexpr float kMetaScale = 1.0f;

constexpr int kTopInset = 4;
constexpr int kRowPitch = 32;     // two text lines plus breathing room
constexpr int kRowH = 30;
constexpr int kMoreLineH = 14;
// One line for a failed menu action, painted as the last line of the card. It
// is reserved whether or not an error is currently shown: the board only
// re-plans a card's height on a layout commit, so an error that appeared on its
// own would otherwise be clipped by the body rectangle painted before it.
constexpr int kErrorLineH = 14;
constexpr int kBottomPad = 4;

// Standard is 112..640 outer, i.e. 64..592 body.
constexpr int kMinBody = 112 - 48;   // 64
constexpr int kMaxBody = 640 - 48;   // 592

constexpr int kCollapsedRows = 3;
// 17 rows * 32 + 4 + 14 (MORE) + 14 (error) + 4 = 580, which leaves both the
// "+N MORE" and the error line fully inside the 592 body cap.
constexpr int kExpandedMaxRows = 17;

// One stored fact as the card shows it. 'kind' is a free-form tag (GOAL, PREF,
// CONSTRAINT, PROJECT, MILESTONE, ...) rendered in the kind colour and
// uppercased for the label.
struct FactView {
    std::wstring id;
    std::wstring kind;
    std::wstring text;
    std::wstring sourceRef;
    long long createdMs = 0;
    long long updatedMs = 0;
    long long expiresMs = 0;
    bool pinned = false;
};

struct MemoryViewState {
    std::vector<FactView> facts;
    int selected = -1;         // index into facts; -1 = none
    bool loading = false;
    std::wstring error;
};

// Rows that fit: 3 collapsed, 17 expanded.
int VisibleRows(const MemoryViewState& state, bool expanded);
// True when facts were cut off and the "+N MORE" line is drawn.
bool HasMore(const MemoryViewState& state, bool expanded);

int BodyHeight(const MemoryViewState& state, bool expanded);

// Maps a body-local y to the fact index drawn there, or -1.
int RowAt(const MemoryViewState& state, int y, bool expanded);

void DrawBody(const MemoryViewState& state, wg::Canvas& canvas, const wg::Rect& body,
              const wg::Theme& theme, bool expanded);

wg::StatusChip Status(const MemoryViewState& state, const wg::Theme& theme);

} // namespace memory_view

// UI-side surface of the card. Thread-safe mutators: the store notification can
// fire on the engine worker (a fact added by a finished answer) as well as on
// the UI thread (the card's own Pin / Delete / Edit / Clear).
//
// SetFacts / SetLoading / SetError remain available for a caller that pushes a
// snapshot instead of relying on the shared store; the card itself publishes
// Facts() from the store it opened in Load(). The three Set*Handler hooks are
// the fallback for a card whose Load() could not open the shared store: when the
// card owns a store it performs Pin / Delete / Refresh on that store directly
// (so the change is written once, not twice through the engine as well).
class MemoryModule : public wg::Module {
public:
    // Single-line edit prompt: 'initial' is the current fact text, '*text' the
    // replacement. Returns false when the user cancelled.
    using EditPrompt = std::function<bool(const std::wstring& initial,
                                          std::wstring* text)>;
    // Destructive-action confirmation. Returns true only to go ahead.
    using ConfirmPrompt = std::function<bool(const std::wstring& message)>;

    virtual void SetFacts(std::vector<memory_view::FactView> facts) = 0;
    virtual void SetLoading(bool loading) = 0;
    virtual void SetError(const std::wstring& error) = 0;

    // Read-only copy of what the card paints (tests / diagnostics).
    virtual memory_view::MemoryViewState ViewState() const = 0;

    // Menu actions on the card's selected fact. 'pinned' is the requested new
    // state (toggle); an empty id means nothing was selected.
    virtual void SetPinHandler(std::function<void(const std::wstring& id, bool pinned)>) = 0;
    virtual void SetDeleteHandler(std::function<void(const std::wstring& id)>) = 0;
    virtual void SetRefreshHandler(std::function<void()>) = 0;

    // Edit / Clear-all plumbing. The card never builds a host of its own: the
    // defaults ask the framework (wg::PromptText) for the new text and Windows
    // (MessageBoxW) for the confirmation. Tests inject both so no modal loop can
    // open on a headless machine, and so a cancel is a plain callback result.
    virtual void SetEditPromptHandler(EditPrompt prompt) = 0;
    virtual void SetConfirmHandler(ConfirmPrompt confirm) = 0;
};

std::unique_ptr<MemoryModule> CreateMemoryModule();
