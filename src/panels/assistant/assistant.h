// ASSISTANT card (Wide 568) for M1.0: input row, streaming answer, citation
// footer, BRIEF block and the IDLE/THINKING/ERROR title chip.
//
// The card owns its view snapshot (AssistantViewState) so the UI and its
// rendering test do not depend on the pi/engine headers: PaintBody reads only
// that snapshot. The engine wiring (Lead, src/data/assistant/engine.*) pushes
// updates through AssistantModule - SetViewState / AppendDelta / FinishAnswer /
// SetBrief / SetError - and receives Enter/Esc through SetAskHandler /
// SetCancelHandler.
//
// BRIEF: Engine::Brief() reads the user's DSH projects, so it is only ever
// called from Refresh() (the board's worker thread), never from a pi callback
// or the UI thread. The card asks the host for that refresh through
// wg::Module::RequestRefresh() (host lane: thread-safe post into the board's
// dispatcher) after focus, after an answer and after a local memory command;
// the finished text arrives back through SetBrief().
//
// Layout: ComputeLayout() is the single source of truth. BodyHeight() and
// DrawBody() both go through it, so the card can never declare a height that
// clips what it paints; there is no second copy of the maths to drift.
#pragma once

#include "widget/canvas.h"
#include "widget/geom.h"
#include "widget/module.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace assistant_view {

// Wide preset width. The board hands PaintBody exactly this wide a body, so
// BodyHeight() and CaretPos() need no Rect to do their geometry.
constexpr int kCardWidth = 568;
constexpr int kPadX = 12;
constexpr int kTextWidth = kCardWidth - kPadX * 2;   // 544

// Answer prose scale: ASCII 5x7 at 1.5 (9px advance) mixes with the fixed
// 14px CJK mask, which is what cjk::TextTop draws without clipping.
constexpr float kAnswerScale = 1.5f;
constexpr float kMetaScale = 1.0f;

// Vertical rhythm. kBaseH is everything except the wrapped line counts, so a
// height is always kBaseH + briefLines*14 + answerLines*15.
constexpr int kTopInset = 2;
constexpr int kBriefHeadH = 12;
constexpr int kBriefLineH = 14;
constexpr int kDividerH = 7;
constexpr int kAnswerHeadH = 13;
constexpr int kAnswerLineH = 15;
constexpr int kCitationH = 15;
constexpr int kInputH = 22;
constexpr int kBottomPad = 5;
constexpr int kBaseH = kTopInset + kBriefHeadH + kDividerH + kAnswerHeadH + kCitationH +
                       kInputH + kBottomPad;   // 76
// Collapsed reserves exactly one BRIEF line and two ANSWER lines so the card
// does not jump while the question is typed or the answer streams in:
// 76 + 14 + 30 = 120 body -> 168 outer, the Wide preset minimum.
constexpr int kCollapsedBriefLines = 1;
constexpr int kCollapsedAnswerLines = 2;
constexpr int kCollapsedHeight =
    kBaseH + kCollapsedBriefLines * kBriefLineH + kCollapsedAnswerLines * kAnswerLineH;
// Expanded caps: wide preset is 168..720 outer, i.e. 120..672 body.
// BRIEF is allowed up to 6 lines (the engine's project + last conclusion +
// blocker text), which still leaves 32 answer lines inside 672:
// 76 + 6*14 + 32*15 = 640. One line collapsed keeps the preset minimum.
constexpr int kExpandedMaxBriefLines = 6;
constexpr int kExpandedMaxAnswerLines = 32;
constexpr int kExpandedMinAnswerLines = 4;
constexpr int kWideMaxBody = 720 - 48;   // 672

// One-letter state of the card, mirrored into the title chip.
enum class State { Idle, Thinking, Error };

struct AssistantViewState {
    State state = State::Idle;
    std::wstring input;                     // single line, caret is a char index
    std::vector<std::wstring> answerLines;  // already wrapped for kTextWidth
    std::wstring citation;                  // verified source ref; empty = none
    std::wstring brief;                     // projects + last conclusion + blocker
    std::wstring footer;                    // free-form meta, e.g. model name
    std::wstring error;                     // shown instead of the answer
    long long inputTokens = 0;
    long long outputTokens = 0;
    long long elapsedMs = 0;
    int caret = 0;                          // caret index into input
};

// Offsets are body-local (0 = top of the body rect); DrawBody adds body.x/y.
struct Layout {
    int height = 0;
    int briefTop = 0;
    int briefTextTop = 0;
    int briefLines = 0;
    int dividerTop = 0;
    int answerTop = 0;
    int answerTextTop = 0;
    int answerLines = 0;
    int citationTop = 0;
    int inputTop = 0;
};

// 'width' defaults to the Wide body width; only BRIEF wrapping depends on it,
// because answerLines arrive pre-wrapped.
Layout ComputeLayout(const AssistantViewState& state, bool expanded,
                     int width = kCardWidth);

int BodyHeight(const AssistantViewState& state, bool expanded);

void DrawBody(const AssistantViewState& state, wg::Canvas& canvas, const wg::Rect& body,
              const wg::Theme& theme, bool expanded, bool focused, bool caretOn);

wg::StatusChip Status(const AssistantViewState& state, const wg::Theme& theme);

} // namespace assistant_view

// UI-side surface for the engine wiring. Every mutator is thread-safe: the
// engine may stream deltas from its worker thread. The handlers, by contrast,
// are UI-thread callbacks.
class AssistantModule : public wg::Module {
public:
    // Replace the whole snapshot (e.g. after a resume or a failed request).
    virtual void SetViewState(const assistant_view::AssistantViewState& state) = 0;
    virtual assistant_view::AssistantViewState ViewState() const = 0;

    // Streaming: append raw model text, re-wrap it for the card and ask for a
    // repaint. Repaints are throttled so a fast token stream cannot flood the
    // UI message queue (idle cards still cost 0 CPU: no timer, no polling).
    virtual void AppendDelta(std::wstring_view text) = 0;
    // Answer finished: attach the verified citation and the cost line, and
    // switch the chip back to IDLE.
    virtual void FinishAnswer(const std::wstring& citation, long long inputTokens,
                              long long outputTokens, long long elapsedMs) = 0;
    virtual void SetBrief(const std::wstring& brief) = 0;
    virtual void SetError(const std::wstring& error) = 0;

    // Enter in the input row hands the stripped question over; Esc asks the
    // engine to abort. Both run on the UI thread.
    virtual void SetAskHandler(std::function<void(const std::wstring&)> handler) = 0;
    virtual void SetCancelHandler(std::function<void()> handler) = 0;
};

std::unique_ptr<AssistantModule> CreateAssistantModule();
