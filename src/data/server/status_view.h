// Main-panel row layout for Server Status. Kept out of the widget so the
// headless test can render exactly what the desktop shows.
#pragma once

#include "server_data.h"

#include "widget/view.h"

namespace server {

std::vector<wg::Row> BuildRows(const ServerData& data, const wg::Theme& theme);

} // namespace server
