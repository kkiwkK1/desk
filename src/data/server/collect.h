// Shared Server Status collection, used by the standalone widget and the
// Servers card. One implementation keeps both views and srvctl consistent.
#pragma once

#include "server_data.h"

#include "widget/canvas.h"
#include "widget/store.h"

namespace server {

void Load(ServerData& data, wg::Store& store, const wg::Theme& theme);
void Refresh(ServerData& data, wg::Store& store, const wg::Theme& theme);

} // namespace server
