// Server Status widget: one row per SSH host, CPU / memory / disk / load /
// uptime read over the user's existing OpenSSH config. No agent required.
#include "data/server/collect.h"
#include "details.h"
#include "data/server/providers.h"
#include "data/server/server_data.h"

#include "widget/host.h"
#include "widget/prompt.h"
#include "widget/view.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

class ServerWidget : public wg::WidgetT<server::ServerData> {
public:
    wg::Desc Describe() override {
        return wg::Desc{L"server", L"Server Status", server::kPanelWidth};
    }

    bool HasSettings() const override { return true; }

    void OnSettings() override {
        const std::wstring current =
            server::JoinHosts(server::ParseHosts(store().Get(L"hosts")));

        wg::PromptSpec spec;
        spec.label = L"SSH HOSTS";
        spec.hint = L"WEB-01, DB-01   TYPE - TO CLEAR";
        spec.initial = current;
        spec.saved = !current.empty();
        spec.width = 420;

        std::wstring input;
        if (!wg::PromptText(spec, input)) return;

        if (input == L"-") store().Set(L"hosts", L"");
        else store().Set(L"hosts", server::JoinHosts(server::ParseHosts(input)));
        store().Save();
    }

    bool HasDetails() const override { return true; }
    void ShowDetails() override { details_.Toggle(shown(), theme_, store()); }

    bool TrayIcon(wg::Canvas& canvas) override {
        for (int i = 0; i < 3; ++i) {
            const int y = 2 + i * 4;
            canvas.Fill(3, y, 10, 3, theme_.border);
            canvas.Fill(4, y + 1, 6, 1, theme_.label);
            canvas.Fill(11, y + 1, 1, 1, theme_.ok);
        }
        return true;
    }

protected:
    void LoadData(server::ServerData& data) override {
        server::Load(data, store(), theme_);
    }

    void RefreshData(server::ServerData& data) override {
        server::Refresh(data, store(), theme_);
    }

    int HeightOf(const server::ServerData& data) const override {
        return wg::ListHeight(style_, std::max<size_t>(1, data.rows.size()));
    }

    void PaintData(const server::ServerData& data, wg::Canvas& canvas) override {
        wg::DrawList(canvas, data.rows, style_, theme_, editing());
    }

    void OnCommit() override {
        if (details_.visible()) details_.Refresh(shown(), theme_, store());
    }

private:
    DetailsPanel details_;
    wg::ListStyle style_;
    wg::Theme theme_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wg::Options options;
    options.refreshSeconds = 30;

    // Distinct from AiMeter's Ctrl+Alt+W / D / Q so both widgets can run.
    options.editHotkeyVk = 'S';
    options.detailsHotkeyVk = 'E';
    options.quitHotkeyMods = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    options.quitHotkeyVk = 'Q';

    return wg::Run(std::make_unique<ServerWidget>(), options);
}
