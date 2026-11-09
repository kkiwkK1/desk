#include "assistant.h"

#include "data/assistant/engine.h"

#include "widget/cjk.h"
#include "widget/size.h"
#include "widget/skin.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace assistant_view {

using namespace wg;

namespace {

// The collapsed height is the Wide preset minimum: 120 body + 48 chrome = 168.
// The expanded worst case (3 BRIEF lines + 32 ANSWER lines) stays under 672
// (720 outer), so the card never leaves its declared preset range.
static_assert(kCollapsedHeight == 120,
              "collapsed assistant body must be 120px (Wide minimum)");
static_assert(wg::kCardChromeHeight + kCollapsedHeight == 168,
              "collapsed assistant card must be exactly the Wide preset minimum");
static_assert(kBaseH + kExpandedMaxBriefLines * kBriefLineH +
                      kExpandedMaxAnswerLines * kAnswerLineH <=
                  kWideMaxBody,
              "expanded assistant body must stay inside the Wide preset");
static_assert(kAnswerLineH >= 15,
              "a CJK mask is 14px tall: the answer line must clear it");
static_assert(kTextWidth == 544, "Wide 568 minus 2*12 padding");

const Color kInputBg = Rgba(0x1A1D24);
const Color kAccent = Rgba(0x60A5FA);

// Input row geometry: prompt ">" + the editable text window + a right hint.
constexpr int kPromptW = 10;
constexpr int kHintW = 66;
constexpr int kCaretH = 12;

std::wstring FormatTokens(long long value) {
    if (value <= 0) return L"0";
    wchar_t buf[32];
    swprintf(buf, 32, L"%lld", value);
    return buf;
}

std::wstring FormatElapsed(long long ms) {
    if (ms <= 0) return std::wstring();
    const long long seconds = (ms + 500) / 1000;
    wchar_t buf[32];
    if (seconds < 60) {
        swprintf(buf, 32, L"%lldS", seconds);
    } else {
        swprintf(buf, 32, L"%lldM %02lldS", seconds / 60, seconds % 60);
    }
    return buf;
}

// Truncation measured with cjk metrics: BRIEF/citation text can be Chinese and
// Canvas::TextWidth would count every glyph as 6px and let it run off the card.
std::wstring FitTo(const Canvas& canvas, std::wstring text, int maxWidth, float scale) {
    (void)canvas;
    if (maxWidth <= 0) return std::wstring();
    if ((int)cjk::TextWidth(text, scale) <= maxWidth) return text;
    while (!text.empty() && (int)cjk::TextWidth(text + L"...", scale) > maxWidth) {
        text.pop_back();
    }
    return text + L"...";
}

std::vector<std::wstring> WrapBrief(const AssistantViewState& state, int width, int maxLines) {
    if (state.brief.empty() || maxLines <= 0) return {};
    const int textWidth = std::max(1, width - kPadX * 2);
    // Engine::Brief() returns one block per project (project / last conclusion /
    // blocker) separated by '\n'. Those breaks are semantic, so they are kept
    // and each block is wrapped on its own; cjk::Wrap alone would fold every
    // newline into a space and run the three lines together. The whole brief is
    // still capped at maxLines, with an elision marker on the last line.
    std::vector<std::wstring> lines;
    std::wstring paragraph;
    bool truncated = false;
    const auto flush = [&] {
        if (paragraph.empty()) return;
        if ((int)lines.size() >= maxLines) {
            paragraph.clear();
            truncated = true;
            return;
        }
        const std::vector<std::wstring> wrapped =
            cjk::Wrap(paragraph, textWidth, (std::size_t)maxLines, kMetaScale);
        paragraph.clear();
        for (const std::wstring& line : wrapped) {
            if ((int)lines.size() >= maxLines) {
                truncated = true;
                break;
            }
            lines.push_back(line);
        }
    };
    for (const wchar_t c : state.brief) {
        if (c == L'\r') continue;
        if (c == L'\n') {
            flush();
            continue;
        }
        paragraph.push_back(c);
    }
    flush();
    if ((int)lines.size() > maxLines) {
        lines.resize((std::size_t)maxLines);
        truncated = true;
    }
    if (truncated && !lines.empty()) {
        std::wstring& last = lines.back();
        const bool marked =
            last.size() >= 3 && last.compare(last.size() - 3, 3, L"...") == 0;
        if (!marked) {
            while (!last.empty() &&
                   (int)cjk::TextWidth(last + L"...", kMetaScale) > textWidth) {
                last.pop_back();
            }
            last += L"...";
        }
    }
    return lines;
}

struct InputGeom {
    int textX = 0;   // body-local x of the first visible character
    int textW = 0;
    int start = 0;   // first visible char index
    int end = 0;     // one past the last visible char
    int caretX = 0;  // body-local x of the caret
};

// The caret always stays inside the window: a long question scrolls its tail
// out to the left instead of pushing the caret under the hint text.
InputGeom InputGeometry(const AssistantViewState& state, int width) {
    InputGeom g;
    g.textX = kPadX + kPromptW;
    g.textW = std::max(40, width - kPadX * 2 - kPromptW - kHintW);

    const int len = (int)state.input.size();
    const int caret = std::clamp(state.caret, 0, len);

    float used = 0.0f;
    int start = caret;
    while (start > 0) {
        const float adv = cjk::Advance(state.input[(std::size_t)(start - 1)], kAnswerScale);
        if (used + adv > (float)g.textW) break;
        used += adv;
        --start;
    }
    g.start = start;
    g.caretX = g.textX + (int)std::lround(used);

    int end = caret;
    while (end < len) {
        const float adv = cjk::Advance(state.input[(std::size_t)end], kAnswerScale);
        if (used + adv > (float)g.textW) break;
        used += adv;
        ++end;
    }
    g.end = end;
    return g;
}

// ERROR wins over any partial prose: a failed request must never present the
// tokens it managed to stream as the answer. The error is wrapped to at most
// kExpandedMinAnswerLines lines, which is the height the card reserves anyway.
std::vector<std::wstring> VisibleAnswer(const AssistantViewState& state, int textWidth) {
    if (state.state == State::Error) {
        const std::wstring message =
            state.error.empty() ? std::wstring(L"ASSISTANT ERROR") : state.error;
        return cjk::Wrap(message, std::max(1, textWidth),
                         kExpandedMinAnswerLines, kAnswerScale);
    }
    if (!state.answerLines.empty()) return state.answerLines;
    switch (state.state) {
    case State::Thinking:
        return {L"THINKING..."};
    case State::Idle:
    case State::Error:
        break;
    }
    return {L"ASK A QUESTION - ENTER TO SEND"};
}

} // namespace

Layout ComputeLayout(const AssistantViewState& state, bool expanded, int width) {
    Layout l;
    const int textWidth = std::max(1, width - kPadX * 2);

    const int briefCap = expanded ? kExpandedMaxBriefLines : kCollapsedBriefLines;
    const std::vector<std::wstring> brief = WrapBrief(state, width, briefCap);
    // The BRIEF slot is reserved even when empty so the card does not change
    // height between "no brief yet" and "brief arrived".
    l.briefLines = expanded ? std::clamp((int)brief.size(), 1, kExpandedMaxBriefLines)
                            : kCollapsedBriefLines;

    const int contentLines = (int)VisibleAnswer(state, textWidth).size();
    if (!expanded) {
        l.answerLines = kCollapsedAnswerLines;
    } else {
        const int room = (kWideMaxBody - kBaseH - l.briefLines * kBriefLineH) / kAnswerLineH;
        const int cap = std::min(room, kExpandedMaxAnswerLines);
        l.answerLines = std::clamp(contentLines, kExpandedMinAnswerLines, std::max(1, cap));
    }

    int y = kTopInset;
    l.briefTop = y;
    y += kBriefHeadH;
    l.briefTextTop = y;
    y += l.briefLines * kBriefLineH;
    l.dividerTop = y;
    y += kDividerH;
    l.answerTop = y;
    y += kAnswerHeadH;
    l.answerTextTop = y;
    y += l.answerLines * kAnswerLineH;
    l.citationTop = y;
    y += kCitationH;
    l.inputTop = y;
    y += kInputH;
    l.height = y + kBottomPad;
    return l;
}

int BodyHeight(const AssistantViewState& state, bool expanded) {
    return ComputeLayout(state, expanded, kCardWidth).height;
}

void DrawBody(const AssistantViewState& state, wg::Canvas& canvas, const wg::Rect& body,
              const wg::Theme& theme, bool expanded, bool focused, bool caretOn) {
    const Layout l = ComputeLayout(state, expanded, body.w);
    const int left = body.x + kPadX;
    const int right = body.right() - kPadX;
    const int top = body.y;

    // ---- BRIEF: active DSH project + last conclusion --------------------
    canvas.Text(L"BRIEF", (float)left, (float)(top + l.briefTop), theme.faint, kMetaScale);
    canvas.TextRight(L"DSH PROJECTS", right, (float)(top + l.briefTop), theme.label,
                     kMetaScale);

    const std::vector<std::wstring> brief = WrapBrief(state, body.w, l.briefLines);
    if (brief.empty()) {
        cjk::TextTop(canvas, L"NO BRIEF YET - IT ARRIVES WITH THE FIRST ANSWER",
                     (float)left, (float)(top + l.briefTextTop), theme.faint, kMetaScale);
    } else {
        const int count = std::min<int>((int)brief.size(), l.briefLines);
        for (int i = 0; i < count; ++i) {
            if (brief[(std::size_t)i].empty()) continue;
            cjk::TextTop(canvas, brief[(std::size_t)i], (float)left,
                         (float)(top + l.briefTextTop + i * kBriefLineH), theme.label,
                         kMetaScale);
        }
    }

    skin::Divider(canvas, left, right, top + l.dividerTop, theme);

    // ---- ANSWER header + state chip ------------------------------------
    canvas.Text(L"ANSWER", (float)left, (float)(top + l.answerTop), theme.faint, kMetaScale);
    const wg::StatusChip chip = Status(state, theme);
    canvas.TextRight(chip.text, right, (float)(top + l.answerTop),
                     chip.color ? chip.color : theme.faint, kMetaScale);

    // ---- answer prose ---------------------------------------------------
    const std::vector<std::wstring> answer = VisibleAnswer(state, body.w - kPadX * 2);
    // The error text is always bad-red, whether it replaced a partial answer
    // or not; an empty idle card keeps the faint placeholder colour.
    Color answerColor = theme.text;
    if (state.state == State::Error) {
        answerColor = theme.bad;
    } else if (state.answerLines.empty()) {
        answerColor = theme.faint;
    }
    const int drawn = std::min<int>((int)answer.size(), l.answerLines);
    int lastLine = -1;
    for (int i = 0; i < drawn; ++i) {
        if (answer[(std::size_t)i].empty()) continue;
        cjk::TextTop(canvas, answer[(std::size_t)i], (float)left,
                     (float)(top + l.answerTextTop + i * kAnswerLineH), answerColor,
                     kAnswerScale);
        lastLine = i;
    }

    // Streaming caret: a block at the end of the text already received. It
    // follows the board's caret blink, so an idle streaming pause costs no
    // timer of its own.
    if (state.state == State::Thinking && lastLine >= 0 && caretOn) {
        const std::wstring& line = answer[(std::size_t)lastLine];
        const int x = left + (int)cjk::TextWidth(line, kAnswerScale) + 2;
        const int y = top + l.answerTextTop + lastLine * kAnswerLineH + 2;
        if (x + 2 <= right) canvas.Fill(x, y, 2, 11, theme.borderEdit);
    }

    // ---- citation + cost footer ----------------------------------------
    const int citationY = top + l.citationTop;
    std::wstring meta;
    if (state.inputTokens > 0 || state.outputTokens > 0) {
        meta = L"TOK " + FormatTokens(state.inputTokens + state.outputTokens);
        const std::wstring elapsed = FormatElapsed(state.elapsedMs);
        if (!elapsed.empty()) meta += L"  " + elapsed;
    } else if (!state.footer.empty()) {
        meta = state.footer;
    }
    if (!meta.empty()) {
        canvas.TextRight(meta, right, (float)citationY, theme.faint, kMetaScale);
    }
    const int metaW = meta.empty() ? 0 : (int)canvas.TextWidth(meta, kMetaScale) + 10;
    const int citeW = std::max(0, right - left - metaW);
    if (state.citation.empty()) {
        canvas.Text(L"NO CITATION", (float)left, (float)citationY, theme.warn, kMetaScale);
    } else {
        const std::wstring text =
            L"SRC " + FitTo(canvas, state.citation, citeW - 24, kMetaScale);
        cjk::Text(canvas, text, (float)left, (float)citationY, theme.faint, kMetaScale);
    }

    // ---- input row ------------------------------------------------------
    const int inputY = top + l.inputTop;
    canvas.Fill(body.x + 6, inputY - 1, body.w - 12, kInputH - 2, kInputBg);
    canvas.Text(L">", (float)left, (float)(inputY + 4), kAccent, kAnswerScale);

    const InputGeom g = InputGeometry(state, body.w);
    const int textX = body.x + g.textX;
    const int textY = inputY + 4;

    if (state.input.empty()) {
        cjk::TextTop(canvas, L"TYPE A QUESTION - ENTER TO SEND", (float)textX,
                     (float)textY, theme.faint, kMetaScale);
    } else {
        const std::wstring visible =
            state.input.substr((std::size_t)g.start, (std::size_t)(g.end - g.start));
        cjk::Text(canvas, visible, (float)textX, (float)textY, theme.text, kAnswerScale);
    }
    if (state.state == State::Thinking) {
        canvas.TextRight(L"ESC CANCEL", right - 6, (float)(inputY + 7), theme.warn,
                         kMetaScale);
    } else {
        canvas.TextRight(L"ENTER SEND", right - 6, (float)(inputY + 7), theme.faint,
                         kMetaScale);
    }
    if (focused && caretOn) {
        canvas.Fill(body.x + g.caretX, inputY + 3, 2, kCaretH, theme.borderEdit);
    }
}

wg::StatusChip Status(const AssistantViewState& state, const wg::Theme& theme) {
    switch (state.state) {
    case State::Thinking:
        return wg::StatusChip{L"THINKING", theme.warn};
    case State::Error:
        return wg::StatusChip{L"ERROR", theme.bad};
    case State::Idle:
        break;
    }
    return wg::StatusChip{L"IDLE", theme.faint};
}

} // namespace assistant_view

namespace {

using assistant_view::AssistantViewState;
using assistant_view::State;

constexpr int kMenuFocus = 1;
constexpr int kMenuClear = 2;
constexpr long long kRepaintThrottleMs = 50;
constexpr std::size_t kMaxInputChars = 4000;
constexpr std::size_t kMaxAnswerLines = 400;
constexpr std::size_t kMaxRawAnswer = 200000;

long long SteadyMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::wstring Trim(std::wstring text) {
    const auto notSpace = [](wchar_t c) {
        return c != L' ' && c != L'\t' && c != L'\r' && c != L'\n';
    };
    const auto first = std::find_if(text.begin(), text.end(), notSpace);
    const auto last = std::find_if(text.rbegin(), text.rend(), notSpace).base();
    if (first >= last) return std::wstring();
    return std::wstring(first, last);
}

bool IsHighSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool IsLowSurrogate(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

std::wstring JoinLines(const std::vector<std::wstring>& lines) {
    std::wstring joined;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) joined.push_back(L'\n');
        joined += lines[i];
    }
    return joined;
}

// Re-wraps the raw streamed answer, keeping the model's own line breaks: a
// paragraph break stays a visual break, and cjk::Wrap does the pixel wrapping
// (Chinese breaks between any two glyphs, ASCII words stay whole).
std::vector<std::wstring> WrapAnswer(const std::wstring& raw) {
    std::vector<std::wstring> lines;
    if (raw.empty()) return lines;

    std::wstring paragraph;
    const auto flush = [&](bool blankWhenEmpty) {
        if (paragraph.empty()) {
            if (blankWhenEmpty && !lines.empty() && !lines.back().empty()) {
                lines.push_back(std::wstring());
            }
            return;
        }
        std::vector<std::wstring> part = wg::cjk::Wrap(
            paragraph, assistant_view::kTextWidth, kMaxAnswerLines,
            assistant_view::kAnswerScale);
        paragraph.clear();
        for (std::wstring& line : part) {
            if (lines.size() >= kMaxAnswerLines) break;
            lines.push_back(std::move(line));
        }
    };

    for (const wchar_t c : raw) {
        if (lines.size() >= kMaxAnswerLines) break;
        if (c == L'\r') continue;
        if (c == L'\n') {
            flush(true);
            continue;
        }
        paragraph.push_back(c);
    }
    if (lines.size() < kMaxAnswerLines) flush(false);
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

int StepLeft(const std::wstring& text, int caret) {
    if (caret <= 0) return 0;
    --caret;
    if (caret > 0 && IsLowSurrogate(text[(std::size_t)caret]) &&
        IsHighSurrogate(text[(std::size_t)(caret - 1)])) {
        --caret;
    }
    return caret;
}

int StepRight(const std::wstring& text, int caret) {
    const int size = (int)text.size();
    if (caret >= size) return size;
    const bool pair = caret + 1 < size && IsHighSurrogate(text[(std::size_t)caret]) &&
                      IsLowSurrogate(text[(std::size_t)(caret + 1)]);
    return caret + (pair ? 2 : 1);
}

class AssistantCard final : public AssistantModule {
public:
    wg::ModuleSpec Describe() const override {
        wg::ModuleSpec spec;
        spec.id = L"assistant";
        spec.title = L"ASSISTANT";
        spec.configFile = L"assistant.ini";
        // Wide 568 because answer prose and the 544px text column only read
        // well at that width; 0 seconds because the engine pushes updates and
        // the card owns no polling timer (idle cost stays 0 CPU).
        spec.sizePreset = wg::SizePreset::Wide;
        spec.refreshSeconds = 0;
        spec.hasDetails = true;
        spec.expandedByDefault = true;
        return spec;
    }

    // The engine owns the pi child process; stop it with the card so an
    // exiting desk.exe never leaves a pi/node process behind.
    ~AssistantCard() override {
        if (engine_) engine_->Stop();
    }

    void Load() override {
        std::wstring dir;
        const std::wstring& path = store().path();
        const size_t slash = path.find_last_of(L"\\/");
        dir = slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);

        engine_ = std::make_unique<assistant::Engine>();
        std::wstring error;
        if (engine_->Start(dir, &error)) {
            SetBrief(engine_->Brief());
        } else {
            SetError(error.empty() ? L"ASSISTANT START FAILED" : error);
        }
        SetAskHandler([this](const std::wstring& question) {
            if (!engine_) return;
            engine_->Ask(question, MakeCallbacks());
        });
        SetCancelHandler([this] {
            if (engine_) engine_->Cancel();
        });
    }
    // The board's worker thread. Engine::Brief() reads the DSH project files,
    // so this is the only place allowed to trigger it; the card asks for this
    // refresh with RequestRefresh() and the host posts the request into its
    // scheduler. A card that never loaded an engine (or whose Start failed)
    // has nothing to read.
    void Refresh() override {
        if (engine_) SetBrief(engine_->Brief());
    }
    void Commit() override {}

    int BodyHeight() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return assistant_view::ComputeLayout(state_, expanded(),
                                             assistant_view::kCardWidth)
            .height;
    }

    void PaintBody(wg::Canvas& canvas, const wg::Rect& body) override {
        AssistantViewState snapshot;
        {
            std::lock_guard<std::mutex> lock(mu_);
            snapshot = state_;
        }
        assistant_view::DrawBody(snapshot, canvas, body, theme_, expanded(), focused(),
                                 caretOn());
    }

    wg::StatusChip Status() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return assistant_view::Status(state_, theme_);
    }

    std::vector<wg::MenuItem> Menu() const override {
        return {wg::MenuItem{L"Focus input", kMenuFocus},
                wg::MenuItem{L"Clear answer", kMenuClear}};
    }

    void OnMenu(int id) override {
        if (id == kMenuFocus) {
            RequestFocus();
            return;
        }
        if (id == kMenuClear) {
            ClearAnswer();
            RequestRepaint();
        }
    }

    // ---- keyboard -----------------------------------------------------

    bool WantsKeyboard(int, int) const override { return true; }

    void OnFocusChanged(bool focused) override {
        // Focus is when the BRIEF is most likely stale (a DSH project changed
        // while the card sat idle); ask the host for a worker-side refresh
        // instead of reading the projects on the UI thread.
        if (focused) RequestRefresh();
        RequestRepaint();
    }

    bool OnKey(const wg::KeyEvent& key) override {
        // Ctrl+V is the board's paste path; no other chord is ours.
        if (key.ctrl) return false;
        switch (key.vk) {
        case VK_RETURN:
            Submit();
            return true;
        case VK_ESCAPE:
            Cancel();
            return true;
        case VK_BACK:
            Backspace();
            return true;
        case VK_DELETE:
            DeleteForward();
            return true;
        case VK_LEFT:
            MoveCaret(-1);
            return true;
        case VK_RIGHT:
            MoveCaret(1);
            return true;
        case VK_HOME:
        case VK_UP:
            SetCaret(0);
            return true;
        case VK_END:
        case VK_DOWN:
            SetCaretEnd();
            return true;
        default:
            return false;
        }
    }

    void OnChar(wchar_t c) override {
        if (c < 32 || c == 127) return;   // control keys arrive through OnKey
        InsertText(std::wstring(1, c));
    }

    void OnPaste(std::wstring_view text) override {
        std::wstring clean;
        clean.reserve(text.size());
        for (const wchar_t c : text) {
            if (c == L'\r') continue;
            clean.push_back(c == L'\n' ? L' ' : c);   // the input row is one line
        }
        InsertText(clean);
    }

    bool CaretPos(int* x, int* y) const override {
        std::lock_guard<std::mutex> lock(mu_);
        const assistant_view::Layout layout = assistant_view::ComputeLayout(
            state_, expanded(), assistant_view::kCardWidth);
        const assistant_view::InputGeom geom =
            assistant_view::InputGeometry(state_, assistant_view::kCardWidth);
        if (x) *x = geom.caretX;
        if (y) *y = layout.inputTop + 3;
        return true;
    }

    // ---- engine wiring -------------------------------------------------

    void SetViewState(const AssistantViewState& state) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            state_ = state;
            state_.caret = std::clamp(state_.caret, 0, (int)state_.input.size());
            answerRaw_ = JoinLines(state_.answerLines);
            // A whole snapshot is a new session: whatever was cancelled before
            // is history, so the next request's stream is accepted again.
            accepting_ = true;
        }
        RequestRepaint();
    }

    AssistantViewState ViewState() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return state_;
    }

    void AppendDelta(std::wstring_view text) override {
        bool repaint = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            // A cancelled request can still flush buffered tokens. They must
            // not resurrect THINKING (or append to the old answer) with no
            // request left to ever finish it.
            if (!accepting_) return;
            answerRaw_.append(text);
            if (answerRaw_.size() > kMaxRawAnswer) answerRaw_.erase(0, answerRaw_.size() - kMaxRawAnswer);
            state_.answerLines = WrapAnswer(answerRaw_);
            if (state_.state == State::Idle && !text.empty()) state_.state = State::Thinking;
            repaint = ShouldRepaintLocked();
        }
        if (repaint) RequestRepaint();
    }

    void FinishAnswer(const std::wstring& citation, long long inputTokens,
                      long long outputTokens, long long elapsedMs) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            // Same guard: the completion of a request the user cancelled must
            // not switch the chip to IDLE with a citation for an answer the
            // card no longer owns.
            if (!accepting_) return;
            state_.citation = citation;
            state_.inputTokens = inputTokens;
            state_.outputTokens = outputTokens;
            state_.elapsedMs = elapsedMs;
            state_.state = State::Idle;
            state_.error.clear();
        }
        RequestRepaint();
        // The answer (or a local memory command, which also completes through
        // onDone) may have changed the project state: let the host re-read the
        // BRIEF on its worker rather than reading DSH on the pi callback.
        RequestRefresh();
    }

    void SetBrief(const std::wstring& brief) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            state_.brief = brief;
        }
        RequestRepaint();
    }

    void SetError(const std::wstring& error) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            // Esc is a normal end, not a failure: errors reported by the
            // cancelled request stay out of the card.
            if (!accepting_) return;
            state_.error = error;
            state_.state = State::Error;
        }
        RequestRepaint();
    }

    void SetAskHandler(std::function<void(const std::wstring&)> handler) override {
        ask_ = std::move(handler);
    }

    void SetCancelHandler(std::function<void()> handler) override {
        cancel_ = std::move(handler);
    }

private:
    // Engine callbacks run on the pi reader/worker thread; the module mutators
    // are thread-safe by contract, so they can be forwarded directly.
    assistant::Engine::Callbacks MakeCallbacks() {
        assistant::Engine::Callbacks callbacks;
        callbacks.onDelta = [this](std::wstring_view text) { AppendDelta(text); };
        callbacks.onDone = [this](const std::wstring& citation, long long inputTokens,
                                  long long outputTokens, long long elapsedMs) {
            FinishAnswer(citation, inputTokens, outputTokens, elapsedMs);
        };
        callbacks.onError = [this](std::wstring_view error) { SetError(std::wstring(error)); };
        return callbacks;
    }

    // True at most once per throttle window; the caller repaints after the lock
    // is released so no board callback runs under the module mutex.
    bool ShouldRepaintLocked() {
        const long long now = SteadyMs();
        if (now - lastRepaintMs_ < kRepaintThrottleMs) return false;
        lastRepaintMs_ = now;
        return true;
    }

    void Submit() {
        std::wstring question;
        {
            std::lock_guard<std::mutex> lock(mu_);
            question = Trim(state_.input);
            if (question.empty()) return;
            accepting_ = true;
            state_.input.clear();
            state_.caret = 0;
            state_.state = State::Thinking;
            state_.error.clear();
            state_.answerLines.clear();
            answerRaw_.clear();
            state_.citation.clear();
            state_.inputTokens = 0;
            state_.outputTokens = 0;
            state_.elapsedMs = 0;
        }
        RequestRepaint();
        const std::function<void(const std::wstring&)> handler = ask_;
        if (handler) handler(question);
    }

    void Cancel() {
        bool thinking = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            thinking = state_.state == State::Thinking;
            if (thinking) {
                state_.state = State::Idle;
                // Drop the tail of the stream the engine may still deliver.
                accepting_ = false;
            }
        }
        if (!thinking) return;
        RequestRepaint();
        const std::function<void()> handler = cancel_;
        if (handler) handler();
    }

    void ClearAnswer() {
        std::lock_guard<std::mutex> lock(mu_);
        state_.answerLines.clear();
        answerRaw_.clear();
        state_.citation.clear();
        state_.inputTokens = 0;
        state_.outputTokens = 0;
        state_.elapsedMs = 0;
        if (state_.state != State::Idle) state_.state = State::Idle;
    }

    void InsertText(const std::wstring& text) {
        if (text.empty()) return;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (state_.input.size() >= kMaxInputChars) return;
            std::wstring& input = state_.input;
            const int caret = std::clamp(state_.caret, 0, (int)input.size());
            std::wstring insert = text;
            const std::size_t room = kMaxInputChars - input.size();
            if (insert.size() > room) insert.resize(room);
            input.insert((std::size_t)caret, insert);
            state_.caret = caret + (int)insert.size();
        }
        RequestRepaint();
    }

    void Backspace() {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const int caret = std::clamp(state_.caret, 0, (int)state_.input.size());
            if (caret > 0) {
                const int from = StepLeft(state_.input, caret);
                state_.input.erase((std::size_t)from, (std::size_t)(caret - from));
                state_.caret = from;
                changed = true;
            }
        }
        if (changed) RequestRepaint();
    }

    void DeleteForward() {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const int size = (int)state_.input.size();
            const int caret = std::clamp(state_.caret, 0, size);
            if (caret < size) {
                const int to = StepRight(state_.input, caret);
                state_.input.erase((std::size_t)caret, (std::size_t)(to - caret));
                changed = true;
            }
        }
        if (changed) RequestRepaint();
    }

    void MoveCaret(int delta) {
        std::lock_guard<std::mutex> lock(mu_);
        const int caret = std::clamp(state_.caret, 0, (int)state_.input.size());
        state_.caret = delta < 0 ? StepLeft(state_.input, caret)
                                 : StepRight(state_.input, caret);
    }

    void SetCaret(int caret) {
        std::lock_guard<std::mutex> lock(mu_);
        state_.caret = std::clamp(caret, 0, (int)state_.input.size());
    }

    void SetCaretEnd() { SetCaret((int)state_.input.size()); }

    mutable std::mutex mu_;
    AssistantViewState state_;
    // False between Esc and the next Submit/SetViewState: engine callbacks for
    // the cancelled request are dropped instead of mutating the snapshot.
    bool accepting_ = true;
    std::wstring answerRaw_;
    std::function<void(const std::wstring&)> ask_;
    std::function<void()> cancel_;
    std::unique_ptr<assistant::Engine> engine_;
    long long lastRepaintMs_ = 0;
    wg::Theme theme_;
};

} // namespace

std::unique_ptr<AssistantModule> CreateAssistantModule() {
    return std::make_unique<AssistantCard>();
}
