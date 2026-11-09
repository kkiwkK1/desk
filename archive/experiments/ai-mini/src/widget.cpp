// AiMini - one 440 px panel: question, streamed answer, tool trace.
//
// The widget is a single window (the old small-card + details pair is gone).
// The agent runs on the host's worker thread; each content delta is appended
// under a mutex and RequestRepaint() asks the host to paint the UI thread.
// ASCII still uses the 5x7 bitmap font, CJK goes through wg::cjk.
#include "widget/cjk.h"
#include "widget/host.h"
#include "widget/prompt.h"

#include "agent.h"
#include "apikey.h"
#include "endpoint.h"
#include "mini_data.h"
#include "text.h"
#include "tools.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 440;
constexpr int kHeight = 272;
constexpr int kTextWidth = 420;  // 10 px padding on both sides

wg::Color StatusColor(const MiniData& data, const wg::Theme& theme) {
    if (data.ok) return theme.ok;
    if (data.status == L"ASK ME") return theme.faint;
    if (data.status == L"THINKING") return theme.warn;
    return theme.bad;
}

std::wstring MetaText(const agent::Result& result) {
    wchar_t buffer[160];
    std::swprintf(buffer, 160, L"%dR %dT %.1fS %d+%d TOK", result.rounds,
                  result.toolsCalled, result.elapsedMs / 1000.0, result.usage.prompt,
                  result.usage.completion);
    return buffer;
}

std::wstring JoinTools(const std::vector<std::wstring>& names) {
    std::wstring out;
    for (const std::wstring& name : names) {
        if (out.find(name) != std::wstring::npos) continue;
        if (!out.empty()) out += L", ";
        out += name;
    }
    return text::Widen(text::Ascii(text::Narrow(out)));
}

std::vector<std::wstring> Lines(const std::wstring& text, int maxLines) {
    return wg::cjk::Wrap(text, kTextWidth, (std::size_t)maxLines, wg::kTextScale);
}

class AiMini : public wg::WidgetT<MiniData> {
public:
    wg::Desc Describe() override { return wg::Desc{L"aimini", L"AiMini", kWidth}; }

    bool HasSettings() const override { return true; }
    void OnSettings() override;

protected:
    void LoadData(MiniData& data) override {
        data.question = store().Get(L"q");
        data.answer = store().Get(L"a");
        data.meta = store().Get(L"m");
        data.tools = store().Get(L"t");
        data.status = store().Get(L"s", L"ASK ME");
        data.ok = data.status == L"READY";

        if (data.question.empty()) {
            data.status = L"ASK ME";
            data.answer = L"托盘菜单 - 设置 - 提问 AiMini。支持中文提问，也会用中文回答。";
            data.meta = L"NO QUESTION YET";
            data.tools = L"NONE";
        }
    }

    void RefreshData(MiniData& data) override {
        // The host refreshes at startup and after Settings; only a submitted
        // question should cost tokens.
        if (!pending_.exchange(false)) return;

        const std::wstring question = store().Get(L"q");
        if (question.empty()) return;

        data.question = question;
        data.status = L"THINKING";
        data.ok = false;
        data.answer.clear();
        data.tools.clear();
        data.meta.clear();

        {
            std::lock_guard<std::mutex> lock(streamMu_);
            streaming_ = true;
            streamQuestion_ = question;
            streamText_.clear();
            streamTools_.clear();
        }
        RequestRepaint();

        const std::wstring key = aimini::LoadApiKey();
        if (key.empty()) {
            StopStream();
            Fail(data, L"NO KEY", L"SET ONE FROM THE TRAY MENU.");
            return;
        }

        llm::HttpTransport transport(key);
        const tools::Registry registry = tools::LocalTools();

        aimini::Endpoint endpoint = aimini::FromEnv();
        const std::wstring iniEndpoint = store().Get(L"endpoint");
        if (!iniEndpoint.empty()) endpoint = aimini::ParseEndpoint(iniEndpoint, endpoint.model);
        const std::wstring iniModel = store().Get(L"model");
        if (!iniModel.empty()) endpoint.model = text::Narrow(iniModel);

        agent::Config config;
        config.host = endpoint.host;
        config.path = endpoint.path;
        config.model = endpoint.model;
        config.maxRounds = (int)store().GetInt(L"rounds", 4);

        std::vector<std::wstring> used;
        const agent::Result result = agent::Run(
            text::Narrow(question), transport, registry, config,
            [this, &used](const agent::Step& step) {
                if (step.kind != "tool") return;
                used.push_back(text::Widen(step.name));
                {
                    std::lock_guard<std::mutex> lock(streamMu_);
                    streamTools_ = JoinTools(used);
                }
                RequestRepaint();
            },
            [this](const std::string& delta) {
                if (delta.empty()) return;
                const std::wstring piece = text::Widen(delta);
                {
                    std::lock_guard<std::mutex> lock(streamMu_);
                    streamText_ += piece;
                }
                RequestRepaint();
            });

        StopStream();

        if (!result.ok) {
            Fail(data, text::Widen(result.error), L"CHECK KEY OR NETWORK, THEN ASK AGAIN.");
            return;
        }

        data.ok = true;
        data.status = L"READY";
        data.answer = text::Widen(result.answer);
        data.tools = JoinTools(used);
        data.meta = MetaText(result);
        Save(data);
    }

    int HeightOf(const MiniData&) const override { return kHeight; }

    void PaintData(const MiniData& data, wg::Canvas& canvas) override {
        const Snapshot snapshot = Snap(data);
        const bool streaming = snapshot.streaming;

        canvas.Panel(theme_, editing());
        canvas.Text(L"AI MINI", 10.0f, 8.0f, theme_.label, 2.0f);

        std::wstring status = streaming
                                  ? (snapshot.answer.empty() ? L"THINKING" : L"STREAMING")
                                  : data.status;
        if (status.size() > 18) status = status.substr(0, 16) + L"..";
        const wg::Color statusColor = streaming ? theme_.warn : StatusColor(data, theme_);
        const float statusWidth = wg::cjk::TextWidth(status, 1.5f);
        wg::cjk::Text(canvas, status, 430.0f - statusWidth, 11.0f, statusColor, 1.5f);
        canvas.HLine(10, 430, 34, theme_.border);

        canvas.Text(L"QUESTION", 10.0f, 42.0f, theme_.faint, 1.0f);
        int y = 54;
        for (const std::wstring& line : Lines(snapshot.question, 2)) {
            wg::cjk::Text(canvas, line, 10.0f, (float)y, theme_.label, wg::kTextScale);
            y += 15;
        }

        canvas.Text(L"ANSWER", 10.0f, 94.0f, theme_.faint, 1.0f);
        const std::vector<std::wstring> answerLines = Lines(snapshot.answer, 8);
        y = 106;
        for (const std::wstring& line : answerLines) {
            wg::cjk::Text(canvas, line, 10.0f, (float)y, theme_.text, wg::kTextScale);
            y += 15;
        }

        if (streaming) {
            const int lastY = 106 + (int)(answerLines.size() - 1) * 15;
            const float endX = 10.0f + wg::cjk::TextWidth(answerLines.back(), wg::kTextScale);
            canvas.Fill((int)endX + 3, lastY + 2, 7, 10, theme_.ok);
        }

        canvas.HLine(10, 430, 236, theme_.border);
        canvas.Text(L"TOOLS", 10.0f, 244.0f, theme_.faint, 1.0f);
        const std::wstring tools = snapshot.tools.empty() ? L"NONE" : snapshot.tools;
        canvas.Text(tools.c_str(), 52.0f, 244.0f, theme_.label, 1.0f);

        const std::wstring meta = streaming ? L"" : data.meta;
        canvas.Text(meta.c_str(), 10.0f, 258.0f, theme_.faint, 1.0f);
        canvas.TextRight(L"CTRL+ALT+SHIFT+W", 430, 258.0f, theme_.faint, 1.0f);
    }

private:
    struct Snapshot {
        std::wstring question;
        std::wstring answer;
        std::wstring tools;
        bool streaming = false;
    };

    Snapshot Snap(const MiniData& data) {
        std::lock_guard<std::mutex> lock(streamMu_);
        if (streaming_) return Snapshot{streamQuestion_, streamText_, streamTools_, true};
        return Snapshot{data.question, data.answer, data.tools, false};
    }

    void StopStream() {
        {
            std::lock_guard<std::mutex> lock(streamMu_);
            streaming_ = false;
            streamText_.clear();
            streamQuestion_.clear();
            streamTools_.clear();
        }
        // The host renders on commit/failure; repainting here would flash an
        // empty answer between the last delta and the committed frame.
    }

    void Fail(MiniData& data, const std::wstring& status, const std::wstring& hint) {
        data.ok = false;
        data.status = status;
        data.answer = status + L". " + hint;
        data.tools = L"NONE";
        data.meta.clear();
        Save(data);
    }

    void Save(const MiniData& data) {
        store().Set(L"a", data.answer);
        store().Set(L"m", data.meta);
        store().Set(L"t", data.tools);
        store().Set(L"s", data.status);
        store().Save();
    }

    void Ask() {
        wg::PromptSpec spec;
        spec.label = L"ASK AIMINI";
        spec.hint = L"ENTER ASK   ESC CANCEL";
        spec.initial = store().Get(L"q");
        spec.width = 460;

        std::wstring question;
        if (!wg::PromptText(spec, question)) return;

        store().Set(L"q", question);
        store().Save();
        pending_ = true;  // StartRefresh() runs right after OnSettings()
    }

    void SetKey() {
        wg::PromptSpec spec;
        spec.label = L"API KEY";
        spec.hint = L"ENTER SAVE   ESC CANCEL";
        spec.mask = true;

        std::wstring key;
        if (!wg::PromptText(spec, key)) return;
        if (!aimini::SaveApiKey(key)) {
            MessageBoxW(nullptr, L"Could not save the key in Credential Manager.", L"AiMini",
                        MB_OK | MB_ICONWARNING);
        }
    }

    wg::Theme theme_;
    std::atomic<bool> pending_{false};
    std::mutex streamMu_;
    bool streaming_ = false;
    std::wstring streamQuestion_;
    std::wstring streamText_;
    std::wstring streamTools_;
};

void AiMini::OnSettings() {
    const bool hasKey = !aimini::LoadApiKey().empty();

    // "--settings --ask" skips the menu and opens the question prompt. Handy
    // for launchers and for the end-to-end checks in tools/.
    if (hasKey && wcsstr(GetCommandLineW(), L"--ask")) {
        Ask();
        return;
    }

    int choice = 0;
    if (!hasKey) {
        choice = 2;  // nothing works without a key; ask for it first
    } else {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"Ask AiMini...");
        AppendMenuW(menu, MF_STRING, 2, L"Set API key...");

        POINT pt{};
        GetCursorPos(&pt);
        HWND owner = GetForegroundWindow();
        if (owner) SetForegroundWindow(owner);
        const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y,
                                           0, owner, nullptr);
        DestroyMenu(menu);
        if (command == 0) return;
        choice = command;
    }

    if (choice == 2) {
        SetKey();
        if (hasKey) return;
        if (aimini::LoadApiKey().empty()) return;
        choice = 1;
    }

    if (choice == 1) Ask();
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wg::Options options;
    options.refreshSeconds = 0;  // no periodic calls: each ask costs tokens
    options.startWithWindows = false;

    // AiMeter owns Ctrl+Alt+W/D/Q; stay out of its way when both widgets run.
    options.editHotkeyMods = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    options.editHotkeyVk = 'W';
    options.quitHotkeyMods = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    options.quitHotkeyVk = 'Q';

    return wg::Run(std::make_unique<AiMini>(), options);
}
