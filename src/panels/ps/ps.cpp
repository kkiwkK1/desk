// POWERSHELL - a real terminal in a card.
//
// ConPTY gives the shell a console, so PSReadLine, vim, less and TUI apps behave
// as they do in Windows Terminal; the card only pushes bytes in and paints the
// grid that comes back. It is still push-driven: the reader thread asks for a
// repaint when new output lands, so an idle card costs nothing.
#include "ps.h"

#include "ps_view.h"

#include "data/ps/keymap.h"
#include "data/ps/pi_process.h"
#include "data/ps/selection.h"
#include "data/ps/terminal.h"
#include "data/ps/text.h"

#include <memory>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kMenuCopy = 1;
constexpr int kMenuClearScrollback = 2;
constexpr int kMenuRestart = 3;
constexpr int kMenuFocus = 4;
constexpr int kMenuSystemTest = 5;
constexpr int kMenuWheelAuto = 6;
constexpr int kMenuWheelHistory = 7;
constexpr int kMenuWheelApplication = 8;

// "Is the machine still there" one-liner. CIM-free: WMI is denied in locked-down
// contexts, the tick counter and Get-PSDrive are not.
const wchar_t* kSystemTest =
    L"\"PS   $($PSVersionTable.PSVersion.ToString())\"; "
    L"\"HOST $env:COMPUTERNAME\"; "
    L"\"UP   $([int]([Environment]::TickCount64/3600000))h\"; "
    L"\"C:   $([math]::Round((Get-PSDrive C).Free/1GB,1))GB FREE\"; "
    L"\"DIR  $($PWD.Path)\"";

class PsModule : public wg::Module {
public:
    ~PsModule() override {
        terminal_.SetOnOutput(nullptr);
        terminal_.Stop();
    }

    wg::ModuleSpec Describe() const override {
        wg::ModuleSpec spec;
        spec.id = L"ps";
        spec.title = L"POWERSHELL";
        spec.configFile = L"ps.ini";
        // The wide preset preserves the terminal's fixed 80-column geometry.
        spec.sizePreset = wg::SizePreset::Wide;
        spec.refreshSeconds = 0;   // push-driven, see the file comment
        spec.hasDetails = true;
        return spec;
    }

    void Load() override {
        wheelMode_ = ps::ParseWheelMode(store().Get(L"wheel.mode", L"auto"));
        terminal_.SetOnOutput([this] { RequestRepaint(); });
        rows_ = expanded() ? ps::kRowsExpanded : ps::kRowsCollapsed;
        // autostart=0 keeps the shell (and its conhost) out of memory until the
        // card is actually used.
        if (store().GetInt(L"autostart", 1) != 0) StartTerminal();
    }

    void Refresh() override {}   // a manual refresh must not restart the shell
    void Commit() override {}

    int BodyHeight() const override { return ps_view::BodyHeight(expanded()); }

    void PaintBody(wg::Canvas& canvas, const wg::Rect& body) override {
        ps_view::DrawBody(terminal_.Snapshot(), selection_, focused(), caretOn(), canvas,
                          body, theme_);
    }

    wg::StatusChip Status() const override {
        return ps_view::Status(terminal_.running(), started_, terminal_.label(),
                               terminal_.scrollOffset(), theme_);
    }

    std::vector<wg::MenuItem> Menu() const override {
        return {
            wg::MenuItem{L"Copy", kMenuCopy},
            wg::MenuItem{L"Clear scrollback", kMenuClearScrollback},
            wg::MenuItem{L"Restart shell", kMenuRestart},
            wg::MenuItem{L"Type here", kMenuFocus},
            wg::MenuItem{L"Run system test", kMenuSystemTest},
            wg::MenuItem{L"Wheel: Auto", kMenuWheelAuto, wheelMode_ == ps::WheelMode::Auto},
            wg::MenuItem{L"Wheel: Terminal history", kMenuWheelHistory,
                         wheelMode_ == ps::WheelMode::History},
            wg::MenuItem{L"Wheel: Application (SGR)", kMenuWheelApplication,
                         wheelMode_ == ps::WheelMode::Application},
        };
    }

    void OnMenu(int id) override {
        switch (id) {
        case kMenuCopy:
            CopySelection();
            break;
        case kMenuClearScrollback:
            terminal_.ClearScrollback();
            break;
        case kMenuRestart:
            StartTerminal();
            break;
        case kMenuFocus:
            RequestFocus();
            break;
        case kMenuSystemTest:
            RunCommand(kSystemTest);
            break;
        case kMenuWheelAuto:
            SetWheelMode(ps::WheelMode::Auto);
            break;
        case kMenuWheelHistory:
            SetWheelMode(ps::WheelMode::History);
            break;
        case kMenuWheelApplication:
            SetWheelMode(ps::WheelMode::Application);
            break;
        default:
            break;
        }
        RequestRepaint();
    }

    // ---- keyboard ---------------------------------------------------------

    bool WantsKeyboard(int, int) const override { return true; }

    void OnFocusChanged(bool) override { RequestRepaint(); }

    bool OnKey(const wg::KeyEvent& key) override {
        if (key.ctrl && key.shift) {
            if (key.vk == 'C') {
                CopySelection();
                return true;
            }
            if (key.vk == 'A') {
                SelectAll();
                return true;
            }
        }
        if (key.ctrl && !key.shift && key.vk == 'V') return false;   // board pastes

        ps::KeyMode mode;
        mode.appCursor = terminal_.appCursorKeys();
        const std::string bytes = ps::EncodeKey(key, mode);
        if (bytes.empty()) return false;

        FollowTail();
        terminal_.Write(bytes);
        RequestRepaint();
        return true;
    }

    void OnChar(wchar_t c) override {
        // Surrogate pairs arrive as two WM_CHARs; glue them before encoding.
        if (c >= 0xD800 && c <= 0xDBFF) {
            pendingHigh_ = c;
            return;
        }
        std::wstring text;
        if (c >= 0xDC00 && c <= 0xDFFF && pendingHigh_ != 0) {
            text.push_back(pendingHigh_);
            pendingHigh_ = 0;
        }
        text.push_back(c);
        FollowTail();
        terminal_.Write(ps::EncodeUtf8(text));
        RequestRepaint();
    }

    void OnPaste(std::wstring_view text) override {
        if (text.empty()) return;
        FollowTail();
        terminal_.Write(ps::EncodePaste(text, terminal_.bracketedPaste()));
        RequestRepaint();
    }

    void OnWheel(int x, int y, int delta) override {
        const ps::View view = terminal_.Snapshot();
        const ps_view::Hit hit = ps_view::HitTest(view, x, y, BodySize());
        if (!hit.ok) return;
        const int steps = wheel_.Add(delta);
        if (steps == 0) return;
        // ConPTY can swallow Pi's mouse and alternate-screen declarations.
        // Identify the Pi CLI among this shell's descendants on demand; never
        // send mouse sequences to an unrelated shell merely because it uses VT.
        const bool pi = wheelMode_ == ps::WheelMode::Auto && !view.mouseTracking &&
                        terminal_.running() && ps::HasPiCliDescendant(terminal_.processId());
        const auto route = ps::ChooseWheelRoute(wheelMode_, view.mouseTracking, view.sgrMouse,
                                                view.alternate, pi);
        if ((route == ps::WheelRoute::SgrMouse || route == ps::WheelRoute::LegacyMouse) &&
            terminal_.running()) {
            terminal_.ScrollToBottom();
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const std::string sequence = ps::EncodeWheel(steps > 0, hit.pos.col + 1,
                                                        hit.row + 1,
                                                        route == ps::WheelRoute::SgrMouse,
                                                        shift, alt, ctrl);
            for (int i = 0; i < std::abs(steps); ++i) terminal_.Write(sequence);
        } else if (route == ps::WheelRoute::History) {
            terminal_.ScrollBy(steps * 3);
        }
        RequestRepaint();
    }

    // ---- mouse: selection -------------------------------------------------

    void OnPress(int x, int y) override {
        const ps::View view = terminal_.Snapshot();
        bottomPressed_ = ps_view::BottomHit(view, x, y, BodySize());
        if (bottomPressed_) {
            dragging_ = false;
            return;
        }
        const ps_view::Hit hit = ps_view::HitTest(view, x, y, BodySize());
        if (!hit.ok) {
            selection_.Clear();
            dragging_ = false;
            RequestRepaint();
            return;
        }
        selection_.active = true;
        selection_.anchor = hit.pos;
        selection_.focus = hit.pos;
        dragging_ = true;
        RequestRepaint();
    }

    void OnDrag(int x, int y) override {
        if (!dragging_) return;
        const ps::View view = terminal_.Snapshot();
        const ps_view::Hit hit = ps_view::HitTest(view, x, y, BodySize());
        if (!hit.ok) return;
        selection_.focus = hit.pos;
        RequestRepaint();
    }

    void OnClick(int x, int y) override {
        if (bottomPressed_) {
            bottomPressed_ = false;
            if (ps_view::BottomHit(terminal_.Snapshot(), x, y, BodySize())) {
                terminal_.ScrollToBottom();
                selection_.Clear();
                RequestRepaint();
            }
            return;
        }
        if (dragging_) {
            dragging_ = false;
            if (!ps::Normalize(selection_, nullptr, nullptr)) selection_.Clear();
            RequestRepaint();
            return;
        }
        selection_.Clear();
        RequestRepaint();
    }

    bool CaretPos(int* x, int* y) const override {
        return ps_view::CellPoint(terminal_.Snapshot(), BodySize(), x, y);
    }

    void OnExpandChanged(bool expanded) override {
        // The ConPTY and the screen must follow the card: rows_ is the single
        // source both the pty resize and the next Start() use, while
        // ps_view::BodyHeight(expanded()) - derived from the same 8/24 row
        // constants, compile-time checked in ps_view.cpp - is the body rect the
        // board paints, HitTest maps and CaretPos anchors to.
        rows_ = expanded ? ps::kRowsExpanded : ps::kRowsCollapsed;
        terminal_.Resize(ps::kCols, rows_);
        RequestRepaint();
    }

private:
    void SetWheelMode(ps::WheelMode mode) {
        wheelMode_ = mode;
        wheel_.Reset();
        store().Set(L"wheel.mode", ps::WheelModeName(mode));
        store().Save();
    }

    // The board hands out the body rect only while painting; the click and IME
    // paths need the same geometry, so rebuild it at the origin. Width is the
    // 80-cell grid and height is the collapsed/expanded body, so this is
    // pixel-identical to the rect skin::CardBody() hands to PaintBody.
    wg::Rect BodySize() const { return wg::Rect{0, 0, ps_view::CardWidth(), BodyHeight()}; }

    void FollowTail() {
        if (terminal_.scrollOffset() > 0) terminal_.ScrollToBottom();
    }

    bool StartTerminal() {
        selection_.Clear();
        dragging_ = false;
        bottomPressed_ = false;
        wheel_.Reset();
        rows_ = expanded() ? ps::kRowsExpanded : ps::kRowsCollapsed;
        const bool ok = terminal_.Start(store(), ps::kCols, rows_);
        if (ok) started_ = true;
        RequestRepaint();
        return ok;
    }

    void RunCommand(const std::wstring& command) {
        if (!terminal_.running() && !StartTerminal()) return;
        terminal_.ScrollToBottom();
        terminal_.Write(ps::EncodeUtf8(command) + "\r");
        RequestRepaint();
    }

    void CopySelection() {
        if (!selection_.active) return;
        const std::wstring text = terminal_.Text(selection_);
        if (!text.empty()) CopyToClipboard(text);
    }

    void SelectAll() {
        const ps::View view = terminal_.Snapshot();
        if (view.lines.empty()) return;
        selection_.active = true;
        selection_.anchor = ps::Pos{view.lines.front().id, 0};
        selection_.focus = ps::Pos{view.lines.back().id, view.cols};
        RequestRepaint();
    }

    ps::Terminal terminal_;
    ps::Selection selection_;
    wg::Theme theme_;
    int rows_ = ps::kRowsCollapsed;
    bool started_ = false;   // at least one successful spawn, so EXITED != NO SHELL
    bool dragging_ = false;
    bool bottomPressed_ = false;
    ps::WheelAccumulator wheel_;
    ps::WheelMode wheelMode_ = ps::WheelMode::Auto;
    wchar_t pendingHigh_ = 0;
};

} // namespace

std::unique_ptr<wg::Module> CreatePsModule() {
    return std::make_unique<PsModule>();
}
