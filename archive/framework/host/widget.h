// The widget contract.
//
// A widget is one header's worth of code: a data struct, a blocking Refresh
// that fills it, and a Paint that draws it. The host owns the window, tray,
// hotkeys, timers, persistence and threading.
//
// Data flow (this is the whole concurrency model):
//
//   UI thread                                  worker thread
//   ---------                                  -------------
//   Load()   -> LoadData(staging) -> shown
//   (idle)                                     Refresh() -> RefreshData(staging)
//   Commit() <- swap(shown, staging) <-------------- post WM_APP when done
//   Paint(shown)
//
// RefreshData() only touches staging, PaintData() only touches shown, and the
// host never runs them at the same time - so a slow HTTP call never blocks the
// message loop and widget code needs no locks.
#pragma once

#include "widget/canvas.h"
#include "widget/hotkey.h"
#include "widget/store.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace wg {

class Host;

struct Desc {
    Desc() = default;
    Desc(std::wstring id_, std::wstring title_, int width_)
        : id(std::move(id_)), title(std::move(title_)), width(width_) {}

    std::wstring id;          // stable: window class, mutex, <id>.ini, credential prefix
    std::wstring title;       // tray tooltip + autostart registry value
    int width = 280;          // pixels; height comes from Widget::Height()
    std::vector<Hotkey> hotkeys;  // optional actions registered by the host
};

class Widget {
public:
    virtual ~Widget() = default;

    virtual Desc Describe() = 0;

    // Optional: paint a 16x16 tray icon. Return false to keep the default glyph.
    virtual bool TrayIcon(Canvas&) { return false; }

    // Optional: show "Settings..." in the tray/context menu.
    virtual bool HasSettings() const { return false; }
    virtual void OnSettings() {}

    // Optional: a read-only details panel, reachable from the tray menu and
    // Options::detailsHotkey (default Ctrl+Alt+D).
    virtual bool HasDetails() const { return false; }
    virtual void ShowDetails() {}

    // Optional dynamic tray actions. The host appends one menu item per label
    // and calls OnAction(index) with its position. Hidden when empty.
    virtual std::vector<std::wstring> Actions() { return {}; }
    virtual bool OnAction(int) { return false; }

    // Optional custom hotkeys advertised in Desc::hotkeys. Return true when the
    // key was handled (the host ignores the result; it is informational).
    virtual bool OnHotkey(int) { return false; }

    // Optional mouse/keyboard hooks, enabled by Options::focusable. Coordinates
    // are client pixels; keys arrive after the host gives the window focus.
    virtual void OnClick(int, int) {}
    virtual void OnChar(wchar_t) {}
    virtual bool OnKey(UINT, bool, bool, bool) { return false; }
    virtual void OnFocus(bool) {}
    virtual void OnScroll(int) {}

    bool editing() const { return editing_; }
    Store& store() { return *store_; }

    // Host plumbing for streaming: the worker calls RequestRepaint(), the host
    // posts a message and repaints on the UI thread.
    void SetRepaintHook(std::function<void()> hook) { repaint_ = std::move(hook); }

    // Host plumbing; WidgetT implements all of it.
    virtual void Load() = 0;
    virtual void Refresh() = 0;
    virtual void Commit() = 0;
    virtual int  Height() const = 0;
    virtual void Paint(Canvas&) = 0;

protected:
    void RequestRepaint() const {
        if (repaint_) repaint_();
    }

private:
    friend class Host;
    Store* store_ = nullptr;
    bool editing_ = false;
    std::function<void()> repaint_;
};

template <class Data>
class WidgetT : public Widget {
public:
    void Load() final {
        LoadData(staging_);
        shown_ = staging_;
    }
    void Refresh() final { RefreshData(staging_); }
    void Commit() final { std::swap(shown_, staging_); OnCommit(); }
    int  Height() const final { return HeightOf(shown_); }
    void Paint(Canvas& canvas) final { PaintData(shown_, canvas); }

protected:
    virtual void LoadData(Data&) {}              // UI thread, once
    virtual void RefreshData(Data&) = 0;         // worker thread, may block
    virtual int  HeightOf(const Data&) const = 0;  // UI thread
    virtual void PaintData(const Data&, Canvas&) = 0;  // UI thread
    virtual void OnCommit() {}                   // UI thread, after the swap
    const Data& shown() const { return shown_; }

private:
    Data shown_{};
    Data staging_{};
};

} // namespace wg
