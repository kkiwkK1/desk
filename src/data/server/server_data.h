#pragma once

#include "providers.h"
#include "widget/view.h"

#include <string>
#include <vector>

namespace server {

constexpr int kPanelWidth = 300;

// One immutable snapshot, produced by RefreshData on the worker thread.
struct ServerData {
    std::vector<wg::Row> rows;          // main panel
    std::vector<HostStatus> hosts;      // details panel / source of rows
    bool sshFound = false;
    std::wstring sshPath;
    std::wstring updated;               // "HH:MM"
};

} // namespace server
