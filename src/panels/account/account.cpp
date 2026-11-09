#include "account.h"

#include "account_view.h"

#include "data/aimeter/collect.h"
#include "data/aimeter/commandcode.h"

#include "widget/prompt.h"
#include "widget/secrets.h"

#include <memory>
#include <string>

namespace {

class AccountModule : public wg::ModuleT<aimeter::AimeterData> {
public:
    wg::ModuleSpec Describe() const override {
        wg::ModuleSpec spec;
        spec.id = L"account";
        spec.title = L"ACCOUNT";
        spec.configFile = L"aimeter.ini";
        spec.sizePreset = wg::SizePreset::Standard;
        spec.refreshSeconds = 300;
        spec.hasDetails = true;
        return spec;
    }

    std::vector<wg::MenuItem> Menu() const override {
        return {
            wg::MenuItem{L"DeepSeek API Key...", 1},
            wg::MenuItem{L"CommandCode API Key...", 2},
        };
    }

    void OnMenu(int id) override {
        if (id == 1) {
            PromptKey(L"AiMeter/DeepSeek", L"deepseek", L"DEEPSEEK API KEY",
                      !wg::LoadSecret(L"AiMeter/DeepSeek").empty());
        } else if (id == 2) {
            PromptKey(L"AiMeter/CommandCode", L"commandcode", L"COMMANDCODE API KEY",
                      !aimeter::LoadCommandCodeKey().empty());
        }
    }

protected:
    void LoadData(aimeter::AimeterData& data) override {
        data.rows = aimeter::InitialRows();
    }

    void RefreshData(aimeter::AimeterData& data) override {
        aimeter::Refresh(data, store(), theme_);
    }

    int BodyHeightOf(const aimeter::AimeterData& data) const override {
        return account_view::BodyHeight(data, expanded());
    }

    void PaintBodyOf(const aimeter::AimeterData& data, wg::Canvas& canvas,
                     const wg::Rect& body) override {
        account_view::DrawBody(data, canvas, body, theme_, expanded());
    }

    wg::StatusChip Status() const override {
        return account_view::Status(shown(), theme_);
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

    wg::Theme theme_;
};

} // namespace

std::unique_ptr<wg::Module> CreateAccountModule() {
    return std::make_unique<AccountModule>();
}
