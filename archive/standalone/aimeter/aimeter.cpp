// AiMeter on top of the widget framework: DeepSeek balance + local token usage
// + Codex quotas.
#include "data/aimeter/aimeter_data.h"
#include "data/aimeter/collect.h"
#include "data/aimeter/commandcode.h"
#include "details.h"

#include "widget/host.h"
#include "widget/prompt.h"
#include "widget/secrets.h"
#include "widget/view.h"

#include <cwchar>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const wchar_t* kCredTarget = L"AiMeter/DeepSeek";
constexpr const wchar_t* kCredUser = L"deepseek";
constexpr const wchar_t* kCcCredTarget = L"AiMeter/CommandCode";
constexpr const wchar_t* kCcCredUser = L"commandcode";

class Aimeter : public wg::WidgetT<aimeter::AimeterData> {
public:
    wg::Desc Describe() override { return wg::Desc{L"aimeter", L"AiMeter", 280}; }

    bool HasSettings() const override { return true; }

    void OnSettings() override {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"DeepSeek API Key...");
        AppendMenuW(menu, MF_STRING, 2, L"CommandCode API Key...");

        POINT pt{};
        GetCursorPos(&pt);
        HWND owner = GetForegroundWindow();
        if (!owner) owner = GetShellWindow();
        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                                       owner, nullptr);
        DestroyMenu(menu);

        if (cmd == 1) {
            PromptKey(kCredTarget, kCredUser, L"DEEPSEEK API KEY",
                      !wg::LoadSecret(kCredTarget).empty());
        } else if (cmd == 2) {
            PromptKey(kCcCredTarget, kCcCredUser, L"COMMANDCODE API KEY",
                      !aimeter::LoadCommandCodeKey().empty());
        }
    }

    bool HasDetails() const override { return true; }

    void ShowDetails() override {
        details_.Toggle(shown(), theme_, store(), HeightOf(shown()));
    }

    bool TrayIcon(wg::Canvas& canvas) override {
        const wg::Color bars[3] = {theme_.ok, theme_.warn, theme_.bad};
        const int heights[3] = {5, 9, 13};
        for (int i = 0; i < 3; ++i) {
            canvas.Fill(3 + i * 4, 14 - heights[i], 2, heights[i] + 1, bars[i]);
        }
        return true;
    }

protected:
    void LoadData(aimeter::AimeterData& data) override {
        data.rows = aimeter::InitialRows();
    }

    void RefreshData(aimeter::AimeterData& data) override {
        aimeter::Refresh(data, store(), theme_);
    }

    int HeightOf(const aimeter::AimeterData& data) const override {
        return wg::ListHeight(style_, data.rows.size());
    }

    void PaintData(const aimeter::AimeterData& data, wg::Canvas& canvas) override {
        wg::DrawList(canvas, data.rows, style_, theme_, editing());
    }

    void OnCommit() override {
        if (details_.visible()) details_.Refresh(shown(), theme_);
    }

private:
    void PromptKey(const wchar_t* target, const wchar_t* user, const wchar_t* label,
                   bool saved) {
        wg::PromptSpec spec;
        spec.label = label;
        spec.mask = true;
        spec.saved = saved;

        std::wstring key;
        if (wg::PromptText(spec, key)) wg::SaveSecret(target, user, key);
    }

    DetailsPanel details_;
    wg::ListStyle style_;
    wg::Theme theme_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return wg::Run(std::make_unique<Aimeter>());
}
