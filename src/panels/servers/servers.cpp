#include "servers.h"

#include "servers_view.h"

#include "data/server/collect.h"
#include "data/server/providers.h"

#include "widget/prompt.h"

#include <memory>
#include <string>

namespace {

class ServersModule : public wg::ModuleT<server::ServerData> {
public:
    wg::ModuleSpec Describe() const override {
        wg::ModuleSpec spec;
        spec.id = L"servers";
        spec.title = L"SERVERS";
        spec.configFile = L"server.ini";
        spec.sizePreset = wg::SizePreset::Standard;
        spec.refreshSeconds = 30;
        spec.hasDetails = true;
        return spec;
    }

    std::vector<wg::MenuItem> Menu() const override {
        return {wg::MenuItem{L"SSH Hosts...", 1}};
    }

    void OnMenu(int id) override {
        if (id != 1) return;

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

protected:
    void LoadData(server::ServerData& data) override {
        server::Load(data, store(), theme_);
    }

    void RefreshData(server::ServerData& data) override {
        server::Refresh(data, store(), theme_);
    }

    int BodyHeightOf(const server::ServerData& data) const override {
        return servers_view::BodyHeight(data, expanded());
    }

    void PaintBodyOf(const server::ServerData& data, wg::Canvas& canvas,
                     const wg::Rect& body) override {
        servers_view::DrawBody(data, canvas, body, theme_, expanded());
    }

    wg::StatusChip Status() const override {
        // expanded() decides how many rows fit, so a limited dataset reports
        // "+N" only when the taller card is the one on screen.
        return servers_view::Status(shown(), theme_, expanded());
    }

private:
    wg::Theme theme_;
};

} // namespace

std::unique_ptr<wg::Module> CreateServersModule() {
    return std::make_unique<ServersModule>();
}
