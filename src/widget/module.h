// The card contract for the desktop board.
//
// A module is one header's worth of code: a data struct, a blocking Refresh
// that fills it, a BodyHeight and a PaintBody. The board owns the window, the
// board surface, card chrome, dragging, menus, tray, timer and worker threads.
//
// Data flow is the same double buffer as the standalone WidgetT:
//
//   UI thread                                  worker thread
//   ---------                                  -------------
//   Load()   -> LoadData(staging) -> shown
//   (idle)                                     Refresh() -> RefreshData(staging)
//   Commit() <- swap(shown, staging) <-------------- post WM_APP when done
//   PaintBody(shown)
//
// RefreshData() can run concurrently with PaintBody(). It only changes staging;
// PaintBody() reads shown. The worker copies shown to staging before refreshing,
// and the board joins the worker before a successful Commit(). Shared state
// outside these two buffers still needs its own synchronization.
#pragma once

#include "widget/canvas.h"
#include "widget/geom.h"
#include "widget/hotkey.h"
#include "widget/size.h"
#include "widget/store.h"

#include <algorithm>
#include <climits>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wg {

// Clamps the body height a component says it needs to the range it declared
// renderable. The framework always measures the returned value, so a component
// whose MinBodyHeight/MaxBodyHeight differ from BodyHeight() gets a stable,
// documented result instead of silent clipping.
//
// exceededMax (optional) is set when the request was clamped DOWN, i.e. the
// component must render inside maxBody (its own scrolling/eliding).
// A negative min is treated as 0; a negative max, or max < min, is a contract
// error: it is ignored (only min is applied) and the caller may log it.
inline int ClampBodyHeight(int requested, int minBody, int maxBody,
                           bool* exceededMax = nullptr) {
    if (exceededMax) *exceededMax = false;
    if (minBody < 0) minBody = 0;
    if (requested < minBody) requested = minBody;
    if (maxBody < 0 || maxBody < minBody) return requested;
    if (requested > maxBody) {
        if (exceededMax) *exceededMax = true;
        return maxBody;
    }
    return requested;
}

// Right-aligned status in a card title bar. color == 0 means theme.faint.
struct StatusChip {
    std::wstring text;
    Color color = 0;
};

// A module-owned action appended to the card menu.
struct MenuItem {
    std::wstring label;
    int id = 0;
    bool checked = false;
};

// One keyboard event, already stripped of the Windows plumbing.
struct KeyEvent {
    unsigned vk = 0;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
};

struct ModuleSpec {
    std::wstring id;          // stable config prefix, menu key and catalog id
    std::wstring title;       // card title
    std::wstring configFile;  // empty -> "<id>.ini"
    // Legacy card width. Only the Auto preset reads it: a component that does
    // not declare a preset keeps exactly this width so old content is never
    // re-laid out. New components declare sizePreset instead.
    int width = 340;
    int refreshSeconds = 0;   // 0 = manual refresh only
    // True when the card can collapse/expand. The expanded height comes from
    // BodyHeight(); the framework checks the resulting frame before committing.
    bool hasDetails = false;
    bool expandedByDefault = false;
    std::vector<Hotkey> hotkeys;  // local ids 1..99; the board allocates
    // Fixed width/height policy. Compact 256, Standard 340, Wide 568, and Full
    // (the board interior width, re-measured when the panel resizes). Auto is
    // the legacy compatibility path and keeps ModuleSpec::width.
    SizePreset sizePreset = SizePreset::Auto;
    // First-run visibility. Turning a card off with "Remove card" persists in
    // the card config and wins over this default.
    bool visibleByDefault = true;
};

class Module {
public:
    virtual ~Module() = default;

    virtual ModuleSpec Describe() const = 0;  // must not touch store()

    virtual void Load() = 0;      // UI thread, once
    virtual void Refresh() = 0;   // worker thread, may block
    virtual void Commit() = 0;    // UI thread, after the swap

    // Content height the component needs for its current state, including the
    // inline details while expanded. The framework adds the card chrome
    // (title bar plus padding), rounds the result up to 8 px and validates it
    // against the preset range; it never shortens a component to fit a preset.
    virtual int BodyHeight() const = 0;
    // After a successful Refresh, the host can check the prospective height
    // before publishing it. Stateful custom modules may override this if
    // their refresh changes the height; the default keeps the current height.
    // The board plans the whole layout with this value first and only then
    // commits the refresh, so a taller result never overlaps another card.
    virtual int PendingBodyHeight() const { return BodyHeight(); }
    // Smallest / largest body height (pixels, excluding card chrome) the
    // component can render. The framework clamps BodyHeight()/PendingBodyHeight()
    // into this range before measuring the card, so a component can declare a
    // scrollable range and still work when the board has less room.
    // Defaults (0 .. INT_MAX) mean "exactly what BodyHeight() says".
    virtual int MinBodyHeight() const { return 0; }
    virtual int MaxBodyHeight() const { return INT_MAX; }
    // UI thread, after a layout commit, whenever this card's body rectangle
    // changed (size or position), including the first time it becomes visible.
    // Not called while the card is hidden. A module may resize internal state
    // (terminal rows, visible list rows) here. Must not throw: exceptions are
    // swallowed and logged.
    virtual void OnBodySizeChanged(const Rect& body) { (void)body; }
    virtual void PaintBody(Canvas& canvas, const Rect& body) = 0;

    virtual StatusChip Status() const { return {}; }
    virtual std::vector<MenuItem> Menu() const { return {}; }
    // Called by the board on the UI thread for one of this module's Menu()
    // items. A menu action never has to ask for a refresh: after the callback
    // returns the board runs one refresh of this card (OnMenu -> StartRefresh),
    // so calling RequestRefresh() from here posts a *second* request while that
    // first worker is still busy - the board records it as "again" and the card
    // ends up refreshing (and, if it fetches, fetching) twice per click.
    // Out-of-band events are different: a worker result, a focus change or a
    // store notification may still call RequestRefresh() to ask for one.
    virtual void OnMenu(int id) { (void)id; }
    virtual bool OnHotkey(int localId) { (void)localId; return false; }
    virtual void OnClick(int x, int y) { (void)x; (void)y; }
    virtual void OnHover(int x, int y) { (void)x; (void)y; }

    // Mouse inside the card body: press, drag with the button held, release.
    // OnClick keeps its "pressed and released without moving" meaning; a drag
    // is reported through OnPress/OnDrag instead (text selection).
    virtual void OnPress(int x, int y) { (void)x; (void)y; }
    virtual void OnDrag(int x, int y) { (void)x; (void)y; }
    // Wheel inside the body; 'delta' uses raw Windows wheel units (120 per
    // detent), positive = away from the user. RequestRepaint() after changes.
    virtual void OnWheel(int x, int y, int delta) { (void)x; (void)y; (void)delta; }
    // Collapsed <-> expanded, i.e. the body just got shorter or taller.
    // Called only after the framework accepted the new expansion (the layout
    // plan succeeded), so a terminal may resize its rows here.
    virtual void OnExpandChanged(bool expanded) { (void)expanded; }

    // Keyboard. The board focuses a card only after a click whose point the
    // module claimed with WantsKeyboard(), so every other card (and every other
    // pixel of the board) keeps the "a click never steals focus" rule. While a
    // card is focused the board forwards keys, characters, pasted text and the
    // IME composition position to it.
    virtual bool WantsKeyboard(int x, int y) const {
        (void)x;
        (void)y;
        return false;
    }
    virtual void OnFocusChanged(bool focused) { (void)focused; }
    virtual bool OnKey(const KeyEvent& key) {
        (void)key;
        return false;
    }
    virtual void OnChar(wchar_t c) { (void)c; }
    virtual void OnPaste(std::wstring_view text) { (void)text; }
    // Caret in body-local pixels; the board anchors the IME window there.
    virtual bool CaretPos(int* x, int* y) const {
        (void)x;
        (void)y;
        return false;
    }

    // Host plumbing. Bound by the board before Load().
    void Bind(Store* store, std::function<void()> repaint) {
        store_ = store;
        repaint_ = std::move(repaint);
    }
    // Separate setter: the board binds this after the module is in its list, so
    // the callback can address the card by index.
    void BindFocus(std::function<void()> focus) { focus_ = std::move(focus); }
    void RequestFocus() const {
        if (focus_) focus_();
    }
    // Worker -> UI refresh request. The board binds this to a PostMessage so a
    // module can ask for a refresh without touching the worker/UI state from a
    // callback thread (the board starts the worker from the UI thread).
    void BindRefresh(std::function<void()> refresh) { refresh_ = std::move(refresh); }
    void RequestRefresh() const {
        if (refresh_) refresh_();
    }
    // Ctrl+V is read by the board; Ctrl+C in a card goes through here, so a
    // module can put a selection on the clipboard without opening it itself.
    void BindClipboard(std::function<void(std::wstring_view)> copy) {
        copy_ = std::move(copy);
    }
    void CopyToClipboard(std::wstring_view text) const {
        if (copy_ && !text.empty()) copy_(text);
    }
    Store& store() { return *store_; }
    bool expanded() const { return expanded_; }
    void SetExpanded(bool on) { expanded_ = on; }
    bool focused() const { return focused_; }
    bool caretOn() const { return caretOn_; }
    void SetFocusState(bool focused, bool caretOn) {
        focused_ = focused;
        caretOn_ = caretOn;
    }
    void RequestRepaint() const {
        if (repaint_) repaint_();
    }

private:
    Store* store_ = nullptr;
    bool expanded_ = false;
    bool focused_ = false;
    bool caretOn_ = false;
    std::function<void()> repaint_;
    std::function<void()> focus_;
    std::function<void()> refresh_;
    std::function<void(std::wstring_view)> copy_;
};

namespace detail {
// The board calls this only after joining the refresh worker. A failed refresh
// must never publish its partially written staging data. An exception from
// Commit itself is reported as failure; a module's OnCommit runs after swap,
// so it should not throw and cannot assume its side effects will be rolled back.
inline bool CommitRefresh(Module& module, bool refreshed) noexcept {
    if (!refreshed) return false;
    try {
        module.Commit();
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace detail

template <class Data>
class ModuleT : public Module {
public:
    void Load() final {
        LoadData(staging_);
        shown_ = staging_;
    }
    void Refresh() final {
        staging_ = shown_;
        RefreshData(staging_);
    }
    void Commit() final {
        std::swap(shown_, staging_);
        OnCommit();
    }
    int BodyHeight() const final { return BodyHeightOf(shown_); }
    int PendingBodyHeight() const final { return BodyHeightOf(staging_); }
    void PaintBody(Canvas& canvas, const Rect& body) final {
        PaintBodyOf(shown_, canvas, body);
    }

protected:
    virtual void LoadData(Data&) {}
    virtual void RefreshData(Data&) = 0;
    virtual int BodyHeightOf(const Data&) const = 0;
    virtual void PaintBodyOf(const Data&, Canvas&, const Rect&) = 0;
    virtual void OnCommit() {}
    const Data& shown() const { return shown_; }

private:
    Data shown_{};
    Data staging_{};
};

} // namespace wg
