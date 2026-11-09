#pragma once

#include "data/server/server_data.h"

#include "widget/canvas.h"
#include "widget/store.h"

#include <windows.h>

// Headless-friendly details layout: the panel below is just a layered window
// around these two functions, so the render test can snapshot it.
int DetailsHeight(const server::ServerData& data);
void DrawDetails(const server::ServerData& data, const wg::Theme& theme, wg::Canvas& canvas);

// Read-only 440px popup: Ctrl+Alt+E or tray -> Details...
class DetailsPanel {
public:
    ~DetailsPanel();

    void Toggle(const server::ServerData& data, const wg::Theme& theme, wg::Store& store);
    void Refresh(const server::ServerData& data, const wg::Theme& theme, wg::Store& store);
    void Hide();
    bool visible() const;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void Show(const server::ServerData& data, const wg::Theme& theme, wg::Store& store);
    void Redraw(const server::ServerData& data, const wg::Theme& theme);
    void Place(wg::Store& store, const server::ServerData& data);

    HWND hwnd_ = nullptr;
    wg::Canvas canvas_;
};
